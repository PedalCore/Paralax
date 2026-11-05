#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace Paralax;

namespace {

	struct Output
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
		Output(nvrhi::IDevice* device, uint32_t width, uint32_t height, nvrhi::Format format = nvrhi::Format::SRGBA8_UNORM)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = format;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Texture = device->createTexture(desc);
			REQUIRE(Texture);
			Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Texture));
			REQUIRE(Framebuffer);
		}
	};

	Entity AddCube(Scene& scene, AssetManager& assets)
	{
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(59001);
		material->BaseColorFactor = {0.5f, 0.5f, 0.5f, 1};
		material->EmissiveFactor = {0.0031308f, 0.18f, 0.6f};
		material->MetallicFactor = 0.5f;
		material->RoughnessFactor = 0.25f;
		assets.AddMemoryAsset(material);
		auto entity = scene.CreateEntity("FXAACube");
		auto& mesh = entity.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		mesh.Materials = {material->Handle};
		return entity;
	}

	void Configure(SceneRenderer& renderer)
	{
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = settings.Shadows = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.Tonemapper = TonemapOperator::None;
	}

	void IntegratedEffects(RenderPath path, bool orthographic)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			Scene scene("FXAAEffects");
			AddCube(scene, assets);
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			Configure(renderer);
			auto& settings = renderer.GetSettings();
			settings.Path = path;
			settings.AntiAliasing = AntiAliasingMode::FXAA;
			settings.SSAOEnabled = settings.BloomEnabled = settings.SSREnabled = settings.SSRTemporalEnabled = true;
			settings.BloomIntensity = 0.2f;
			settings.BloomLevels = 2;
			Output output(graphics.GetDevice(), 33, 25);
			auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 33.0f / 25);
			if (orthographic)
			{
				camera.Orthographic = true;
				camera.Projection = glm::orthoRH_ZO(-1.32f, 1.32f, -1.0f, 1.0f, 10.0f, 0.1f);
			}
			uint64_t reflectedFrames = 0;
			for (float scale : {0.75f, 1.0f, 2.0f})
			{
				settings.InternalRenderScale = scale;
				for (int frame = 0; frame < 2; ++frame)
				{
					renderer.Render(scene, camera, output.Framebuffer);
					const auto& stats = renderer.GetStats();
					CHECK(stats.EffectivePath == path);
					CHECK(stats.EffectiveRenderScale == scale);
					CHECK(stats.EffectiveAA == AntiAliasingMode::FXAA);
					CHECK(stats.AAPasses == 1);
					CHECK_FALSE(stats.AAHistoryValid);
					CHECK(stats.AASampleIndex == 0);
					CHECK(stats.SSAOActive);
					CHECK(stats.BloomActive);
					CHECK(stats.SSRActive);
					CHECK(stats.SSRHistoryActive);
					CHECK(stats.SSRHistoryEligible == (frame != 0));
					CHECK(stats.SSRHistoryCompletedFrames == ++reflectedFrames);
				}
			}
			settings.DebugView = RenderDebugView::Normals;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Off);
			CHECK(renderer.GetStats().AAPasses == 0);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("FXAA: Forward perspective integrates scaled AO bloom and independent reflection history")
{
	IntegratedEffects(RenderPath::Forward, false);
}
TEST_CASE("FXAA: Forward orthographic integrates scaled AO bloom and independent reflection history")
{
	IntegratedEffects(RenderPath::Forward, true);
}
TEST_CASE("FXAA: Deferred perspective integrates scaled AO bloom and independent reflection history")
{
	IntegratedEffects(RenderPath::Deferred, false);
}
TEST_CASE("FXAA: Deferred orthographic integrates scaled AO bloom and independent reflection history")
{
	IntegratedEffects(RenderPath::Deferred, true);
}
TEST_CASE("FXAA: Visibility perspective integrates scaled AO bloom and independent reflection history")
{
	IntegratedEffects(RenderPath::Visibility, false);
}
TEST_CASE("FXAA: Visibility orthographic integrates scaled AO bloom and independent reflection history")
{
	IntegratedEffects(RenderPath::Visibility, true);
}

TEST_CASE("FXAA: native tonemap intermediate preserves constant linear and sRGB attachment colors")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("FXAAConstant");
		auto cube = AddCube(scene, assets);
		cube.GetTransform().Scale = {20, 20, 0.1f};
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 17.0f / 13);
		for (auto format : {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::SRGBA8_UNORM})
		{
			Output output(graphics.GetDevice(), 17, 13, format);
			renderer.GetSettings().AntiAliasing = AntiAliasingMode::Off;
			renderer.Render(scene, camera, output.Framebuffer);
			const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.GetSettings().AntiAliasing = AntiAliasingMode::FXAA;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::FXAA);
			CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) <= 1);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: resized input retires before an affordable supersampling allocation")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("FXAATransition");
		AddCube(scene, assets);
		Output small(graphics.GetDevice(), 33, 25), large(graphics.GetDevice(), 129, 97);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 33.0f / 25);
		uint64_t finalBytes = 0, oldBytes = 0;
		{
			SceneRenderer measure(graphics, assets);
			REQUIRE(measure.Initialize());
			Configure(measure);
			measure.GetSettings().AntiAliasing = AntiAliasingMode::FXAA;
			measure.GetSettings().InternalRenderScale = 2;
			measure.Render(scene, camera, small.Framebuffer);
			REQUIRE(measure.GetStats().EffectiveAA == AntiAliasingMode::FXAA);
			finalBytes = measure.GetStats().EffectOwnedBytes;
			measure.GetSettings().InternalRenderScale = 1;
			measure.Render(scene, camera, large.Framebuffer);
			oldBytes = measure.GetStats().EffectOwnedBytes;
		}
		const auto capacity = std::max(finalBytes, oldBytes);
		REQUIRE(finalBytes > 0);
		REQUIRE(oldBytes > 0);
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, nullptr, capacity);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto& settings = renderer.GetSettings();
		settings.AntiAliasing = AntiAliasingMode::FXAA;
		renderer.Render(scene, camera, large.Framebuffer);
		REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::FXAA);
		settings.InternalRenderScale = 2;
		for (int frame = 0; frame < 2; ++frame)
		{
			renderer.Render(scene, camera, small.Framebuffer);
			CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::FXAA);
			CHECK(renderer.GetStats().EffectiveRenderScale == 2);
			CHECK(renderer.GetStats().EffectOwnedBytes == finalBytes);
			CHECK(renderer.GetStats().EffectPeakBytes <= capacity);
		}
		settings.InternalRenderScale = 1;
		settings.AntiAliasing = AntiAliasingMode::Spatial;
		renderer.Render(scene, camera, small.Framebuffer);
		CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Spatial);
		settings.AntiAliasing = AntiAliasingMode::Off;
		renderer.Render(scene, camera, small.Framebuffer);
		CHECK(renderer.GetStats().EffectOwnedBytes == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: private capture preserves viewport temporal and reflection histories")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("FXAACaptureIsolation");
		AddCube(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto& settings = renderer.GetSettings();
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		settings.SSREnabled = settings.SSRTemporalEnabled = true;
		Output output(graphics.GetDevice(), 33, 25);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 33.0f / 25);
		renderer.Render(scene, camera, output.Framebuffer);
		renderer.Render(scene, camera, output.Framebuffer);
		auto& viewport = renderer.GetDefaultTemporalHistory().GetState();
		auto& reflection = renderer.GetDefaultReflectionHistory().GetState();
		REQUIRE(viewport.GetCompletedFrames() == 2);
		REQUIRE(reflection.GetCompletedFrames() == 2);
		{
			auto capture = renderer.CreateTemporalHistory();
			auto captureReflection = renderer.CreateReflectionHistory();
			settings.AntiAliasing = AntiAliasingMode::FXAA;
			for (uint32_t frame = 0; frame < 2; ++frame)
			{
				renderer.Render(scene, camera, output.Framebuffer, nullptr, capture.get(), captureReflection.get());
				CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::FXAA);
				CHECK_FALSE(capture->GetState().IsValid());
				CHECK(capture->GetOwnedBytes() == 0);
				CHECK(viewport.GetCompletedFrames() == 2);
				CHECK(viewport.GetSampleIndex() == 2);
				CHECK(viewport.IsValid());
				CHECK(reflection.GetCompletedFrames() == 2);
				CHECK(reflection.IsValid());
				CHECK(captureReflection->GetState().GetCompletedFrames() == frame + 1);
			}
		}
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(viewport.GetCompletedFrames() == 3);
		CHECK(reflection.GetCompletedFrames() == 3);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
