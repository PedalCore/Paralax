#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

using namespace Paralax;

namespace {

	constexpr uint32_t Width = 192, Height = 128;
	constexpr std::array<uint32_t, 3> Left{16, 80, 144};
	constexpr std::array<uint32_t, 6> QuadIndices{0, 1, 2, 0, 2, 3};
	constexpr std::array<uint32_t, 3> VertexOffsets{5, 13, 26}, IndexOffsets{3, 12, 24};
	struct Extent
	{
		const char* Name;
		uint32_t VertexStride, IndexStride;
		bool White = false;
		uint32_t VertexPrefix = 0;
	};
	constexpr std::array Extents{
		Extent{"compact", 4, 6}, Extent{"vertex-extent", 12160, 6},
		Extent{"index-extent", 4, 12288}, Extent{"combined-extent", 12160, 12288},
		Extent{"boundary-65535", 4, 6, false, 65535}, Extent{"boundary-65536", 4, 6, false, 65536}, Extent{"boundary-65537", 4, 6, false, 65537},
		Extent{"combined-white", 12160, 12288, true}};

	enum class Representation
	{
		Local,
		Global,
		Separate
	};
	struct Transition
	{
		const char* Name;
		bool Blend, Text;
		std::vector<std::string> PrecedingCases{};
	};

	struct OracleResult
	{
		uint32_t InteriorErrors = 0, BackgroundErrors = 0, InteriorSamples = 0, BackgroundSamples = 0;
		std::vector<uint32_t> CenterErrors = std::vector<uint32_t>(3, 0);
	};

	// Pixel regions are specified independently of mesh projection and draw representation.
	OracleResult Inspect(const Image& image, uint32_t shift, const Extent* extent = nullptr, bool footer = false)
	{
		const uint32_t width = extent ? 360 : Width, height = extent ? 280 : Height;
		REQUIRE(image.GetWidth() == width);
		REQUIRE(image.GetHeight() == height + (footer ? 40 : 0));
		OracleResult result;
		result.CenterErrors.resize(extent ? 63 : 3, 0);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
			{
				int interior = -1;
				bool edge = false;
				const uint32_t first = extent ? (y / 40) * 9 + x / 40 : 0;
				const uint32_t end = extent ? first + 1 : 3;
				for (uint32_t quad = first; quad < end; ++quad)
				{
					const uint32_t left = (extent ? (quad % 9) * 40 + 8 : Left[quad]) + shift;
					const uint32_t top = extent ? (quad / 9) * 40 + 8 : 40;
					const uint32_t right = left + (extent ? 24 : 32), bottom = top + (extent ? 24 : 48);
					if (x >= left + 2 && x < right - 2 && y >= top + 2 && y < bottom - 2)
						interior = static_cast<int>(quad);
					else if (x >= left - 2 && x < right + 2 && y >= top - 2 && y < bottom + 2)
						edge = true;
				}
				if (interior < 0 && edge)
					continue;
				const auto pixel = image.GetPixel(x, y);
				bool wrong = false;
				for (int channel = 0; channel < 3; ++channel)
				{
					int expected = channel == interior ? 255 : 0;
					if (extent && interior >= 0)
					{
						constexpr std::array<int, 4> encoded{0, 137, 188, 255};
						expected = extent->White ? 255 : encoded[(static_cast<uint32_t>(interior + 1) >> (2 * channel)) & 3u];
					}
					wrong |= std::abs(int(pixel[channel]) - expected) > 1;
				}
				if (interior >= 0)
				{
					++result.InteriorSamples;
					result.InteriorErrors += wrong ? 1u : 0u;
					const uint32_t quad = static_cast<uint32_t>(interior);
					const uint32_t center = (extent ? (quad % 9) * 40 + 20 : Left[quad] + 16) + shift;
					const uint32_t centerY = extent ? (quad / 9) * 40 + 20 : 64;
					if (x >= center - 2 && x <= center + 2 && y >= centerY - 2 && y <= centerY + 2)
						result.CenterErrors[static_cast<size_t>(interior)] += wrong ? 1u : 0u;
				}
				else
				{
					++result.BackgroundSamples;
					result.BackgroundErrors += wrong ? 1u : 0u;
				}
			}
		return result;
	}

	std::array<MeshVertex, 4> QuadVertices(uint32_t quad, bool grid = false)
	{
		std::array<MeshVertex, 4> vertices;
		const float left = grid ? float((quad % 9) * 40 + 8) / 180 - 1 : float(Left[quad]) / 96 - 1;
		const float right = grid ? float((quad % 9) * 40 + 32) / 180 - 1 : float(Left[quad] + 32) / 96 - 1;
		const float bottom = grid ? 1 - float((quad / 9) * 40 + 32) / 140 : -0.375f;
		const float top = grid ? 1 - float((quad / 9) * 40 + 8) / 140 : 0.375f;
		const std::array<glm::vec3, 4> positions{{{left, bottom, -2}, {right, bottom, -2}, {right, top, -2}, {left, top, -2}}};
		for (size_t i = 0; i < vertices.size(); ++i)
		{
			vertices[i].Position = positions[i];
			vertices[i].Normal = {0, 0, 1};
			vertices[i].Tangent = {1, 0, 0, 1};
		}
		return vertices;
	}

	std::string HashText(uint64_t hash)
	{
		std::ostringstream text;
		text << std::hex << std::setfill('0') << std::setw(16) << hash;
		return text.str();
	}

	nlohmann::json WordManifest(size_t count, const auto& getWord)
	{
		uint64_t hash = 14695981039346656037ull;
		nlohmann::json samples = nlohmann::json::array();
		for (size_t i = 0; i < count; ++i)
		{
			const uint32_t word = getWord(i);
			for (uint32_t byte = 0; byte < 4; ++byte)
				hash = (hash ^ ((word >> (byte * 8)) & 255u)) * 1099511628211ull;
			if (count <= 4096 || i < 48 || i + 48 >= count || i % std::max<size_t>(1, count / 128) == 0)
				samples.push_back({i, word});
		}
		return {{"count", count}, {"hash", HashText(hash)}, {"completeWords", count <= 4096}, {"samples", samples}};
	}

	nlohmann::json MeshManifest(const MeshAsset& mesh)
	{
		const auto vertices = WordManifest(mesh.Vertices.size() * 12, [&](size_t index) {
			const auto& vertex = mesh.Vertices[index / 12];
			const std::array fields{vertex.Position.x, vertex.Position.y, vertex.Position.z, vertex.Normal.x, vertex.Normal.y, vertex.Normal.z,
									vertex.Tangent.x, vertex.Tangent.y, vertex.Tangent.z, vertex.Tangent.w, vertex.UV.x, vertex.UV.y};
			return std::bit_cast<uint32_t>(fields[index % 12]);
		});
		nlohmann::json manifest = {{"handle", uint64_t(mesh.Handle)}, {"version", mesh.Version}, {"vertexCount", mesh.Vertices.size()}, {"indexCount", mesh.Indices.size()}, {"vertexBytes", mesh.Vertices.size() * sizeof(MeshVertex)}, {"indexBytes", mesh.Indices.size() * sizeof(uint32_t)}, {"hashEncoding", "FNV1a64 over little-endian uint32 field words; position/normal/tangent/UV, no host padding; large arrays have bounded samples"}, {"vertices", vertices}, {"indices", WordManifest(mesh.Indices.size(), [&](size_t index) { return mesh.Indices[index]; })}, {"submeshes", nlohmann::json::array()}};
		for (const auto& submesh : mesh.Submeshes)
		{
			uint32_t minimum = UINT32_MAX, maximum = 0;
			for (uint32_t i = 0; i < submesh.IndexCount; ++i)
			{
				const auto index = mesh.Indices[submesh.IndexOffset + i];
				minimum = std::min(minimum, index);
				maximum = std::max(maximum, index);
			}
			manifest["submeshes"].push_back({{"vertexOffset", submesh.VertexOffset}, {"vertexCount", submesh.VertexCount}, {"indexOffset", submesh.IndexOffset}, {"indexCount", submesh.IndexCount}, {"materialSlot", submesh.MaterialIndex}, {"indexRange", {minimum, maximum}}, {"effectiveRange", {minimum + submesh.VertexOffset, maximum + submesh.VertexOffset}}, {"boundsMin", {submesh.Bounds.Min.x, submesh.Bounds.Min.y, submesh.Bounds.Min.z}}, {"boundsMax", {submesh.Bounds.Max.x, submesh.Bounds.Max.y, submesh.Bounds.Max.z}}});
		}
		return manifest;
	}

	glm::vec4 QuadColor(uint32_t quad, const Extent* extent)
	{
		glm::vec4 color(0, 0, 0, 1);
		if (!extent)
			color[quad] = 1;
		else
			for (uint32_t channel = 0; channel < 3; ++channel)
			{
				constexpr std::array<float, 4> levels{0, 0.25f, 0.5f, 1};
				color[channel] = extent->White ? 1 : levels[((quad + 1) >> (2 * channel)) & 3u];
			}
		return color;
	}

	std::vector<Ref<MeshAsset>> MakeMeshes(Representation representation, AssetManager& assets, Scene& scene, const Extent* extent = nullptr)
	{
		const uint32_t quadCount = extent ? 63 : 3;
		const bool separate = representation == Representation::Separate;
		std::vector<AssetHandle> materials;
		for (uint32_t quad = 0; quad < (extent && extent->White ? 1 : quadCount); ++quad)
		{
			auto material = CreateRef<MaterialAsset>();
			material->Handle = AssetHandle(71000 + quad);
			material->DoubleSided = true;
			material->BaseColorFactor = QuadColor(quad, extent);
			assets.AddMemoryAsset(material);
			materials.push_back(material->Handle);
		}
		std::vector<Ref<MeshAsset>> meshes;
		const uint32_t count = separate ? quadCount : 1u;
		for (uint32_t meshIndex = 0; meshIndex < count; ++meshIndex)
		{
			auto mesh = CreateRef<MeshAsset>();
			mesh->Handle = AssetHandle(72000 + meshIndex);
			mesh->Materials = materials;
			mesh->Vertices.resize(separate ? 4 : extent ? extent->VertexPrefix + quadCount * extent->VertexStride
														: 30);
			mesh->Indices.resize(separate ? 6 : extent ? quadCount * extent->IndexStride
													   : 30,
								 0);
			for (auto& vertex : mesh->Vertices)
				vertex.Position = {20, 20, -2};
			for (uint32_t quad = 0; quad < quadCount; ++quad)
			{
				if (separate && quad != meshIndex)
					continue;
				const uint32_t vertexOffset = separate ? 0 : extent ? extent->VertexPrefix + quad * extent->VertexStride
																	: VertexOffsets[quad];
				const uint32_t indexOffset = separate ? 0 : extent ? quad * extent->IndexStride
																   : IndexOffsets[quad];
				const auto vertices = QuadVertices(quad, extent != nullptr);
				for (uint32_t i = 0; i < 4; ++i)
					mesh->Vertices[vertexOffset + i] = vertices[i];
				for (uint32_t i = 0; i < 6; ++i)
					mesh->Indices[indexOffset + i] = QuadIndices[i] + (representation == Representation::Global ? vertexOffset : 0);
				Submesh submesh;
				submesh.VertexOffset = representation == Representation::Global ? 0 : vertexOffset;
				submesh.VertexCount = representation == Representation::Global ? static_cast<uint32_t>(mesh->Vertices.size()) : 4;
				submesh.IndexOffset = indexOffset;
				submesh.IndexCount = 6;
				submesh.MaterialIndex = extent && extent->White ? 0 : quad;
				mesh->Submeshes.push_back(submesh);
			}
			std::string error;
			REQUIRE_MESSAGE(mesh->Validate(error), error);
			mesh->ComputeBounds();
			assets.AddMemoryAsset(mesh);
			scene.CreateEntityWithUUID(UUID(73000 + meshIndex), "Analytic quads").AddComponent<MeshComponent>().Mesh = mesh->Handle;
			meshes.push_back(mesh);
		}
		return meshes;
	}

	MeshImportResult ImportGrid(const Extent& extent, const char* scope)
	{
		const auto folder = Testing::GetTestOutputDirectory() / "MeshOffsets" / extent.Name / scope;
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		REQUIRE_FALSE(error);
		std::ofstream binary(folder / "Grid.bin", std::ios::binary);
		REQUIRE(binary);
		size_t bytes = 0;
		const auto writeWord = [&](uint32_t word) {
			std::array<char, 4> encoded;
			for (uint32_t i = 0; i < 4; ++i)
				encoded[i] = static_cast<char>((word >> (8 * i)) & 255u);
			binary.write(encoded.data(), 4);
			bytes += 4;
		};
		nlohmann::json document = {{"asset", {{"version", "2.0"}}}, {"scene", 0}, {"scenes", {{{"nodes", {0}}}}}, {"nodes", {{{"mesh", 0}}}}, {"bufferViews", nlohmann::json::array()}, {"accessors", nlohmann::json::array()}, {"materials", nlohmann::json::array()}};
		nlohmann::json primitives = nlohmann::json::array();
		for (uint32_t quad = 0; quad < 63; ++quad)
		{
			const auto vertices = QuadVertices(quad, true);
			const auto color = QuadColor(quad, &extent);
			document["materials"].push_back({{"doubleSided", true}, {"pbrMetallicRoughness", {{"baseColorFactor", {color.r, color.g, color.b, color.a}}}}});
			const size_t vertexStart = bytes;
			for (uint32_t i = 0; i < extent.VertexStride; ++i)
			{
				const glm::vec3 position = i < 4 ? vertices[i].Position : glm::vec3(20, 20, -2);
				for (float field : std::array{position.x, position.y, position.z, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f})
					writeWord(std::bit_cast<uint32_t>(field));
			}
			const size_t vertexView = document["bufferViews"].size();
			document["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", vertexStart}, {"byteLength", bytes - vertexStart}, {"byteStride", 48}});
			const size_t accessor = document["accessors"].size();
			for (const auto& [offset, type] : std::array{std::pair{0, "VEC3"}, std::pair{12, "VEC3"}, std::pair{24, "VEC4"}, std::pair{40, "VEC2"}})
			{
				nlohmann::json entry = {{"bufferView", vertexView}, {"byteOffset", offset}, {"componentType", 5126}, {"count", extent.VertexStride}, {"type", type}};
				if (offset == 0)
				{
					entry["min"] = {vertices[0].Position.x, vertices[0].Position.y, -2};
					entry["max"] = extent.VertexStride > 4 ? nlohmann::json{20, 20, -2} : nlohmann::json{vertices[2].Position.x, vertices[2].Position.y, -2};
				}
				document["accessors"].push_back(entry);
			}
			const size_t indexStart = bytes;
			for (uint32_t index : QuadIndices)
				writeWord(index);
			document["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", indexStart}, {"byteLength", bytes - indexStart}});
			document["accessors"].push_back({{"bufferView", vertexView + 1}, {"componentType", 5125}, {"count", 6}, {"type", "SCALAR"}});
			primitives.push_back({{"attributes", {{"POSITION", accessor}, {"NORMAL", accessor + 1}, {"TANGENT", accessor + 2}, {"TEXCOORD_0", accessor + 3}}},
								  {"indices", accessor + 4},
								  {"material", quad},
								  {"mode", 4}});
		}
		binary.close();
		REQUIRE_FALSE(binary.fail());
		document["buffers"] = {{{"uri", "Grid.bin"}, {"byteLength", bytes}}};
		document["meshes"] = {{{"name", "Analytic grid"}, {"primitives", primitives}}};
		std::ofstream file(folder / "Grid.gltf");
		REQUIRE(file);
		file << document.dump();
		file.close();
		REQUIRE_FALSE(file.fail());
		MeshImportResult imported;
		std::string importError;
		REQUIRE_MESSAGE(MeshImporter::Import(folder / "Grid.gltf", AssetHandle(75000), imported, importError), importError);
		REQUIRE(imported.Mesh);
		REQUIRE(imported.NodeMeshes.size() == 1);
		REQUIRE(imported.Materials.size() == 63);
		CHECK(imported.Warnings.empty());
		for (const auto& mesh : {imported.Mesh, imported.NodeMeshes.front()})
		{
			REQUIRE(mesh->Submeshes.size() == 63);
			REQUIRE(mesh->Vertices.size() == 63 * extent.VertexStride);
			REQUIRE(mesh->Indices.size() == 63 * 6);
			REQUIRE(mesh->Materials.size() == 63);
			uint32_t vertexErrors = 0;
			for (uint32_t quad = 0; quad < 63; ++quad)
			{
				const auto& submesh = mesh->Submeshes[quad];
				CHECK(submesh.VertexOffset == quad * extent.VertexStride);
				CHECK(submesh.VertexCount == extent.VertexStride);
				CHECK(submesh.IndexOffset == quad * 6);
				CHECK(submesh.IndexCount == 6);
				REQUIRE(submesh.MaterialIndex < mesh->Materials.size());
				CHECK(mesh->Materials[submesh.MaterialIndex] == imported.Materials[quad]->Handle);
				CHECK(imported.Materials[quad]->BaseColorFactor == QuadColor(quad, &extent));
				CHECK(imported.Materials[quad]->DoubleSided);
				const auto expected = QuadVertices(quad, true);
				for (uint32_t i = 0; i < extent.VertexStride; ++i)
				{
					const auto& vertex = mesh->Vertices[quad * extent.VertexStride + i];
					const glm::vec3 position = i < 4 ? expected[i].Position : glm::vec3(20, 20, -2);
					vertexErrors += vertex.Position != position || vertex.Normal != glm::vec3(0, 0, 1) || vertex.Tangent != glm::vec4(1, 0, 0, 1) || vertex.UV != glm::vec2(0) ? 1u : 0u;
				}
				for (uint32_t i = 0; i < 6; ++i)
					CHECK(mesh->Indices[quad * 6 + i] == QuadIndices[i]);
			}
			CHECK(vertexErrors == 0);
		}
		std::ofstream manifest(folder / "Import.json");
		REQUIRE(manifest);
		manifest << nlohmann::json{{"fixtureBytes", bytes}, {"merged", MeshManifest(*imported.Mesh)}, {"node", MeshManifest(*imported.NodeMeshes.front())}}.dump(2) << '\n';
		manifest.close();
		CHECK_FALSE(manifest.fail());
		return imported;
	}

	void CheckRepresentation(GraphicsDevice& graphics, Representation representation, const char* name, const Extent* extent = nullptr,
							 const MeshImportResult* imported = nullptr, const Transition* transition = nullptr)
	{
		const uint32_t width = extent ? 360 : Width, height = transition ? 320 : extent ? 280
																						: Height;
		AssetManager assets;
		Scene scene("AnalyticMeshOffsets");
		std::vector<Ref<MeshAsset>> meshes;
		if (imported)
		{
			for (const auto& material : imported->Materials)
				assets.AddMemoryAsset(material);
			assets.AddMemoryAsset(imported->Mesh);
			scene.CreateEntityWithUUID(UUID(73000), "Imported analytic quads").AddComponent<MeshComponent>().Mesh = imported->Mesh->Handle;
			meshes.push_back(imported->Mesh);
		}
		else
			meshes = MakeMeshes(representation, assets, scene, extent);
		if (transition)
		{
			REQUIRE(extent);
			if (transition->Blend)
			{
				auto material = assets.GetAsset<MaterialAsset>(AssetHandle(71062));
				REQUIRE(material);
				material->Alpha = AlphaMode::Blend;
				++material->Version;
			}
			if (transition->Text)
			{
				auto entity = scene.CreateEntityWithUUID(UUID(74001), "Diagnostic HUD");
				entity.GetTransform().Translation = {8.0f / width, 286.0f / height, 0};
				auto& text = entity.AddComponent<TextComponent>();
				text.Text = "HUD";
				text.Size = 24.0f * TextRenderer::ReferenceViewportHeight / height;
				text.ScreenSpace = true;
				text.Color = glm::vec4(1);
			}
		}
		auto& sky = scene.CreateEntityWithUUID(UUID(74000), "Black sky").AddComponent<SkyLightComponent>();
		sky.Intensity = 0;
		sky.ShowBackground = false;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Path = RenderPath::Forward;
		settings.DebugView = RenderDebugView::BaseColor;
		settings.Tonemapper = TonemapOperator::None;
		settings.GIEnabled = false;
		settings.Shadows = false;
		settings.DefaultEnvironmentIntensity = 0;
		auto camera = CameraView::LookAt({}, {0, 0, -1}, glm::radians(60.0f), float(width) / height, 0.1f, 10);
		camera.Orthographic = true;
		camera.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, transition ? 1.0f - float(height) / 140.0f : -1.0f, 1.0f, camera.Far, camera.Near);
		auto* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		auto texture = device->createTexture(desc);
		REQUIRE(texture);
		auto framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
		REQUIRE(framebuffer);
		const auto folder = extent ? Testing::GetTestOutputDirectory() / "MeshOffsets" / extent->Name / name : Testing::GetTestOutputDirectory() / "MeshOffsets" / name;
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		REQUIRE_FALSE(error);
		nlohmann::json report = {{"representation", name}, {"extent", extent ? extent->Name : "small"}, {"width", width}, {"height", height}, {"frames", nlohmann::json::array()}, {"materials", nlohmann::json::array()}, {"oracle", extent ? "63 tiles in9x7 grid,40px cells,24px quads with8px margins. Code=tile+1; two bits/channel encode linear0,.25,.5,1 as sRGB0,137,188,255 (tolerance1). White control overrides all channels255." : "Red/green/blue rectangles x=[16,48),[80,112),[144,176), y=[40,88)."}, {"oracleMargins", "Frame2 shifts x by4. Interiors and black exterior exclude two-pixel edge margins; separate5x5 centers."}, {"camera", "Origin looking -Z; orthographic[-1,1]x[-1,1], reversed depth near.1 far10; geometry z=-2, identity models."}};
		if (transition)
		{
			report["transition"] = {{"control", transition->Name}, {"lastTileBlendAlphaOne", transition->Blend}, {"screenText", transition->Text}};
			report["processArguments"] = nlohmann::json::array();
			const auto& arguments = Testing::GetMainArguments();
			for (int i = 0; i < arguments.Count; ++i)
				report["processArguments"].push_back(arguments.Values[i]);
			report["diagnosticCaseIndex"] = transition->PrecedingCases.size();
			report["precedingDiagnosticCases"] = transition->PrecedingCases;
			report["initialState"] = "New SceneRenderer, shared GraphicsDevice; no warmup/discard. CTest runs this test alone; unfiltered binary may run earlier tests (see process arguments).";
			report["oracleMargins"] = "No geometry shift; original63tile oracle in top280 rows; footer40 rows for isolated HUD.";
			report["camera"] = "Origin looking -Z; ortho x[-1,1], y[1-320/140,1], reversed near.1 far10; original grid pixel positions unchanged.";
		}
		for (uint32_t quad = 0; quad < (extent ? 63u : 3u); ++quad)
		{
			const auto color = QuadColor(quad, extent);
			report["materials"].push_back({color.r, color.g, color.b, color.a});
		}
		constexpr std::array paths{RenderPath::Visibility, RenderPath::Visibility, RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility, RenderPath::Visibility};
		for (uint32_t frame = 0; frame < (transition ? paths.size() : 3); ++frame)
		{
			CAPTURE(name);
			CAPTURE(frame);
			if (transition)
				settings.Path = paths[frame];
			const bool reupload = !transition && frame == 2;
			if (reupload)
				for (auto& mesh : meshes)
				{
					for (auto& vertex : mesh->Vertices)
						vertex.Position.x += 8.0f / float(width);
					mesh->ComputeBounds();
					++mesh->Version;
				}
			renderer.Render(scene, camera, framebuffer);
			const auto image = ReadbackTexture(device, texture);
			const auto result = Inspect(image, reupload ? 4 : 0, extent, transition != nullptr);
			const auto& stats = renderer.GetStats();
			CHECK(stats.RequestedPath == settings.Path);
			CHECK(stats.EffectivePath == settings.Path);
			CHECK(stats.PathFallbackReason.empty());
			CHECK(stats.CulledSubmeshes == 0);
			CHECK(result.InteriorSamples == (extent ? 63 * 20 * 20 : 3 * 28 * 44));
			CHECK(result.BackgroundSamples > 18000);
			CHECK(result.InteriorErrors == 0);
			CHECK(result.BackgroundErrors == 0);
			for (uint32_t failures : result.CenterErrors)
				CHECK(failures == 0);
			uint32_t footerWhite = 0, footerErrors = 0;
			if (transition)
			{
				for (uint32_t y = 280; y < 320; ++y)
					for (uint32_t x = 0; x < width; ++x)
					{
						const auto pixel = image.GetPixel(x, y);
						const bool textRegion = transition->Text && x >= 6 && x < 96 && y >= 284 && y < 318;
						footerWhite += textRegion && pixel.r > 200 && pixel.g > 200 && pixel.b > 200 ? 1u : 0u;
						footerErrors += !textRegion && (pixel.r > 1 || pixel.g > 1 || pixel.b > 1) ? 1u : 0u;
					}
				CHECK(footerErrors == 0);
				CHECK(stats.TextGlyphs == (transition->Text ? 3 : 0));
				if (transition->Text)
					CHECK(footerWhite > 20);
			}
			CHECK(image.SavePNG(folder / (std::to_string(frame) + ".png")));
			nlohmann::json manifests = nlohmann::json::array();
			for (const auto& mesh : meshes)
				manifests.push_back(MeshManifest(*mesh));
			report["frames"].push_back({{"frame", frame}, {"renderFrame", stats.RenderFrame}, {"phase", frame == 0 ? "cold" : frame == 1 ? "stable"
																																		 : "version-reupload-shift4"},
										{"settings", Reflection::ToJson(SceneRendererSettings::GetFields(), &settings)},
										{"drawCalls", stats.DrawCalls},
										{"culledSubmeshes", stats.CulledSubmeshes},
										{"interiorErrors", result.InteriorErrors},
										{"backgroundErrors", result.BackgroundErrors},
										{"centerErrors", result.CenterErrors},
										{"meshes", manifests}});
			auto& recorded = report["frames"].back();
			if (transition)
				recorded["phase"] = "path-transition";
			recorded["requestedPath"] = static_cast<int>(stats.RequestedPath);
			recorded["effectivePath"] = static_cast<int>(stats.EffectivePath);
			recorded["fallback"] = stats.PathFallbackReason;
			recorded["footerWhite"] = footerWhite;
			recorded["footerErrors"] = footerErrors;
			recorded["textGlyphs"] = stats.TextGlyphs;
			std::ofstream file(folder / "Report.json");
			REQUIRE(file);
			file << report.dump(2) << '\n';
			file.close();
			CHECK_FALSE(file.fail());
		}
	}
}

TEST_CASE("MeshOffsets: analytic oracle rejects missing recolored and misplaced geometry")
{
	Image correct(Width, Height);
	for (uint32_t quad = 0; quad < 3; ++quad)
		for (uint32_t y = 40; y < 88; ++y)
			for (uint32_t x = Left[quad]; x < Left[quad] + 32; ++x)
			{
				glm::u8vec4 color(0, 0, 0, 255);
				color[quad] = 255;
				correct.SetPixel(x, y, color);
			}
	CHECK(Inspect(correct, 0).InteriorErrors == 0);
	CHECK(Inspect(correct, 0).BackgroundErrors == 0);
	CHECK(Inspect(Image(Width, Height), 0).InteriorErrors == 3 * 28 * 44);
	auto recolored = correct;
	recolored.SetPixel(32, 64, {0, 255, 0, 255});
	CHECK(Inspect(recolored, 0).CenterErrors[0] == 1);
	auto misplaced = correct;
	misplaced.SetPixel(64, 64, {255, 0, 0, 255});
	CHECK(Inspect(misplaced, 0).BackgroundErrors == 1);
	CHECK(Inspect(correct, 4).InteriorErrors > 0);
}

TEST_CASE("MeshOffsets: successive submeshes preserve independent regions across indexed representations")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (const auto& [representation, name] : std::array{
			 std::pair{Representation::Local, "local-base"}, std::pair{Representation::Global, "global-base-zero"}, std::pair{Representation::Separate, "separate-buffers"}})
	{
		CheckRepresentation(graphics, representation, name);
		graphics.WaitForIdle();
		graphics.CollectGarbage();
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("MeshOffsets: grid oracle rejects missing tiles wrong materials gutters and stale geometry")
{
	for (const auto& extent : {Extents.front(), Extents.back()})
	{
		Image correct(360, 280);
		for (uint32_t quad = 0; quad < 63; ++quad)
		{
			glm::u8vec4 color(255);
			constexpr std::array<uint8_t, 4> encoded{0, 137, 188, 255};
			for (uint32_t channel = 0; channel < 3; ++channel)
				color[channel] = extent.White ? 255 : encoded[((quad + 1) >> (2 * channel)) & 3u];
			for (uint32_t y = (quad / 9) * 40 + 8; y < (quad / 9) * 40 + 32; ++y)
				for (uint32_t x = (quad % 9) * 40 + 8; x < (quad % 9) * 40 + 32; ++x)
					correct.SetPixel(x, y, color);
		}
		const auto good = Inspect(correct, 0, &extent);
		CHECK(good.InteriorErrors == 0);
		CHECK(good.BackgroundErrors == 0);
		CHECK(good.InteriorSamples == 63 * 400);
		CHECK(Inspect(Image(360, 280), 0, &extent).InteriorErrors == 63 * 400);
		auto recolored = correct;
		recolored.SetPixel(340, 260, {0, 0, 0, 255});
		CHECK(Inspect(recolored, 0, &extent).CenterErrors[62] == 1);
		auto misplaced = correct;
		misplaced.SetPixel(39, 20, {255, 255, 255, 255});
		CHECK(Inspect(misplaced, 0, &extent).BackgroundErrors == 1);
		CHECK(Inspect(correct, 4, &extent).InteriorErrors > 0);
	}
}

TEST_CASE("MeshOffsets: generated glTF preserves decoded local indices vertices and materials")
{
	for (const auto& extent : {Extents[0], Extents[1]})
	{
		CAPTURE(extent.Name);
		const auto imported = ImportGrid(extent, "cpu-import");
		CHECK(imported.Mesh->Vertices.size() == 63 * extent.VertexStride);
	}
}

TEST_CASE("MeshOffsets: sixty three draws preserve regions across independent buffer extents")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (const auto& extent : Extents)
	{
		CAPTURE(extent.Name);
		for (const auto& [representation, name] : std::array{
				 std::pair{Representation::Local, "local-base"}, std::pair{Representation::Global, "global-base-zero"}, std::pair{Representation::Separate, "separate-buffers"}})
		{
			// Separate buffers are compact for every extent; run once for each material control.
			if (representation == Representation::Separate && &extent != &Extents.front() && !extent.White)
				continue;
			CheckRepresentation(graphics, representation, name, &extent);
			graphics.WaitForIdle();
			graphics.CollectGarbage();
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("MeshOffsets: imported compact and padded primitives match the independent grid oracle")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (const auto& extent : {Extents[0], Extents[1]})
	{
		CAPTURE(extent.Name);
		const auto imported = ImportGrid(extent, "gpu-import");
		CheckRepresentation(graphics, Representation::Local, "imported", &extent, &imported);
		graphics.WaitForIdle();
		graphics.CollectGarbage();
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("MeshOffsets: raw pulling survives indexed offsets path transitions and screen text")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	std::vector<std::string> preceding;
	for (const auto& [representation, name] : std::array{
			 std::pair{Representation::Local, "local-offset"}, std::pair{Representation::Global, "global-index"}})
		for (auto transition : {Transition{"opaque", false, false}, Transition{"blend", true, false}, Transition{"blend-text", true, true}})
		{
			const std::string label = std::string("transition-") + name + "-" + transition.Name;
			transition.PrecedingCases = preceding;
			CheckRepresentation(graphics, representation, label.c_str(), &Extents[1], nullptr, &transition);
			preceding.push_back(label);
			graphics.WaitForIdle();
			graphics.CollectGarbage();
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
