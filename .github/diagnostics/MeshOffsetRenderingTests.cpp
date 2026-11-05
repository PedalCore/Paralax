#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Renderer/SceneRenderer.h>
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

	enum class Representation
	{
		Local,
		Global,
		Separate
	};

	struct OracleResult
	{
		uint32_t InteriorErrors = 0, BackgroundErrors = 0, InteriorSamples = 0, BackgroundSamples = 0;
		std::array<uint32_t, 3> CenterErrors{};
	};

	// Pixel regions are specified independently of mesh projection and draw representation.
	OracleResult Inspect(const Image& image, uint32_t shift)
	{
		REQUIRE(image.GetWidth() == Width);
		REQUIRE(image.GetHeight() == Height);
		OracleResult result;
		for (uint32_t y = 0; y < Height; ++y)
			for (uint32_t x = 0; x < Width; ++x)
			{
				int interior = -1;
				bool edge = false;
				for (uint32_t quad = 0; quad < 3; ++quad)
				{
					const uint32_t left = Left[quad] + shift;
					if (x >= left + 2 && x < left + 30 && y >= 42 && y < 86)
						interior = static_cast<int>(quad);
					else if (x >= left - 2 && x < left + 34 && y >= 38 && y < 90)
						edge = true;
				}
				if (interior < 0 && edge)
					continue;
				const auto pixel = image.GetPixel(x, y);
				bool wrong = false;
				for (int channel = 0; channel < 3; ++channel)
					wrong |= std::abs(int(pixel[channel]) - (channel == interior ? 255 : 0)) > 1;
				if (interior >= 0)
				{
					++result.InteriorSamples;
					result.InteriorErrors += wrong ? 1u : 0u;
					const uint32_t center = Left[static_cast<size_t>(interior)] + shift + 16;
					if (x >= center - 2 && x <= center + 2 && y >= 62 && y <= 66)
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

	std::array<MeshVertex, 4> QuadVertices(uint32_t quad)
	{
		std::array<MeshVertex, 4> vertices;
		const float left = float(Left[quad]) / 96 - 1, right = float(Left[quad] + 32) / 96 - 1;
		const float bottom = -0.375f, top = 0.375f;
		const std::array<glm::vec3, 4> positions{{{left, bottom, -2}, {right, bottom, -2}, {right, top, -2}, {left, top, -2}}};
		for (size_t i = 0; i < vertices.size(); ++i)
		{
			vertices[i].Position = positions[i];
			vertices[i].Normal = {0, 0, 1};
			vertices[i].Tangent = {1, 0, 0, 1};
		}
		return vertices;
	}

	std::string WordHash(const std::vector<uint32_t>& words)
	{
		uint64_t hash = 14695981039346656037ull;
		for (uint32_t word : words)
			for (uint32_t byte = 0; byte < 4; ++byte)
				hash = (hash ^ ((word >> (byte * 8)) & 255u)) * 1099511628211ull;
		std::ostringstream text;
		text << std::hex << std::setfill('0') << std::setw(16) << hash;
		return text.str();
	}

	nlohmann::json MeshManifest(const MeshAsset& mesh)
	{
		std::vector<uint32_t> words;
		for (const auto& vertex : mesh.Vertices)
			for (float value : {vertex.Position.x, vertex.Position.y, vertex.Position.z, vertex.Normal.x, vertex.Normal.y, vertex.Normal.z,
								vertex.Tangent.x, vertex.Tangent.y, vertex.Tangent.z, vertex.Tangent.w, vertex.UV.x, vertex.UV.y})
				words.push_back(std::bit_cast<uint32_t>(value));
		nlohmann::json manifest = {{"handle", uint64_t(mesh.Handle)}, {"version", mesh.Version}, {"vertexCount", mesh.Vertices.size()}, {"indexCount", mesh.Indices.size()}, {"vertexBytes", mesh.Vertices.size() * sizeof(MeshVertex)}, {"indexBytes", mesh.Indices.size() * sizeof(uint32_t)}, {"hashEncoding", "FNV1a64 over little-endian uint32 field words; vertex fields position/normal/tangent/UV, no host padding"}, {"vertexHash", WordHash(words)}, {"indexHash", WordHash(mesh.Indices)}, {"vertexWords", words}, {"indexWords", mesh.Indices}, {"submeshes", nlohmann::json::array()}};
		for (const auto& submesh : mesh.Submeshes)
			manifest["submeshes"].push_back({{"vertexOffset", submesh.VertexOffset}, {"vertexCount", submesh.VertexCount}, {"indexOffset", submesh.IndexOffset}, {"indexCount", submesh.IndexCount}, {"materialSlot", submesh.MaterialIndex}, {"boundsMin", {submesh.Bounds.Min.x, submesh.Bounds.Min.y, submesh.Bounds.Min.z}}, {"boundsMax", {submesh.Bounds.Max.x, submesh.Bounds.Max.y, submesh.Bounds.Max.z}}});
		return manifest;
	}

	std::vector<Ref<MeshAsset>> MakeMeshes(Representation representation, AssetManager& assets, Scene& scene)
	{
		std::vector<AssetHandle> materials;
		for (uint32_t quad = 0; quad < 3; ++quad)
		{
			auto material = CreateRef<MaterialAsset>();
			material->Handle = AssetHandle(71000 + quad);
			material->DoubleSided = true;
			material->BaseColorFactor = {0, 0, 0, 1};
			material->BaseColorFactor[quad] = 1;
			assets.AddMemoryAsset(material);
			materials.push_back(material->Handle);
		}
		std::vector<Ref<MeshAsset>> meshes;
		const uint32_t count = representation == Representation::Separate ? 3u : 1u;
		for (uint32_t meshIndex = 0; meshIndex < count; ++meshIndex)
		{
			auto mesh = CreateRef<MeshAsset>();
			mesh->Handle = AssetHandle(72000 + meshIndex);
			mesh->Materials = materials;
			mesh->Vertices.resize(count == 3 ? 4 : 30);
			mesh->Indices.resize(count == 3 ? 6 : 30, 0);
			for (auto& vertex : mesh->Vertices)
				vertex.Position = {20, 20, -2};
			for (uint32_t quad = 0; quad < 3; ++quad)
			{
				if (count == 3 && quad != meshIndex)
					continue;
				const uint32_t vertexOffset = count == 3 ? 0 : VertexOffsets[quad];
				const uint32_t indexOffset = count == 3 ? 0 : IndexOffsets[quad];
				const auto vertices = QuadVertices(quad);
				for (uint32_t i = 0; i < 4; ++i)
					mesh->Vertices[vertexOffset + i] = vertices[i];
				for (uint32_t i = 0; i < 6; ++i)
					mesh->Indices[indexOffset + i] = QuadIndices[i] + (representation == Representation::Global ? vertexOffset : 0);
				Submesh submesh;
				submesh.VertexOffset = representation == Representation::Global ? 0 : vertexOffset;
				submesh.VertexCount = representation == Representation::Global ? static_cast<uint32_t>(mesh->Vertices.size()) : 4;
				submesh.IndexOffset = indexOffset;
				submesh.IndexCount = 6;
				submesh.MaterialIndex = quad;
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

	void CheckRepresentation(GraphicsDevice& graphics, Representation representation, const char* name)
	{
		AssetManager assets;
		Scene scene("AnalyticMeshOffsets");
		auto meshes = MakeMeshes(representation, assets, scene);
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
		auto camera = CameraView::LookAt({}, {0, 0, -1}, glm::radians(60.0f), float(Width) / Height, 0.1f, 10);
		camera.Orthographic = true;
		camera.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, camera.Far, camera.Near);
		auto* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = Width;
		desc.height = Height;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		auto texture = device->createTexture(desc);
		REQUIRE(texture);
		auto framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
		REQUIRE(framebuffer);
		const auto folder = Testing::GetTestOutputDirectory() / "MeshOffsets" / name;
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		REQUIRE_FALSE(error);
		nlohmann::json report = {{"representation", name}, {"width", Width}, {"height", Height}, {"frames", nlohmann::json::array()}, {"oracle", "Red/green/blue rectangles x=[16,48),[80,112),[144,176), y=[40,88); frame2 shifts x by4. Interiors and black exterior exclude only two-pixel edge margins; separate 5x5 centers."}};
		for (uint32_t frame = 0; frame < 3; ++frame)
		{
			CAPTURE(name);
			CAPTURE(frame);
			if (frame == 2)
				for (auto& mesh : meshes)
				{
					for (auto& vertex : mesh->Vertices)
						vertex.Position.x += 4.0f / 96;
					mesh->ComputeBounds();
					++mesh->Version;
				}
			renderer.Render(scene, camera, framebuffer);
			const auto image = ReadbackTexture(device, texture);
			const auto result = Inspect(image, frame == 2 ? 4 : 0);
			const auto& stats = renderer.GetStats();
			CHECK(stats.EffectivePath == RenderPath::Forward);
			CHECK(stats.CulledSubmeshes == 0);
			CHECK(result.InteriorSamples == 3 * 28 * 44);
			CHECK(result.BackgroundSamples > 18000);
			CHECK(result.InteriorErrors == 0);
			CHECK(result.BackgroundErrors == 0);
			for (uint32_t failures : result.CenterErrors)
				CHECK(failures == 0);
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
