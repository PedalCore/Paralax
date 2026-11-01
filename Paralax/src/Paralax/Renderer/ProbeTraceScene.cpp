#include "pxpch.h"
#include "Paralax/Renderer/ProbeTraceScene.h"

#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Asset/MaterialAsset.h"
#include "Paralax/Asset/MeshAsset.h"
#include "Paralax/Asset/TextureAsset.h"
#include "Paralax/Scene/Entity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace Paralax {

	namespace {

		bool IsFinite(const glm::vec4& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		bool IsBounded(const glm::vec4& value, float maximum)
		{
			return IsFinite(value) && glm::all(glm::lessThanEqual(glm::abs(value), glm::vec4(maximum)));
		}

		float ToLinear(float value)
		{
			return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
		}

	}

	bool ProbeTraceScene::Update(Scene& scene, AssetManager& assets, bool& changed)
	{
		changed = false;
		struct Instance
		{
			Ref<MeshAsset> Mesh;
			const Submesh* Part;
			Ref<MaterialAsset> Material;
			glm::mat4 World;
		};
		std::vector<Entity> entities;
		for (auto handle : scene.GetAllEntitiesWith<MeshComponent>())
			entities.emplace_back(handle, &scene);
		std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return uint64_t(a.GetUUID()) < uint64_t(b.GetUUID()); });
		std::vector<Instance> instances;
		std::vector<uint64_t> snapshot{reinterpret_cast<uintptr_t>(&scene)};
		auto remember = [&](const Ref<Asset>& asset) {
			snapshot.push_back(asset ? uint64_t(asset->Handle) : 0);
			snapshot.push_back(asset ? asset->Version : 0);
			snapshot.push_back(reinterpret_cast<uintptr_t>(asset.get()));
		};
		for (Entity entity : entities)
		{
			const auto& component = entity.GetComponent<MeshComponent>();
			auto mesh = assets.GetAsset<MeshAsset>(component.Mesh);
			if (!mesh)
				continue;
			snapshot.push_back(entity.GetUUID());
			remember(mesh);
			const glm::mat4 world = scene.GetWorldTransform(entity);
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					snapshot.push_back(std::bit_cast<uint32_t>(world[column][row]));
			for (const Submesh& part : mesh->Submeshes)
			{
				AssetHandle materialHandle = 0;
				if (part.MaterialIndex < component.Materials.size() && component.Materials[part.MaterialIndex].IsValid())
					materialHandle = component.Materials[part.MaterialIndex];
				else if (part.MaterialIndex < mesh->Materials.size())
					materialHandle = mesh->Materials[part.MaterialIndex];
				auto material = materialHandle.IsValid() ? assets.GetAsset<MaterialAsset>(materialHandle) : nullptr;
				if (!material)
					material = assets.GetAsset<MaterialAsset>(AssetHandle(AssetManager::BuiltinDefaultMaterial));
				remember(material);
				for (AssetHandle texture : {material->BaseColorTexture, material->EmissiveTexture, material->MetallicRoughnessTexture})
					remember(texture.IsValid() ? assets.GetAsset<TextureAsset>(texture) : nullptr);
				if (material->Alpha != AlphaMode::Blend)
					instances.push_back({mesh, &part, material, world * part.Transform});
			}
		}
		if (snapshot == m_Snapshot)
			return true;
		if (snapshot == m_RejectedSnapshot)
			return false;

		std::vector<GPU::ProbeTraceTriangle> triangles;
		std::vector<GPU::ProbeTraceMaterial> materials;
		std::vector<glm::vec4> texels{glm::vec4(1.0f)};
		std::unordered_map<const MaterialAsset*, uint32_t> materialIndices;
		std::unordered_map<const TextureAsset*, glm::uvec4> textureIndices;
		std::unordered_set<const TextureAsset*> maskTextures;
		for (const auto& instance : instances)
			if (instance.Material->Alpha == AlphaMode::Mask && instance.Material->BaseColorTexture.IsValid())
				maskTextures.insert(assets.GetAsset<TextureAsset>(instance.Material->BaseColorTexture).get());
		bool valid = true;
		const char* failure = "invalid transformed vertex or material";
		auto appendTexture = [&](AssetHandle handle) -> glm::uvec4 {
			auto texture = handle.IsValid() ? assets.GetAsset<TextureAsset>(handle) : nullptr;
			if (!texture)
				return {0, 1, 1, 0};
			if (auto found = textureIndices.find(texture.get()); found != textureIndices.end())
				return found->second;
			const uint32_t stride = texture->GetBytesPerPixel();
			const uint64_t sourceCount = uint64_t(texture->Width) * texture->Height;
			if (sourceCount == 0 || texture->Width > TextureAsset::MaxDimension || texture->Height > TextureAsset::MaxDimension ||
				(texture->Format != TextureFormat::RGBA8 && texture->Format != TextureFormat::RGBA32F) ||
				sourceCount > TextureAsset::MaxDecodedBytes / stride || texture->Mips.empty() || sourceCount * stride != texture->Mips[0].size())
			{
				valid = false;
				failure = "texture size, mip data or texel budget";
				return {};
			}
			// Diffuse radiance is low frequency. Reuse authored mips to bound the ray material cache, but retain
			// full-resolution alpha cutouts: blurring their coverage would change occlusion through thin openings.
			uint32_t width = texture->Width, height = texture->Height;
			size_t mip = 0;
			while (!maskTextures.contains(texture.get()) && std::max(width, height) > 512 && mip + 1 < texture->Mips.size())
			{
				width = std::max(1u, width / 2);
				height = std::max(1u, height / 2);
				++mip;
			}
			const uint64_t count = uint64_t(width) * height;
			if (count > MaxTexels - texels.size() || count * stride != texture->Mips[mip].size())
			{
				valid = false;
				failure = "texture mip data or texel budget";
				return {};
			}
			const glm::uvec4 descriptor(static_cast<uint32_t>(texels.size()), width, height, 0);
			for (size_t i = 0; i < count; ++i)
			{
				glm::vec4 pixel;
				if (texture->Format == TextureFormat::RGBA32F)
					std::memcpy(&pixel, texture->Mips[mip].data() + i * stride, sizeof(pixel));
				else
				{
					const uint8_t* bytes = texture->Mips[mip].data() + i * stride;
					pixel = glm::vec4(bytes[0], bytes[1], bytes[2], bytes[3]) / 255.0f;
					if (texture->SRGB)
						pixel = {ToLinear(pixel.r), ToLinear(pixel.g), ToLinear(pixel.b), pixel.a};
				}
				if (!IsBounded(pixel, MaxMaterialValue))
				{
					valid = false;
					failure = "nonfinite or excessive texture values";
					return {};
				}
				texels.push_back(pixel);
			}
			textureIndices.emplace(texture.get(), descriptor);
			return descriptor;
		};
		for (const Instance& instance : instances)
		{
			const auto& part = *instance.Part;
			const auto& mesh = *instance.Mesh;
			const auto& material = *instance.Material;
			if (part.IndexCount % 3 != 0 || uint64_t(part.IndexOffset) + part.IndexCount > mesh.Indices.size() ||
				part.IndexCount / 3 > MaxTriangles - triangles.size())
			{
				valid = false;
				failure = "submesh indices or triangle budget";
				break;
			}
			uint32_t materialIndex;
			if (auto found = materialIndices.find(&material); found != materialIndices.end())
				materialIndex = found->second;
			else
			{
				materialIndex = static_cast<uint32_t>(materials.size());
				GPU::ProbeTraceMaterial data;
				data.BaseColor = material.BaseColorFactor;
				data.Emissive = glm::vec4(material.EmissiveFactor * material.EmissiveStrength, material.DoubleSided ? 1.0f : 0.0f);
				data.Parameters = {material.MetallicFactor, material.RoughnessFactor, material.AlphaCutoff, static_cast<float>(material.Alpha)};
				data.Textures[0] = appendTexture(material.BaseColorTexture);
				data.Textures[1] = appendTexture(material.EmissiveTexture);
				data.Textures[2] = appendTexture(material.MetallicRoughnessTexture);
				valid &= IsBounded(data.BaseColor, MaxMaterialValue) && IsBounded(data.Emissive, MaxMaterialValue) && IsBounded(data.Parameters, MaxMaterialValue);
				materials.push_back(data);
				materialIndices.emplace(&material, materialIndex);
			}
			const float determinant = glm::determinant(glm::mat3(instance.World));
			// Zero scale collapses the surface; it has no area and cannot occlude or reflect light.
			if (std::isfinite(determinant) && std::abs(determinant) < 1e-12f)
				continue;
			const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(instance.World)));
			for (uint32_t offset = 0; valid && offset < part.IndexCount; offset += 3)
			{
				GPU::ProbeTraceTriangle triangle;
				std::array<glm::vec2, 3> uv;
				for (uint32_t corner = 0; corner < 3; ++corner)
				{
					const uint64_t index = uint64_t(mesh.Indices[static_cast<size_t>(part.IndexOffset) + offset + corner]) + part.VertexOffset;
					if (index >= mesh.Vertices.size())
					{
						valid = false;
						failure = "vertex index outside mesh";
						break;
					}
					const auto& vertex = mesh.Vertices[static_cast<size_t>(index)];
					triangle.Position[corner] = instance.World * glm::vec4(vertex.Position, 1.0f);
					triangle.Normal[corner] = glm::vec4(normalMatrix * vertex.Normal, 0.0f);
					uv[corner] = vertex.UV;
					valid &= IsBounded(triangle.Position[corner], MaxCoordinate) && IsBounded(triangle.Normal[corner], MaxCoordinate) &&
							 IsBounded(glm::vec4(uv[corner], 0.0f, 0.0f), MaxCoordinate);
				}
				if (!valid)
					break;
				// Preserve the authored front face under reflected transforms, matching raster culling.
				if (determinant < 0.0f)
				{
					std::swap(triangle.Position[1], triangle.Position[2]);
					std::swap(triangle.Normal[1], triangle.Normal[2]);
					std::swap(uv[1], uv[2]);
				}
				triangle.UV01 = {uv[0].x, uv[0].y, uv[1].x, uv[1].y};
				triangle.UV2 = {uv[2].x, uv[2].y, 0.0f, 0.0f};
				triangle.Material.x = materialIndex;
				triangles.push_back(triangle);
			}
			if (!valid)
				break;
		}
		if (!valid)
		{
			m_RejectedSnapshot = std::move(snapshot);
			PX_CORE_ERROR("Probe GI scene rejected: {} ({} triangles, {} texels)", failure, triangles.size(), texels.size());
			return false;
		}

		std::vector<GPU::ProbeTraceNode> nodes;
		nodes.reserve(triangles.size());
		// Median splitting bounds recursion to 20 levels at MaxTriangles, independently of scene hierarchy depth.
		auto build = [&](auto&& self, uint32_t first, uint32_t count) -> void {
			const uint32_t index = static_cast<uint32_t>(nodes.size());
			GPU::ProbeTraceNode node;
			node.Minimum = glm::vec4(std::numeric_limits<float>::max());
			node.Maximum = glm::vec4(std::numeric_limits<float>::lowest());
			for (uint32_t i = first; i < first + count; ++i)
				for (const auto& position : triangles[i].Position)
				{
					node.Minimum = glm::min(node.Minimum, position);
					node.Maximum = glm::max(node.Maximum, position);
				}
			nodes.push_back(node);
			if (count <= 4)
				nodes[index].Range = {first, count, index + 1, 0};
			else
			{
				const glm::vec3 extent(node.Maximum - node.Minimum);
				const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1
																										 : 2;
				std::stable_sort(triangles.begin() + first, triangles.begin() + first + count, [axis](const auto& a, const auto& b) {
					return (a.Position[0][axis] + a.Position[1][axis] + a.Position[2][axis]) <
						   (b.Position[0][axis] + b.Position[1][axis] + b.Position[2][axis]);
				});
				self(self, first, count / 2);
				self(self, first + count / 2, count - count / 2);
				nodes[index].Range.z = static_cast<uint32_t>(nodes.size());
			}
		};
		if (!triangles.empty())
			build(build, 0, static_cast<uint32_t>(triangles.size()));
		m_Snapshot = std::move(snapshot);
		m_RejectedSnapshot.clear();
		m_Nodes = std::move(nodes);
		m_Triangles = std::move(triangles);
		m_Materials = std::move(materials);
		m_Texels = std::move(texels);
		changed = true;
		return true;
	}

}
