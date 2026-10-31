#include "pxpch.h"
#include "Paralax/Asset/MeshImporter.h"

#include "Paralax/Asset/AssetManager.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace Paralax {

	namespace {

		uint64_t SplitMix64(uint64_t value)
		{
			value += 0x9E3779B97F4A7C15ull;
			value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
			value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
			return value ^ (value >> 31);
		}

		// Upper bound on accessor element counts (also bounds zero-filled accessors, which have no backing bytes).
		constexpr size_t s_MaxElements = size_t(1) << 24;
		constexpr size_t s_MaxNodeDepth = 256;

		size_t GetBufferSize(const fastgltf::Buffer& buffer)
		{
			return std::visit(fastgltf::visitor{
								  [](const fastgltf::sources::Array& array) -> size_t { return array.bytes.size_bytes(); },
								  [](const fastgltf::sources::Vector& vector) -> size_t { return vector.bytes.size(); },
								  [](const fastgltf::sources::ByteView& view) -> size_t { return view.bytes.size(); },
								  [](const auto&) -> size_t { return 0; },
							  },
							  buffer.data);
		}

		// Returns the encoded bytes of an image, or an empty vector if the source is unsupported.
		std::vector<uint8_t> GetImageBytes(const fastgltf::Asset& asset, const fastgltf::Image& image, const std::filesystem::path& directory, std::string& outProblem)
		{
			std::vector<uint8_t> bytes;
			std::visit(fastgltf::visitor{
						   [&](const fastgltf::sources::Array& array) {
							   const auto* data = reinterpret_cast<const uint8_t*>(array.bytes.data());
							   bytes.assign(data, data + array.bytes.size_bytes());
						   },
						   [&](const fastgltf::sources::Vector& vector) {
							   const auto* data = reinterpret_cast<const uint8_t*>(vector.bytes.data());
							   bytes.assign(data, data + vector.bytes.size());
						   },
						   [&](const fastgltf::sources::BufferView& view) {
							   if (view.bufferViewIndex >= asset.bufferViews.size() || asset.bufferViews[view.bufferViewIndex].bufferIndex >= asset.buffers.size())
							   {
								   outProblem = "image buffer view does not exist";
								   return;
							   }
							   const fastgltf::BufferView& bufferView = asset.bufferViews[view.bufferViewIndex];
							   const size_t bufferSize = GetBufferSize(asset.buffers[bufferView.bufferIndex]);
							   if (bufferView.byteOffset > bufferSize || bufferView.byteLength > bufferSize - bufferView.byteOffset)
							   {
								   outProblem = "image buffer view exceeds its buffer's data";
								   return;
							   }
							   const auto span = fastgltf::DefaultBufferDataAdapter{}(asset, view.bufferViewIndex);
							   const auto* data = reinterpret_cast<const uint8_t*>(span.data());
							   bytes.assign(data, data + span.size());
						   },
						   [&](const fastgltf::sources::URI& uri) {
							   if (!uri.uri.isLocalPath())
							   {
								   outProblem = "image URI is not a local file";
								   return;
							   }
							   // Image files must stay inside the model's directory (no "../" or absolute escapes).
							   std::error_code error;
							   const std::filesystem::path base = std::filesystem::weakly_canonical(directory, error);
							   const std::filesystem::path target = std::filesystem::weakly_canonical(directory / uri.uri.fspath(), error);
							   const std::filesystem::path relative = target.lexically_relative(base);
							   if (error || relative.empty() || *relative.begin() == ".." || uri.uri.fspath().is_absolute())
							   {
								   outProblem = fmt::format("image path '{}' leaves the model's directory", uri.uri.fspath().string());
								   return;
							   }
							   std::ifstream file(target, std::ios::binary);
							   if (!file)
							   {
								   outProblem = fmt::format("image file '{}' not found", uri.uri.fspath().string());
								   return;
							   }
							   bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
						   },
						   [&](const auto&) { outProblem = "unsupported image source"; },
					   },
					   image.data);
			return bytes;
		}

		// Checks that reading the accessor stays within the bytes actually present (not just the declared lengths).
		bool ValidateAccessor(const fastgltf::Asset& asset, size_t index, std::string& outProblem)
		{
			if (index >= asset.accessors.size())
			{
				outProblem = fmt::format("accessor {} does not exist", index);
				return false;
			}
			const fastgltf::Accessor& accessor = asset.accessors[index];
			if (accessor.sparse.has_value())
			{
				outProblem = fmt::format("accessor {} is sparse (unsupported)", index);
				return false;
			}
			if (accessor.count > s_MaxElements)
			{
				outProblem = fmt::format("accessor {} has too many elements ({})", index, accessor.count);
				return false;
			}
			if (!accessor.bufferViewIndex.has_value() || accessor.count == 0)
				return true; // Zero-filled (bounded by s_MaxElements) or empty.

			if (*accessor.bufferViewIndex >= asset.bufferViews.size())
			{
				outProblem = fmt::format("accessor {} references a missing buffer view", index);
				return false;
			}
			const fastgltf::BufferView& view = asset.bufferViews[*accessor.bufferViewIndex];
			if (view.bufferIndex >= asset.buffers.size())
			{
				outProblem = fmt::format("buffer view of accessor {} references a missing buffer", index);
				return false;
			}
			const size_t bufferSize = GetBufferSize(asset.buffers[view.bufferIndex]);
			if (view.byteOffset > bufferSize || view.byteLength > bufferSize - view.byteOffset)
			{
				outProblem = fmt::format("buffer view of accessor {} exceeds its buffer's data", index);
				return false;
			}
			const size_t elementSize = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
			const size_t stride = view.byteStride ? *view.byteStride : elementSize;
			if (elementSize == 0 || stride < elementSize)
			{
				outProblem = fmt::format("accessor {} has an invalid layout", index);
				return false;
			}
			const size_t lastByte = accessor.byteOffset + (accessor.count - 1) * stride + elementSize; // count <= 2^24: no overflow.
			if (lastByte > view.byteLength)
			{
				outProblem = fmt::format("accessor {} reads past its buffer view", index);
				return false;
			}
			return true;
		}

	}

	AssetHandle DeriveSubAssetHandle(AssetHandle parent, SubAssetKind kind, uint64_t index)
	{
		uint64_t value = SplitMix64(static_cast<uint64_t>(parent) ^ SplitMix64(static_cast<uint64_t>(kind) * 0x100000001B3ull + index));
		// Never collide with the reserved built-in range.
		while (value < AssetManager::FirstUserHandle)
			value = SplitMix64(value);
		return AssetHandle(value);
	}

	bool MeshImporter::Import(const std::filesystem::path& path, AssetHandle meshHandle, MeshImportResult& outResult, std::string& outError)
	{
		outResult = MeshImportResult();
		outError.clear();

		auto data = fastgltf::GltfDataBuffer::FromPath(path);
		if (data.error() != fastgltf::Error::None)
		{
			outError = fmt::format("cannot read '{}': {}", path.string(), fastgltf::getErrorMessage(data.error()));
			return false;
		}

		fastgltf::Parser parser(fastgltf::Extensions::KHR_materials_emissive_strength | fastgltf::Extensions::KHR_texture_transform);
		const auto options = fastgltf::Options::LoadExternalBuffers | fastgltf::Options::GenerateMeshIndices;
		auto loaded = parser.loadGltf(data.get(), path.parent_path(), options);
		if (loaded.error() != fastgltf::Error::None)
		{
			outError = fmt::format("cannot parse '{}': {}", path.string(), fastgltf::getErrorMessage(loaded.error()));
			return false;
		}
		const fastgltf::Asset& asset = loaded.get();
		const std::filesystem::path directory = path.parent_path();

		// fastgltf only validates on export; reject structurally invalid documents before touching any data.
		if (const fastgltf::Error validation = fastgltf::validate(asset); validation != fastgltf::Error::None)
		{
			outError = fmt::format("'{}' is not a valid glTF file: {}", path.string(), fastgltf::getErrorMessage(validation));
			return false;
		}

		// Textures: one TextureAsset per (glTF texture, color space) pair actually used by materials.
		std::unordered_map<uint64_t, AssetHandle> textureHandles; // key: textureIndex * 2 + srgb
		auto importTexture = [&](size_t textureIndex, bool srgb) -> AssetHandle {
			const uint64_t key = static_cast<uint64_t>(textureIndex) * 2 + (srgb ? 1 : 0);
			if (auto it = textureHandles.find(key); it != textureHandles.end())
				return it->second;

			AssetHandle handle = 0;
			if (textureIndex < asset.textures.size() && asset.textures[textureIndex].imageIndex.has_value() && *asset.textures[textureIndex].imageIndex < asset.images.size())
			{
				std::string problem;
				const std::vector<uint8_t> bytes = GetImageBytes(asset, asset.images[*asset.textures[textureIndex].imageIndex], directory, problem);
				Ref<TextureAsset> texture = bytes.empty() ? nullptr : TextureAsset::LoadFromMemory(bytes.data(), bytes.size(), srgb);
				if (texture)
				{
					handle = DeriveSubAssetHandle(meshHandle, SubAssetKind::Texture, key);
					texture->Handle = handle;
					outResult.Textures.push_back(texture);
				}
				else
				{
					outResult.Warnings.push_back(fmt::format("texture {}: {}", textureIndex, problem.empty() ? "could not be decoded" : problem));
				}
			}
			else
			{
				outResult.Warnings.push_back(fmt::format("texture {} has no image", textureIndex));
			}
			textureHandles[key] = handle;
			return handle;
		};

		// Materials (one slot per glTF material, plus a default slot for primitives without one).
		for (size_t i = 0; i < asset.materials.size(); i++)
		{
			const fastgltf::Material& source = asset.materials[i];
			auto material = CreateRef<MaterialAsset>();
			material->Handle = DeriveSubAssetHandle(meshHandle, SubAssetKind::Material, i);

			const auto& pbr = source.pbrData;
			material->BaseColorFactor = glm::vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
			material->MetallicFactor = pbr.metallicFactor;
			material->RoughnessFactor = pbr.roughnessFactor;
			if (pbr.baseColorTexture)
				material->BaseColorTexture = importTexture(pbr.baseColorTexture->textureIndex, true);
			if (pbr.metallicRoughnessTexture)
				material->MetallicRoughnessTexture = importTexture(pbr.metallicRoughnessTexture->textureIndex, false);
			if (source.normalTexture)
			{
				material->NormalTexture = importTexture(source.normalTexture->textureIndex, false);
				material->NormalScale = source.normalTexture->scale;
			}
			if (source.occlusionTexture)
			{
				material->OcclusionTexture = importTexture(source.occlusionTexture->textureIndex, false);
				material->OcclusionStrength = source.occlusionTexture->strength;
			}
			if (source.emissiveTexture)
				material->EmissiveTexture = importTexture(source.emissiveTexture->textureIndex, true);
			material->EmissiveFactor = glm::vec3(source.emissiveFactor[0], source.emissiveFactor[1], source.emissiveFactor[2]);
			material->EmissiveStrength = source.emissiveStrength;
			material->Alpha = source.alphaMode == fastgltf::AlphaMode::Blend  ? AlphaMode::Blend
							  : source.alphaMode == fastgltf::AlphaMode::Mask ? AlphaMode::Mask
																			  : AlphaMode::Opaque;
			material->AlphaCutoff = source.alphaCutoff;
			material->DoubleSided = source.doubleSided;
			outResult.Materials.push_back(material);
		}

		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = meshHandle;
		for (const auto& material : outResult.Materials)
			mesh->Materials.push_back(material->Handle);
		const uint32_t defaultMaterialSlot = static_cast<uint32_t>(mesh->Materials.size());
		bool usesDefaultMaterial = false;

		const size_t sceneIndex = asset.defaultScene.value_or(0);
		if (asset.scenes.empty() || sceneIndex >= asset.scenes.size())
		{
			outError = fmt::format("'{}' contains no usable scene", path.string());
			return false;
		}

		auto processNode = [&](const fastgltf::Node& node, const fastgltf::math::fmat4x4& matrix) {
			if (!node.meshIndex.has_value() || *node.meshIndex >= asset.meshes.size())
				return;

			glm::mat4 transform;
			std::memcpy(glm::value_ptr(transform), matrix.data(), sizeof(float) * 16);
			const fastgltf::Mesh& sourceMesh = asset.meshes[*node.meshIndex];

			for (size_t p = 0; p < sourceMesh.primitives.size(); p++)
			{
				const fastgltf::Primitive& primitive = sourceMesh.primitives[p];
				if (primitive.type != fastgltf::PrimitiveType::Triangles)
				{
					outResult.Warnings.push_back(fmt::format("mesh '{}' primitive {}: only triangle lists are supported", sourceMesh.name.c_str(), p));
					continue;
				}

				const auto* positionAttribute = primitive.findAttribute("POSITION");
				if (positionAttribute == primitive.attributes.end() || !primitive.indicesAccessor.has_value())
				{
					outResult.Warnings.push_back(fmt::format("mesh '{}' primitive {}: missing positions or indices", sourceMesh.name.c_str(), p));
					continue;
				}

				// Every accessor this primitive reads must stay within real data.
				std::string problem;
				bool accessorsValid = ValidateAccessor(asset, positionAttribute->accessorIndex, problem) && ValidateAccessor(asset, *primitive.indicesAccessor, problem);
				for (const char* name : {"NORMAL", "TANGENT", "TEXCOORD_0"})
				{
					if (const auto* attribute = primitive.findAttribute(name); accessorsValid && attribute != primitive.attributes.end())
						accessorsValid = ValidateAccessor(asset, attribute->accessorIndex, problem);
				}
				if (!accessorsValid)
				{
					outError = fmt::format("mesh '{}' primitive {}: {}", sourceMesh.name.c_str(), p, problem);
					return;
				}
				if (asset.accessors[*primitive.indicesAccessor].count % 3 != 0)
				{
					outResult.Warnings.push_back(fmt::format("mesh '{}' primitive {}: index count is not a multiple of 3; skipped", sourceMesh.name.c_str(), p));
					continue;
				}
				if (mesh->Vertices.size() + asset.accessors[positionAttribute->accessorIndex].count > s_MaxElements * 4 || mesh->Indices.size() + asset.accessors[*primitive.indicesAccessor].count > s_MaxElements * 4)
				{
					outError = fmt::format("'{}' is too large to import", path.string());
					return;
				}

				Submesh submesh;
				submesh.Name = sourceMesh.name.empty() ? fmt::format("Mesh{}", *node.meshIndex) : std::string(sourceMesh.name.c_str());
				submesh.Transform = transform;
				submesh.VertexOffset = static_cast<uint32_t>(mesh->Vertices.size());
				submesh.IndexOffset = static_cast<uint32_t>(mesh->Indices.size());

				const fastgltf::Accessor& positions = asset.accessors[positionAttribute->accessorIndex];
				submesh.VertexCount = static_cast<uint32_t>(positions.count);
				mesh->Vertices.resize(mesh->Vertices.size() + positions.count);
				MeshVertex* vertices = mesh->Vertices.data() + submesh.VertexOffset;

				fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, positions, [&](glm::vec3 value, size_t index) { vertices[index].Position = value; });

				bool hasNormals = false;
				if (const auto* attribute = primitive.findAttribute("NORMAL"); attribute != primitive.attributes.end())
				{
					hasNormals = asset.accessors[attribute->accessorIndex].count == positions.count;
					if (hasNormals)
						fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, asset.accessors[attribute->accessorIndex], [&](glm::vec3 value, size_t index) { vertices[index].Normal = value; });
				}
				bool hasTangents = false;
				if (const auto* attribute = primitive.findAttribute("TANGENT"); attribute != primitive.attributes.end())
				{
					hasTangents = asset.accessors[attribute->accessorIndex].count == positions.count;
					if (hasTangents)
						fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, asset.accessors[attribute->accessorIndex], [&](glm::vec4 value, size_t index) { vertices[index].Tangent = value; });
				}
				if (const auto* attribute = primitive.findAttribute("TEXCOORD_0"); attribute != primitive.attributes.end())
				{
					if (asset.accessors[attribute->accessorIndex].count == positions.count)
						fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, asset.accessors[attribute->accessorIndex], [&](glm::vec2 value, size_t index) { vertices[index].UV = value; });
				}

				const fastgltf::Accessor& indexAccessor = asset.accessors[*primitive.indicesAccessor];
				submesh.IndexCount = static_cast<uint32_t>(indexAccessor.count);
				mesh->Indices.resize(mesh->Indices.size() + indexAccessor.count);
				fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, mesh->Indices.data() + submesh.IndexOffset);

				for (uint32_t i = 0; i < submesh.IndexCount; i++)
				{
					if (mesh->Indices[submesh.IndexOffset + i] >= submesh.VertexCount)
					{
						outError = fmt::format("mesh '{}' primitive {} has an out-of-range index", submesh.Name, p);
						return;
					}
				}

				if (!hasNormals)
				{
					// Flat normals from face orientation (accumulated, so shared vertices are smoothed).
					for (uint32_t i = 0; i < submesh.VertexCount; i++)
						vertices[i].Normal = glm::vec3(0.0f);
					for (uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3)
					{
						const uint32_t* tri = &mesh->Indices[submesh.IndexOffset + i];
						const glm::vec3 n = glm::cross(vertices[tri[1]].Position - vertices[tri[0]].Position, vertices[tri[2]].Position - vertices[tri[0]].Position);
						vertices[tri[0]].Normal += n;
						vertices[tri[1]].Normal += n;
						vertices[tri[2]].Normal += n;
					}
					for (uint32_t i = 0; i < submesh.VertexCount; i++)
					{
						const float length = glm::length(vertices[i].Normal);
						vertices[i].Normal = length > 0.0f ? vertices[i].Normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
					}
				}
				if (!hasTangents)
					ComputeTangents(mesh->Vertices, mesh->Indices, submesh.IndexOffset, submesh.IndexCount, submesh.VertexOffset, submesh.VertexCount);

				if (primitive.materialIndex.has_value() && *primitive.materialIndex < outResult.Materials.size())
				{
					submesh.MaterialIndex = static_cast<uint32_t>(*primitive.materialIndex);
				}
				else
				{
					submesh.MaterialIndex = defaultMaterialSlot;
					usesDefaultMaterial = true;
				}
				mesh->Submeshes.push_back(std::move(submesh));
			}
		};

		// Iterative scene traversal with a visited set (glTF node graphs must be trees; cycles or shared nodes in
		// malformed files are ignored) and a depth limit.
		struct PendingNode
		{
			size_t Index;
			fastgltf::math::fmat4x4 Parent;
			size_t Depth;
		};
		std::vector<bool> visited(asset.nodes.size(), false);
		std::vector<PendingNode> stack;
		const auto& roots = asset.scenes[sceneIndex].nodeIndices;
		for (auto it = roots.rbegin(); it != roots.rend(); ++it)
			stack.push_back({*it, fastgltf::math::fmat4x4(), 0});
		while (!stack.empty() && outError.empty())
		{
			const PendingNode pending = stack.back();
			stack.pop_back();
			if (pending.Index >= asset.nodes.size() || visited[pending.Index])
			{
				outResult.Warnings.push_back(fmt::format("node {} is invalid or referenced more than once; skipped", pending.Index));
				continue;
			}
			if (pending.Depth > s_MaxNodeDepth)
			{
				outError = fmt::format("'{}' has a node hierarchy deeper than {}", path.string(), s_MaxNodeDepth);
				break;
			}
			visited[pending.Index] = true;

			const fastgltf::Node& node = asset.nodes[pending.Index];
			const fastgltf::math::fmat4x4 matrix = fastgltf::getTransformMatrix(node, pending.Parent);
			processNode(node, matrix);
			for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
				stack.push_back({*it, matrix, pending.Depth + 1});
		}

		if (!outError.empty())
			return false;
		if (mesh->Submeshes.empty())
		{
			outError = fmt::format("'{}' contains no triangle meshes", path.string());
			return false;
		}
		if (usesDefaultMaterial)
			mesh->Materials.push_back(0); // Slot rendered with the engine's default material.

		mesh->ComputeBounds();
		std::string validationError;
		if (!mesh->Validate(validationError))
		{
			outError = fmt::format("'{}' produced an invalid mesh: {}", path.string(), validationError);
			return false;
		}

		outResult.Mesh = mesh;
		return true;
	}

}
