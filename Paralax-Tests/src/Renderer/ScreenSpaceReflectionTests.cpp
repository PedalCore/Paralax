#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

using namespace Paralax;

namespace {

	Ref<MeshAsset> ReflectionQuad(AssetHandle handle, const std::array<glm::vec3, 4>& positions, glm::vec3 normal)
	{
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = handle;
		for (size_t i = 0; i < positions.size(); ++i)
		{
			MeshVertex vertex;
			vertex.Position = positions[i];
			vertex.Normal = normal;
			vertex.Tangent = {1, 0, 0, 1};
			vertex.UV = {i == 1 || i == 2 ? 1 : 0, i >= 2 ? 1 : 0};
			mesh->Vertices.push_back(vertex);
		}
		mesh->Indices = {0, 1, 2, 0, 2, 3};
		Submesh submesh;
		submesh.IndexCount = 6;
		submesh.VertexCount = 4;
		mesh->Submeshes.push_back(submesh);
		mesh->ComputeBounds();
		return mesh;
	}

}

namespace {

	struct ReflectionCase
	{
		bool Orthographic;
		RenderPath Path;
	};

	void CheckEmissiveReflections(std::span<const ReflectionCase> cases, bool detailed = true)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		auto* device = graphics.GetDevice();
		const auto errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			auto mirror = CreateRef<MaterialAsset>();
			mirror->Handle = AssetHandle(61001);
			mirror->MetallicFactor = 1;
			mirror->RoughnessFactor = 0.08f;
			mirror->BaseColorFactor = glm::vec4(1);
			assets.AddMemoryAsset(mirror);
			auto emitter = CreateRef<MaterialAsset>();
			emitter->Handle = AssetHandle(61002);
			emitter->EmissiveFactor = {0.5f, 0, 0};
			emitter->BaseColorFactor = glm::vec4(0, 0, 0, 1);
			assets.AddMemoryAsset(emitter);
			assets.AddMemoryAsset(ReflectionQuad(AssetHandle(61003), {{{-3, 0, 3}, {3, 0, 3}, {3, 0, -3}, {-3, 0, -3}}}, {0, 1, 0}));
			assets.AddMemoryAsset(ReflectionQuad(AssetHandle(61004), {{{-2, 0, -2}, {2, 0, -2}, {2, 3, -2}, {-2, 3, -2}}}, {0, 0, 1}));
			Scene scene("ReflectionAnalyticMirror");
			auto floor = scene.CreateEntity("Mirror");
			auto& floorMesh = floor.AddComponent<MeshComponent>();
			floorMesh.Mesh = AssetHandle(61003);
			floorMesh.Materials = {mirror->Handle};
			auto source = scene.CreateEntity("Emissive panel");
			auto& panelMesh = source.AddComponent<MeshComponent>();
			panelMesh.Mesh = AssetHandle(61004);
			panelMesh.Materials = {emitter->Handle};
			nvrhi::TextureDesc desc;
			desc.width = desc.height = 96;
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			auto texture = device->createTexture(desc);
			auto framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.DefaultEnvironmentIntensity = 0;
			settings.GIEnabled = false;
			settings.Shadows = false;
			settings.Tonemapper = TonemapOperator::None;
			settings.SSRMaxDistance = 20;
			settings.SSRMaxSteps = 256;
			for (const auto [orthographic, path] : cases)
			{
				CAPTURE(orthographic);
				auto camera = CameraView::LookAt({0, 2, 4}, {0, 0, -1}, glm::radians(60.0f), 1);
				if (orthographic)
					camera.Projection = glm::orthoRH_ZO(-3.0f, 3.0f, -3.0f, 3.0f, camera.Far, camera.Near);
				const glm::mat4 inverse = glm::inverse(camera.Projection * camera.View);
				std::vector<glm::uvec2> expectedHits;
				for (uint32_t y = 0; y < 96; ++y)
					for (uint32_t x = 0; x < 96; ++x)
					{
						const glm::vec2 ndc = (glm::vec2(x, y) + 0.5f) / 96.0f * glm::vec2(2, -2) + glm::vec2(-1, 1);
						const glm::vec4 near = inverse * glm::vec4(ndc, 1, 1);
						const glm::vec3 origin = orthographic ? glm::vec3(near) / near.w : glm::vec3(0, 2, 4);
						const glm::vec3 direction = orthographic ? glm::normalize(glm::vec3(glm::inverse(camera.View)[2])) * -1.0f : glm::normalize(glm::vec3(near) / near.w - origin);
						if (direction.y >= -0.01f)
							continue;
						const float distance = -origin.y / direction.y;
						const glm::vec3 point = origin + direction * distance;
						if (std::abs(point.x) >= 2 || point.z <= -1.5f || point.z >= 2)
							continue;
						const glm::vec3 ray = glm::reflect(direction, glm::vec3(0, 1, 0));
						const float hitDistance = (-2 - point.z) / ray.z;
						const glm::vec3 hit = point + ray * hitDistance;
						if (hitDistance <= 0 || std::abs(hit.x) >= 1.5f || hit.y < 0.3f || hit.y > 2.5f)
							continue;
						expectedHits.emplace_back(x, y);
					}
				REQUIRE(expectedHits.size() > 40);
				CAPTURE(int(path));
				settings.Path = path;
				auto render = [&] {
					renderer.Render(scene, camera, framebuffer);
					INFO(renderer.GetStats().PathFallbackReason);
					REQUIRE(renderer.GetStats().EffectivePath == path);
				};
				settings.SSREnabled = false;
				render();
				const auto before = ReadbackTexture(device, texture);
				settings.SSREnabled = true;
				render();
				INFO(renderer.GetStats().SSRFallbackReason);
				REQUIRE(renderer.GetStats().SSRActive);
				const auto after = ReadbackTexture(device, texture);
				if (!detailed)
				{
					REQUIRE(std::find(expectedHits.begin(), expectedHits.end(), glm::uvec2(46, 56)) != expectedHits.end());
					CHECK(before.GetPixel(46, 56).r < 2);
					CHECK(after.GetPixel(46, 56).r > 100);
					continue;
				}
				if (!orthographic && path == RenderPath::Forward)
					for (size_t sample = 1; sample <= 4; ++sample)
					{
						const auto pixel = expectedHits[expectedHits.size() * sample / 5];
						const auto probe = renderer.InspectReflectionPixel(pixel, renderer.GetStats().RenderFrame);
						REQUIRE(probe);
						CHECK((probe->PlaneFlags & 32u) != 0);
						CHECK(probe->Confidence > 0.99f);
					}

				// Independent geometric intersections across neighboring raster phases; all are interior panel hits.
				for (uint32_t y = 54; y < 58; ++y)
					for (uint32_t x = 44; x < 48; ++x)
					{
						const glm::uvec2 pixel{x, y};
						REQUIRE(std::find(expectedHits.begin(), expectedHits.end(), pixel) != expectedHits.end());
						const glm::vec2 ndc = (glm::vec2(pixel) + 0.5f) / 96.0f * glm::vec2(2, -2) + glm::vec2(-1, 1);
						const glm::vec4 near = inverse * glm::vec4(ndc, 1, 1);
						const auto origin = orthographic ? glm::vec3(near) / near.w : glm::vec3(0, 2, 4);
						const auto direction = orthographic ? -glm::normalize(glm::vec3(glm::inverse(camera.View)[2])) : glm::normalize(glm::vec3(near) / near.w - origin);
						const auto reflected = glm::reflect(direction, glm::vec3(0, 1, 0));
						const auto receiver = origin + direction * (-origin.y / direction.y) + glm::vec3(0, 0.01f, 0);
						const float distance = (-2 - receiver.z) / reflected.z;
						const auto point = receiver + reflected * distance;
						const auto clip = camera.Projection * camera.View * glm::vec4(point, 1);
						const auto projected = (glm::vec2(clip) / clip.w * glm::vec2(0.5f, -0.5f) + 0.5f) * 96.0f;
						const auto probe = renderer.InspectReflectionPixel(pixel, renderer.GetStats().RenderFrame);
						REQUIRE(probe);
						CAPTURE(x);
						CAPTURE(y);
						CHECK(probe->CandidateTexel == glm::floor(projected));
						CHECK((probe->PlaneFlags & 32u) != 0);
						CHECK(probe->Distance == doctest::Approx(double(distance)).epsilon(0.0002));
						CHECK(probe->Confidence > 0.99f);
						CHECK(after.GetPixel(x, y).r > 100);
					}
				uint32_t hits = 0;
				for (const auto pixel : expectedHits)
				{
					CHECK(before.GetPixel(pixel.x, pixel.y).r < 2);
					hits += after.GetPixel(pixel.x, pixel.y).r > 100 ? 1 : 0;
					CHECK(after.GetPixel(pixel.x, pixel.y).g < 2);
					CHECK(after.GetPixel(pixel.x, pixel.y).b < 2);
				}
				CHECK(hits > expectedHits.size() * 9 / 10);
				// A ray bound shorter than the panel distance must restore the original environment fallback.
				settings.SSRMaxDistance = 0.1f;
				render();
				const auto miss = ReadbackTexture(device, texture);
				for (const auto pixel : expectedHits)
					CHECK(miss.GetPixel(pixel.x, pixel.y) == before.GetPixel(pixel.x, pixel.y));
				settings.SSRMaxDistance = 20;
				for (bool roughnessReject : {false, true})
				{
					settings.EnvironmentSpecular = roughnessReject;
					settings.SSRMaxRoughness = roughnessReject ? 0.05f : 0.8f;
					render();
					const auto rejected = ReadbackTexture(device, texture);
					for (const auto pixel : expectedHits)
						CHECK(rejected.GetPixel(pixel.x, pixel.y) == before.GetPixel(pixel.x, pixel.y));
				}
				settings.EnvironmentSpecular = true;
				settings.SSRMaxRoughness = 0.8f;
				if (!orthographic)
				{
					// An opaque black-albedo emitter remains covered; masked and blended holes cannot supply radiance.
					for (auto alpha : {AlphaMode::Mask, AlphaMode::Blend})
					{
						emitter->Alpha = alpha;
						emitter->BaseColorFactor.a = alpha == AlphaMode::Mask ? 0.0f : 1.0f;
						++emitter->Version;
						render();
						const auto uncovered = ReadbackTexture(device, texture);
						for (const auto pixel : expectedHits)
							CHECK(uncovered.GetPixel(pixel.x, pixel.y).r < 2);
					}
					emitter->Alpha = AlphaMode::Mask;
					emitter->BaseColorFactor.a = 1;
					++emitter->Version;
					source.GetComponent<TransformComponent>().Scale.x = -1;
					render();
					const auto mirrored = ReadbackTexture(device, texture);
					uint32_t mirroredHits = 0;
					for (const auto pixel : expectedHits)
						mirroredHits += mirrored.GetPixel(pixel.x, pixel.y).r > 100 ? 1u : 0u;
					CHECK(mirroredHits > expectedHits.size() * 9 / 10);
					source.GetComponent<TransformComponent>().Scale.x = 1;
					emitter->Alpha = AlphaMode::Opaque;
					++emitter->Version;
					mirror->Alpha = AlphaMode::Blend;
					++mirror->Version;
					render();
					const auto transparentReceiver = ReadbackTexture(device, texture);
					for (const auto pixel : expectedHits)
						CHECK(transparentReceiver.GetPixel(pixel.x, pixel.y).r < 2);
					mirror->Alpha = AlphaMode::Opaque;
					++mirror->Version;
					// A strong white environment must be removed at red-panel hits, rather than added a second time.
					settings.DefaultEnvironmentIntensity = 1;
					settings.SSREnabled = false;
					render();
					const auto environment = ReadbackTexture(device, texture);
					settings.SSREnabled = true;
					render();
					const auto replacement = ReadbackTexture(device, texture);
					uint32_t removed = 0;
					for (const auto pixel : expectedHits)
					{
						const auto original = environment.GetPixel(pixel.x, pixel.y);
						const auto reflected = replacement.GetPixel(pixel.x, pixel.y);
						CHECK(original.g > 20);
						removed += reflected.g < original.g / 2 && reflected.r > 100 ? 1u : 0u;
					}
					CHECK(removed > expectedHits.size() * 8 / 10);
					settings.DefaultEnvironmentIntensity = 0;
				}
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}

}

TEST_CASE("ScreenSpaceReflections: opaque emissive hits preserve replacement energy in Forward perspective views")
{
	const std::array cases{ReflectionCase{false, RenderPath::Forward}};
	CheckEmissiveReflections(cases);
}

TEST_CASE("ScreenSpaceReflections: opaque emissive hits preserve replacement energy in Deferred perspective views")
{
	const std::array cases{ReflectionCase{false, RenderPath::Deferred}};
	CheckEmissiveReflections(cases);
}

TEST_CASE("ScreenSpaceReflections: opaque emissive hits preserve replacement energy in Visibility perspective views")
{
	const std::array cases{ReflectionCase{false, RenderPath::Visibility}};
	CheckEmissiveReflections(cases);
}

TEST_CASE("ScreenSpaceReflections: opaque emissive hits preserve replacement energy in Forward orthographic views")
{
	const std::array cases{ReflectionCase{true, RenderPath::Forward}};
	CheckEmissiveReflections(cases);
}

TEST_CASE("ScreenSpaceReflections: opaque emissive hits preserve replacement energy in Deferred orthographic views")
{
	const std::array cases{ReflectionCase{true, RenderPath::Deferred}};
	CheckEmissiveReflections(cases);
}

TEST_CASE("ScreenSpaceReflections: opaque emissive hits preserve replacement energy in Visibility orthographic views")
{
	const std::array cases{ReflectionCase{true, RenderPath::Visibility}};
	CheckEmissiveReflections(cases);
}

TEST_CASE("ScreenSpaceReflections: emissive hits survive shared renderer path and projection transitions")
{
	const std::array cases{
		ReflectionCase{false, RenderPath::Forward},
		ReflectionCase{false, RenderPath::Deferred},
		ReflectionCase{false, RenderPath::Visibility},
		ReflectionCase{true, RenderPath::Forward},
		ReflectionCase{true, RenderPath::Deferred},
		ReflectionCase{true, RenderPath::Visibility}};
	CheckEmissiveReflections(cases, false);
}
