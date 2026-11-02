#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <limits>

using namespace Paralax;

namespace {

	struct ShadowOutput
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
		explicit ShadowOutput(nvrhi::IDevice* device)
		{
			nvrhi::TextureDesc desc;
			desc.width = desc.height = 128;
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Texture = device->createTexture(desc);
			Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Texture));
		}
	};

	Entity AddMesh(Scene& scene, const char* name, uint64_t mesh, glm::vec3 position, glm::vec3 scale = glm::vec3(1))
	{
		auto entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		entity.GetTransform().Scale = scale;
		auto& component = entity.AddComponent<MeshComponent>();
		component.Mesh = AssetHandle(mesh);
		component.Materials = {AssetHandle(AssetManager::BuiltinGroundMaterial)};
		return entity;
	}

	float SampleWorld(const Image& image, const CameraView& camera, glm::vec3 position)
	{
		const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(position, 1);
		const glm::vec2 pixel = glm::vec2(clip) / clip.w * glm::vec2(0.5f, -0.5f) + 0.5f;
		REQUIRE(pixel.x > 0.02f);
		REQUIRE(pixel.x < 0.98f);
		REQUIRE(pixel.y > 0.02f);
		REQUIRE(pixel.y < 0.98f);
		const uint32_t x = static_cast<uint32_t>(pixel.x * static_cast<float>(image.GetWidth()));
		const uint32_t y = static_cast<uint32_t>(pixel.y * static_cast<float>(image.GetHeight()));
		float total = 0;
		for (uint32_t dy = y - 1; dy <= y + 1; ++dy)
			for (uint32_t dx = x - 1; dx <= x + 1; ++dx)
				total += image.GetPixel(dx, dy).r;
		return total / (255.0f * 9.0f);
	}

	void Configure(SceneRenderer& renderer, RenderPath path)
	{
		auto& settings = renderer.GetSettings();
		settings.Path = path;
		settings.ShadowResolution = ShadowQuality::Low;
		settings.LocalShadowResolution = LocalShadowQuality::Low;
		settings.MaxShadowedLocalLights = 1;
		settings.LocalShadowSoftness = 0;
		settings.GIEnabled = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.DebugView = RenderDebugView::LocalShadow;
	}

	class FailingShadowFactory : public LocalShadowResourceFactory
	{
	public:
		int Calls = 0, FailAt = -1;
		int PipelineCalls = 0, FailPipeline = -1;
		nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
		{
			return PipelineCalls++ == FailPipeline ? nullptr : device->createGraphicsPipeline(desc, info);
		}
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			return Calls++ == FailAt ? nullptr : device->createTexture(desc);
		}
		nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
		{
			return Calls++ == FailAt ? nullptr : device->createFramebuffer(desc);
		}
	};

}

TEST_CASE("LocalShadow: point and spot shadows follow the caster and respect mesh and light flags")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			const auto camera = CameraView::LookAt({4, 6, 5}, {0, 0, 0}, glm::radians(60.0f), 1);
			for (int kind = 0; kind < 3; ++kind)
			{
				CAPTURE(kind);
				Scene scene("Local shadow oracle");
				AddMesh(scene, "Ground", AssetManager::BuiltinPlane, {});
				auto cube = AddMesh(scene, "Caster", AssetManager::BuiltinCube, {0, 0.5f, 0});
				auto lamp = scene.CreateEntity("Lamp");
				lamp.GetTransform().Translation = {-3, 3, 0};
				bool* casts;
				if (kind == 0)
				{
					auto& light = lamp.AddComponent<PointLightComponent>();
					light.Range = 20;
					casts = &light.CastShadows;
				}
				else
				{
					lamp.GetTransform().Rotation = glm::quat_cast(glm::inverse(glm::lookAtRH(glm::vec3(-3, 3, 0), glm::vec3(0), glm::vec3(0, 1, 0))));
					auto& light = lamp.AddComponent<SpotLightComponent>();
					light.Range = 20;
					light.InnerConeAngle = glm::radians(20.0f);
					light.OuterConeAngle = glm::radians(kind == 1 ? 45.0f : 85.0f);
					casts = &light.CastShadows;
				}
				*casts = true;
				auto render = [&] {
					renderer.Render(scene, camera, output.Framebuffer);
					return ReadbackTexture(graphics.GetDevice(), output.Texture);
				};
				const Image shadow = render();
				REQUIRE(shadow.SavePNG(Testing::GetTestOutputDirectory() / ("LocalShadow-" + std::to_string(kind) + ".png")));
				CHECK(renderer.GetStats().ShadowedLocalLights == 1);
				CHECK(renderer.GetStats().LocalShadowLayers == (kind == 1 ? 1 : 6));
				CHECK(renderer.GetStats().LocalShadowDrawCalls > 0);
				CHECK(SampleWorld(shadow, camera, {1, 0, 0}) < 0.1f);
				CHECK(SampleWorld(shadow, camera, {2, 0, 2}) > 0.95f);
				renderer.GetSettings().DebugView = RenderDebugView::DirectDiffuse;
				const float shadowDiffuse = SampleWorld(render(), camera, {1, 0, 0});
				*casts = false;
				const float litDiffuse = SampleWorld(render(), camera, {1, 0, 0});
				CHECK(litDiffuse > 0.03f);
				CHECK(shadowDiffuse < litDiffuse * 0.25f);
				*casts = true;
				renderer.GetSettings().DebugView = RenderDebugView::LocalShadow;
				const auto allocations = renderer.GetStats().LocalShadowAllocationAttempts;
				const Image repeated = render();
				CHECK(Image::MaxChannelDifference(shadow, repeated) == 0);
				CHECK(renderer.GetStats().LocalShadowAllocationAttempts == allocations);
				CHECK(renderer.GetStats().StaticLightingGenerations == 0);
				cube.GetTransform().Translation.z = -3;
				CHECK(SampleWorld(render(), camera, {1, 0, 0}) > 0.95f);
				cube.GetTransform().Translation.z = 0;
				cube.GetComponent<MeshComponent>().CastShadows = false;
				CHECK(SampleWorld(render(), camera, {1, 0, 0}) > 0.95f);
				cube.GetComponent<MeshComponent>().CastShadows = true;
				*casts = false;
				CHECK(SampleWorld(render(), camera, {1, 0, 0}) > 0.95f);
				CHECK(renderer.GetStats().ShadowedLocalLights == 0);
				*casts = true;
				renderer.GetSettings().Shadows = false;
				CHECK(SampleWorld(render(), camera, {1, 0, 0}) > 0.95f);
				renderer.GetSettings().Shadows = true;
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadowMaps: every partial allocation failure falls back and retries without stable-frame churn")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const uint32_t errors = graphics.GetValidationErrorCount();
	{
		FailingShadowFactory factory;
		LocalShadowMaps maps(graphics.GetDevice(), graphics.GetInfo().Limits, &factory);
		auto commands = graphics.GetDevice()->createCommandList();
		commands->open();
		REQUIRE(maps.Initialize(commands));
		commands->close();
		graphics.GetDevice()->executeCommandList(commands);
		graphics.WaitForIdle();
		auto* fallback = maps.GetTexture();
		for (int failure = 0; failure < 7; ++failure)
		{
			CAPTURE(failure);
			factory.Calls = 0;
			factory.FailAt = failure;
			CHECK_FALSE(maps.Ensure(512, 1));
			CHECK(maps.GetLayers() == 0);
			CHECK(maps.GetFramebuffer(0) == nullptr);
			CHECK(maps.GetTexture() == fallback);
			factory.FailAt = -1;
			REQUIRE(maps.Ensure(512, 1));
			CHECK(maps.GetLayers() == 6);
			CHECK(maps.GetFramebuffer(5) != nullptr);
			CHECK(maps.GetFramebuffer(6) == nullptr);
			const int calls = factory.Calls;
			CHECK_FALSE(maps.NeedsUpdate(512, 1));
			REQUIRE(maps.Ensure(512, 1));
			CHECK(factory.Calls == calls);
			REQUIRE(maps.Ensure(512, 0));
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("LocalShadow: cube face edges and corners keep the same blocked receiver")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			renderer.GetSettings().LocalShadowSoftness = 0.08f;
			REQUIRE(renderer.Initialize());
			Scene scene("Cube face seams");
			auto receiver = AddMesh(scene, "Receiver", AssetManager::BuiltinPlane, {});
			auto caster = AddMesh(scene, "Caster", AssetManager::BuiltinCube, {}, glm::vec3(0.35f));
			auto& light = scene.CreateEntity("Light").AddComponent<PointLightComponent>();
			light.Range = 10;
			light.CastShadows = true;
			for (int x = -1; x <= 1; ++x)
				for (int y = -1; y <= 1; ++y)
					for (int z = -1; z <= 1; ++z)
					{
						if (x == 0 && y == 0 && z == 0)
							continue;
						CAPTURE(x);
						CAPTURE(y);
						CAPTURE(z);
						const glm::vec3 direction = glm::normalize(glm::vec3(x, y, z));
						const glm::vec3 tangent = glm::normalize(glm::cross(direction, std::abs(direction.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
						const glm::vec3 point = direction * 3.0f;
						receiver.GetTransform().Translation = point;
						receiver.GetTransform().Rotation = glm::rotation(glm::vec3(0, 1, 0), -direction);
						caster.GetTransform().Translation = direction;
						const auto camera = CameraView::LookAt(point - direction * 2.0f + tangent * 2.0f, point, glm::radians(60.0f), 1);
						renderer.Render(scene, camera, output.Framebuffer);
						const auto image = ReadbackTexture(graphics.GetDevice(), output.Texture);
						CHECK(SampleWorld(image, camera, point) < 0.1f);
						CHECK(SampleWorld(image, camera, point + tangent * 1.2f) > 0.95f);
					}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: casters closer than the near plane cast while geometry behind the light does not")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("Near light");
			AddMesh(scene, "Receiver", AssetManager::BuiltinPlane, {0, -3, 0});
			auto caster = AddMesh(scene, "Near caster", AssetManager::BuiltinCube, {0, -0.025f, 0}, glm::vec3(0.012f));
			auto& light = scene.CreateEntity("Light").AddComponent<PointLightComponent>();
			light.Range = 10;
			light.CastShadows = true;
			const auto camera = CameraView::LookAt({2, -1, 0}, {0, -3, 0}, glm::radians(60.0f), 1);
			auto render = [&] {
				renderer.Render(scene, camera, output.Framebuffer);
				return ReadbackTexture(graphics.GetDevice(), output.Texture);
			};
			CHECK(SampleWorld(render(), camera, {0, -3, 0}) < 0.1f);
			caster.GetTransform().Translation.y = 0.025f;
			CHECK(SampleWorld(render(), camera, {0, -3, 0}) > 0.95f);
			caster.GetTransform().Translation.y = 0;
			CHECK(SampleWorld(render(), camera, {0, -3, 0}) < 0.1f);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: unobstructed sloping receivers remain lit at every quality and filter width")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("Acne oracle");
			AddMesh(scene, "Ground", AssetManager::BuiltinPlane, {});
			auto lamp = scene.CreateEntity("Light");
			auto& light = lamp.AddComponent<PointLightComponent>();
			light.Range = 100;
			light.CastShadows = true;
			const auto camera = CameraView::LookAt({4, 6, 5}, {0, 0, 0}, glm::radians(60.0f), 1);
			renderer.GetSettings().Shadows = false;
			renderer.Render(scene, camera, output.Framebuffer);
			const Image baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.GetSettings().Shadows = true;
			for (auto quality : {LocalShadowQuality::Low, LocalShadowQuality::Medium, LocalShadowQuality::High})
				for (float softness : {0.0f, 0.5f})
					for (float distance : {1.0f, 20.0f})
					{
						CAPTURE(static_cast<int>(quality));
						CAPTURE(softness);
						CAPTURE(distance);
						renderer.GetSettings().LocalShadowResolution = quality;
						renderer.GetSettings().LocalShadowSoftness = softness;
						lamp.GetTransform().Translation = {-distance, distance * 0.4f, 0};
						renderer.Render(scene, camera, output.Framebuffer);
						const Image image = ReadbackTexture(graphics.GetDevice(), output.Texture);
						CHECK(Image::MaxChannelDifference(baseline, image) <= 3);
					}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: two-sided mirrored and alpha-masked casters work while blended surfaces do not cast")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("Caster materials");
			AddMesh(scene, "Receiver", AssetManager::BuiltinPlane, {});
			auto plate = AddMesh(scene, "Plate", AssetManager::BuiltinPlane, {0, 1, 0}, glm::vec3(0.2f));
			auto material = CreateRef<MaterialAsset>();
			material->Handle = AssetHandle(50101);
			assets.AddMemoryAsset(material);
			plate.GetComponent<MeshComponent>().Materials = {material->Handle};
			auto lamp = scene.CreateEntity("Light");
			lamp.GetTransform().Translation = {0, 3, 0};
			auto& light = lamp.AddComponent<PointLightComponent>();
			light.CastShadows = true;
			light.Range = 10;
			// The camera is below the plate, isolating its cast shadow from the visible caster.
			const auto camera = CameraView::LookAt({0, 0.6f, 0}, {}, glm::radians(60.0f), 1);
			auto render = [&] {
				renderer.Render(scene, camera, output.Framebuffer);
				return ReadbackTexture(graphics.GetDevice(), output.Texture);
			};
			const Image solid = render();
			CHECK(SampleWorld(solid, camera, {}) < 0.01f);
			plate.GetTransform().Scale.x *= -1;
			CHECK(Image::MaxChannelDifference(solid, render()) == 0);
			plate.GetTransform().Rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(1, 0, 0));
			CHECK(Image::MaxChannelDifference(solid, render()) == 0);
			std::vector<uint8_t> pixels(64 * 64 * 4, 255);
			for (uint32_t y = 0; y < 64; ++y)
				for (uint32_t x = 0; x < 64; ++x)
					pixels[(y * 64 + x) * 4 + 3] = ((x / 4 + y / 4) % 2) == 0 ? 255 : 0;
			auto texture = TextureAsset::CreateRGBA8(64, 64, pixels.data(), true);
			texture->Handle = AssetHandle(50102);
			assets.AddMemoryAsset(texture);
			material->BaseColorTexture = texture->Handle;
			material->Alpha = AlphaMode::Mask;
			++material->Version;
			const Image masked = render();
			uint32_t blocked = 0;
			for (uint32_t y = 0; y < 128; ++y)
				for (uint32_t x = 0; x < 128; ++x)
					blocked += masked.GetPixel(x, y).r < 128 ? 1u : 0u;
			const float fraction = static_cast<float>(blocked) / (128 * 128);
			CHECK(fraction > 0.35f);
			CHECK(fraction < 0.65f);
			REQUIRE(masked.SavePNG(Testing::GetTestOutputDirectory() / "LocalShadow-AlphaMask.png"));
			material->Alpha = AlphaMode::Blend;
			++material->Version;
			const Image blended = render();
			light.CastShadows = false;
			CHECK(Image::MaxChannelDifference(blended, render()) == 0);
			CHECK(SampleWorld(blended, camera, {}) > 0.99f);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: wide spot cone edges stay lit on tilted unobstructed receivers")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			renderer.GetSettings().LocalShadowSoftness = 0.5f;
			REQUIRE(renderer.Initialize());
			Scene scene("Spot cone edge");
			const glm::vec3 position(std::tan(glm::radians(74.5f)), 0, -1);
			auto receiver = AddMesh(scene, "Receiver", AssetManager::BuiltinPlane, position);
			auto& light = scene.CreateEntity("Light").AddComponent<SpotLightComponent>();
			light.CastShadows = true;
			light.Range = 20;
			light.InnerConeAngle = glm::radians(74.0f);
			light.OuterConeAngle = glm::radians(75.0f);
			const auto camera = CameraView::LookAt(position + glm::vec3(0, 0, 2), position, glm::radians(40.0f), 1);
			for (float tilt : {0.0f, 5.0f, 40.0f})
			{
				CAPTURE(tilt);
				receiver.GetTransform().Rotation = glm::angleAxis(glm::radians(tilt), glm::vec3(0, 1, 0)) * glm::rotation(glm::vec3(0, 1, 0), glm::vec3(0, 0, 1));
				renderer.GetSettings().Shadows = false;
				renderer.Render(scene, camera, output.Framebuffer);
				const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
				renderer.GetSettings().Shadows = true;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) <= 3);
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: renderer falls back after each allocation failure and recovers on the next frame")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			FailingShadowFactory factory;
			SceneRenderer renderer(graphics, assets, &factory);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("Allocation recovery");
			AddMesh(scene, "Ground", AssetManager::BuiltinPlane, {});
			AddMesh(scene, "Caster", AssetManager::BuiltinCube, {0, 0.5f, 0});
			auto lamp = scene.CreateEntity("Lamp");
			lamp.GetTransform().Translation = {-3, 3, 0};
			auto& light = lamp.AddComponent<PointLightComponent>();
			light.Range = 20;
			light.CastShadows = true;
			const auto camera = CameraView::LookAt({4, 6, 5}, {}, glm::radians(60.0f), 1);
			renderer.GetSettings().Shadows = false;
			renderer.Render(scene, camera, output.Framebuffer);
			const Image baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.GetSettings().Shadows = true;
			for (int failure = 0; failure < 7; ++failure)
			{
				CAPTURE(failure);
				// Change quality with a populated old pool: failure must release it for the retry.
				renderer.GetSettings().LocalShadowResolution = failure % 2 == 0 ? LocalShadowQuality::Low : LocalShadowQuality::Medium;
				factory.Calls = 0;
				factory.FailAt = failure;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().ShadowedLocalLights == 0);
				CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
				factory.FailAt = -1;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().ShadowedLocalLights == 1);
				CHECK(SampleWorld(ReadbackTexture(graphics.GetDevice(), output.Texture), camera, {1, 0, 0}) < 0.1f);
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: invalid lights are rejected without assertions or GPU errors")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("Hostile lights");
			auto lamp = scene.CreateEntity("Point");
			auto& point = lamp.AddComponent<PointLightComponent>();
			point.CastShadows = true;
			const auto camera = CameraView::LookAt({0, 1, 4}, {}, glm::radians(60.0f), 1);
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float inf = std::numeric_limits<float>::infinity();
			for (float range : {0.0f, -1.0f, nan, inf, 1e30f})
			{
				point.Range = range;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().Lights == 0);
				CHECK(renderer.GetStats().ShadowedLocalLights == 0);
			}
			point.Range = 1e-6f;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().Lights == 1);
			CHECK(renderer.GetStats().ShadowedLocalLights == 0);
			point.Range = 10;
			point.Intensity = nan;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().Lights == 0);
			point.Intensity = 1;
			lamp.GetTransform().Translation.x = inf;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().Lights == 0);
			lamp.GetTransform().Translation.x = 0;
			lamp.RemoveComponent<PointLightComponent>();
			auto& spot = lamp.AddComponent<SpotLightComponent>();
			spot.CastShadows = true;
			for (float angle : {0.0f, -1.0f, nan, inf, glm::pi<float>()})
			{
				spot.OuterConeAngle = angle;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().Lights == 0);
			}
			spot.OuterConeAngle = glm::radians(89.0f);
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().ShadowedLocalLights == 1);
			spot.OuterConeAngle = 0.000001f;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().ShadowedLocalLights == 1);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: settings round trip and reject values outside their documented ranges")
{
	SceneRendererSettings settings;
	const auto fields = SceneRendererSettings::GetFields();
	std::string error;
	for (const auto& [name, value] : std::vector<std::pair<std::string, nlohmann::json>>{{"LocalShadowResolution", "High"}, {"MaxShadowedLocalLights", 16}, {"LocalShadowSoftness", 0.5}, {"DebugView", "LocalShadow"}})
	{
		const auto* field = FindField(fields, name);
		REQUIRE(field);
		REQUIRE(Reflection::SetField(*field, &settings, value, error));
	}
	SceneRendererSettings restored;
	REQUIRE(Reflection::FromJson(fields, "Settings", &restored, Reflection::ToJson(fields, &settings), error));
	CHECK(restored.GetLocalShadowMapSize() == 2048);
	CHECK(restored.MaxShadowedLocalLights == 16);
	CHECK(restored.LocalShadowSoftness == 0.5f);
	CHECK(restored.DebugView == RenderDebugView::LocalShadow);
	for (const auto& [name, value] : std::vector<std::pair<std::string, nlohmann::json>>{{"LocalShadowResolution", "Ultra"}, {"MaxShadowedLocalLights", 17}, {"MaxShadowedLocalLights", -1}, {"LocalShadowSoftness", 0.6}, {"LocalShadowSoftness", -0.01}})
		CHECK_FALSE(Reflection::SetField(*FindField(fields, name), &settings, value, error));
}

TEST_CASE("LocalShadowMaps: missing device and command list are verified preconditions")
{
	Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(LocalShadowMaps(nullptr, {}), Testing::AssertionFailedError);
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	LocalShadowMaps maps(graphics.GetDevice(), graphics.GetInfo().Limits);
	CHECK_THROWS_AS(maps.Initialize(nullptr), Testing::AssertionFailedError);
}

TEST_CASE("LocalShadow: failed opaque or masked pipeline retries without reallocating the depth pool")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		for (int failure = 0; failure < 2; ++failure)
		{
			CAPTURE(failure);
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			FailingShadowFactory factory;
			factory.FailPipeline = failure;
			SceneRenderer renderer(graphics, assets, &factory);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("Pipeline recovery");
			auto& light = scene.CreateEntity("Light").AddComponent<PointLightComponent>();
			light.CastShadows = true;
			const auto camera = CameraView::LookAt({0, 1, 4}, {}, glm::radians(60.0f), 1);
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().ShadowedLocalLights == 0);
			const int allocations = factory.Calls;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().ShadowedLocalLights == 1);
			CHECK(factory.PipelineCalls == 3);
			CHECK(factory.Calls == allocations);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: a triangle crossing the near plane keeps its true depth beyond the clamp")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			auto mesh = CreateRef<MeshAsset>();
			mesh->Handle = AssetHandle(50200);
			for (glm::vec3 position : {glm::vec3(0, 0, -0.025f), glm::vec3(1, 0, -1), glm::vec3(0, 1, -1)})
			{
				MeshVertex vertex;
				vertex.Position = position;
				mesh->Vertices.push_back(vertex);
			}
			mesh->Indices = {0, 1, 2};
			mesh->Materials = {AssetHandle(AssetManager::BuiltinGroundMaterial)};
			auto& submesh = mesh->Submeshes.emplace_back();
			submesh.IndexCount = submesh.VertexCount = 3;
			mesh->ComputeBounds();
			assets.AddMemoryAsset(mesh);
			Scene scene("Crossing near plane");
			AddMesh(scene, "Triangle", uint64_t(mesh->Handle), {});
			// Barycentric (.9,.05,.05) hits the triangle at z=-.1225. This receiver lies behind
			// that hit but in front of the incorrect z=-.223 produced by vertex-only depth clamping.
			const glm::vec3 point = glm::vec3(0.05f, 0.05f, -0.1225f) * (0.2f / 0.1225f);
			auto receiver = AddMesh(scene, "Receiver", AssetManager::BuiltinPlane, point, glm::vec3(0.02f));
			receiver.GetTransform().Rotation = glm::rotation(glm::vec3(0, 1, 0), glm::vec3(0, 0, 1));
			auto& light = scene.CreateEntity("Light").AddComponent<PointLightComponent>();
			light.Range = 10;
			light.CastShadows = true;
			const auto camera = CameraView::LookAt(point + glm::vec3(-0.15f, -0.15f, 0.1f), point, glm::radians(40.0f), 1, 0.01f, 10);
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			renderer.Render(scene, camera, output.Framebuffer);
			const Image shadow = ReadbackTexture(graphics.GetDevice(), output.Texture);
			CHECK(SampleWorld(shadow, camera, point) < 0.1f);
			light.CastShadows = false;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(SampleWorld(ReadbackTexture(graphics.GetDevice(), output.Texture), camera, point) > 0.95f);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: asymmetric plate footprint matches pinhole projection on all four sides")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("Pinhole footprint");
			AddMesh(scene, "Ground", AssetManager::BuiltinPlane, {});
			AddMesh(scene, "Plate", AssetManager::BuiltinPlane, {0.35f, 1, -0.2f}, {0.12f, 1, 0.07f});
			auto lamp = scene.CreateEntity("Light");
			lamp.GetTransform().Translation = {0, 3, 0};
			auto& light = lamp.AddComponent<PointLightComponent>();
			light.CastShadows = true;
			light.Range = 20;
			const auto camera = CameraView::LookAt({4, 6, 5}, {}, glm::radians(60.0f), 1);
			renderer.Render(scene, camera, output.Framebuffer);
			const Image image = ReadbackTexture(graphics.GetDevice(), output.Texture);
			// Lamp height 3, plate height 1: projected scale is 3/(3-1)=1.5.
			const glm::vec3 center(0.525f, 0, -0.3f);
			for (glm::vec3 offset : {glm::vec3(-0.7f, 0, 0), glm::vec3(0.7f, 0, 0), glm::vec3(0, 0, -0.3f), glm::vec3(0, 0, 0.3f)})
				CHECK(SampleWorld(image, camera, center + offset) < 0.1f);
			for (glm::vec3 offset : {glm::vec3(-1.1f, 0, 0), glm::vec3(1.1f, 0, 0), glm::vec3(0, 0, -0.75f), glm::vec3(0, 0, 0.75f)})
				CHECK(SampleWorld(image, camera, center + offset) > 0.95f);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("LocalShadow: captures ignore viewport retention and scene copies reset the retained selection")
{
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		CAPTURE(static_cast<int>(path));
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			ShadowOutput output(graphics.GetDevice());
			SceneRenderer renderer(graphics, assets);
			Configure(renderer, path);
			REQUIRE(renderer.Initialize());
			Scene scene("View retention");
			AddMesh(scene, "Ground", AssetManager::BuiltinPlane, {});
			AddMesh(scene, "Caster", AssetManager::BuiltinCube, {0, 0.5f, 0});
			for (uint64_t id : {101u, 102u})
			{
				auto lamp = scene.CreateEntityWithUUID(UUID(id), "Light");
				lamp.GetTransform().Translation = {id == 101 ? -1.0f : 1.0f, 3, 0};
				auto& light = lamp.AddComponent<PointLightComponent>();
				light.CastShadows = true;
				light.Range = 20;
			}
			const auto cameraA = CameraView::LookAt({-0.2f, 4, 4}, {}, glm::radians(60.0f), 1);
			const auto cameraB = CameraView::LookAt({0.1f, 4, 4}, {}, glm::radians(60.0f), 1);
			LocalShadowHistory history;
			for (int frame = 0; frame < 3; ++frame)
				renderer.Render(scene, cameraA, output.Framebuffer, &history);
			renderer.Render(scene, cameraB, output.Framebuffer, &history);
			REQUIRE(renderer.GetStats().ShadowedLocalLightIDs.size() == 1);
			CHECK(renderer.GetStats().ShadowedLocalLightIDs[0] == 101);
			const auto originalIdentity = history.SceneIdentity;
			renderer.Render(scene, cameraB, output.Framebuffer);
			const Image capture = ReadbackTexture(graphics.GetDevice(), output.Texture);
			CHECK(renderer.GetStats().ShadowedLocalLightIDs[0] == 102);
			{
				SceneRenderer fresh(graphics, assets);
				Configure(fresh, path);
				REQUIRE(fresh.Initialize());
				fresh.Render(scene, cameraB, output.Framebuffer);
				CHECK(Image::MaxChannelDifference(capture, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			}
			auto replacement = Scene::Copy(scene);
			REQUIRE(replacement);
			renderer.Render(*replacement, cameraB, output.Framebuffer, &history);
			CHECK(history.SceneIdentity != originalIdentity);
			CHECK(renderer.GetStats().ShadowedLocalLightIDs[0] == 102);
			CHECK(Image::MaxChannelDifference(capture, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}
