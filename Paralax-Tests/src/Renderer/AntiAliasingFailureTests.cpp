#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

using namespace Paralax;

namespace {

	class FailureFactory : public PostEffectResourceFactory
	{
	public:
		bool Resources = true;
		uint32_t Calls = 0, FailAt = 0;
		bool Fail(bool resource) { return resource == Resources && ++Calls == FailAt; }
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			return Fail(true) ? nullptr : PostEffectResourceFactory::CreateTexture(device, desc);
		}
		nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc) override
		{
			return Fail(true) ? nullptr : PostEffectResourceFactory::CreateBuffer(device, desc);
		}
		nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc) override
		{
			return Fail(false) ? nullptr : PostEffectResourceFactory::CreateBindingLayout(device, desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			return Fail(false) ? nullptr : PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
		}
		nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
		{
			return Fail(false) ? nullptr : PostEffectResourceFactory::CreateFramebuffer(device, desc);
		}
		nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
		{
			return Fail(false) ? nullptr : PostEffectResourceFactory::CreatePipeline(device, desc, info);
		}
		nvrhi::SamplerHandle CreateSampler(nvrhi::IDevice* device, const nvrhi::SamplerDesc& desc) override
		{
			return Fail(false) ? nullptr : PostEffectResourceFactory::CreateSampler(device, desc);
		}
	};

	struct Output
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
		explicit Output(nvrhi::IDevice* device)
		{
			nvrhi::TextureDesc desc;
			desc.width = desc.height = 17;
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Texture = device->createTexture(desc);
			Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Texture));
		}
	};

	void EveryTemporalFailure(bool resources)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto errors = graphics.GetValidationErrorCount();
		{
			FailureFactory factory;
			factory.Resources = resources;
			AssetManager assets;
			Scene scene("AAFailure");
			scene.CreateEntity("Cube").AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
			SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.Shadows = settings.GIEnabled = false;
			settings.AntiAliasing = AntiAliasingMode::Temporal;
			Output output(graphics.GetDevice());
			const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
			renderer.Render(scene, camera, output.Framebuffer);
			renderer.Render(scene, camera, output.Framebuffer);
			auto& viewport = renderer.GetDefaultTemporalHistory().GetState();
			REQUIRE(viewport.GetCompletedFrames() == 2);
			uint32_t count = 0;
			{
				auto capture = renderer.CreateTemporalHistory();
				factory.Calls = 0;
				renderer.Render(scene, camera, output.Framebuffer, nullptr, capture.get());
				REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
				count = factory.Calls;
			}
			REQUIRE(count > 0);
			for (uint32_t fail = 1; fail <= count; ++fail)
			{
				CAPTURE(fail);
				auto capture = renderer.CreateTemporalHistory();
				factory.Calls = 0;
				factory.FailAt = fail;
				renderer.Render(scene, camera, output.Framebuffer, nullptr, capture.get());
				CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Spatial);
				CHECK_FALSE(renderer.GetStats().AAFallbackReason.empty());
				CHECK_FALSE(capture->GetState().IsValid());
				CHECK(capture->GetState().GetCompletedFrames() == 0);
				CHECK(capture->GetState().GetSampleIndex() == 0);
				CHECK(capture->GetOwnedBytes() == 0);
				CHECK(viewport.IsValid());
				CHECK(viewport.GetCompletedFrames() == 2);
				CHECK(viewport.GetSampleIndex() == 2);
				factory.FailAt = 0;
				renderer.Render(scene, camera, output.Framebuffer, nullptr, capture.get());
				CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
				CHECK(capture->GetState().GetCompletedFrames() == 1);
				CHECK(capture->GetState().GetSampleIndex() == 1);
			}
			settings.AntiAliasing = AntiAliasingMode::Off;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}

}

TEST_CASE("TemporalAA: every texture and buffer allocation failure preserves viewport state and retries without partial history")
{
	EveryTemporalFailure(true);
}

TEST_CASE("TemporalAA: every layout binding framebuffer and pipeline failure preserves viewport state and retries without partial history")
{
	EveryTemporalFailure(false);
}

TEST_CASE("SpatialAA: every allocation failure falls back without jitter and recovers with no leaked effect budget")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Output output(graphics.GetDevice());
		for (bool resources : {false, true})
		{
			FailureFactory factory;
			factory.Resources = resources;
			uint32_t count = 0;
			PostEffectBudget budget;
			{
				SpatialAA aa(graphics, budget, &factory);
				REQUIRE(aa.Prepare(true, output.Framebuffer));
				count = factory.Calls;
			}
			REQUIRE(count > 0);
			for (uint32_t fail = 1; fail <= count; ++fail)
			{
				CAPTURE(resources);
				CAPTURE(fail);
				SpatialAA aa(graphics, budget, &factory);
				factory.Calls = 0;
				factory.FailAt = fail;
				CHECK_FALSE(aa.Prepare(true, output.Framebuffer));
				CHECK(budget.GetUsedBytes() == 0);
				factory.FailAt = 0;
				CHECK(aa.Prepare(true, output.Framebuffer));
				aa.Release();
				CHECK(budget.GetUsedBytes() == 0);
			}
		}
		AssetManager assets;
		Scene scene("NoAABudget");
		scene.CreateEntity("Cube").AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, nullptr, 1);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = settings.GIEnabled = false;
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(scene, camera, output.Framebuffer);
		const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		for (int frame = 0; frame < 2; ++frame)
		{
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Off);
			CHECK(renderer.GetStats().AACompletedFrames == 0);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AntiAliasing: rendering unprepared resources verifies preconditions in every configuration")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		TemporalAA temporal(graphics, budget);
		SpatialAA spatial(graphics, budget);
		CHECK_FALSE(temporal.Prepare(nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr));
		CHECK_FALSE(spatial.Prepare(true, nullptr));
		CHECK(budget.GetUsedBytes() == 0);
		Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(temporal.UploadAndClear(nullptr, {}), Testing::AssertionFailedError);
		CHECK_THROWS_AS(temporal.Resolve(nullptr, glm::mat4(1), 0.9f), Testing::AssertionFailedError);
		CHECK_THROWS_AS(temporal.Commit(), Testing::AssertionFailedError);
		CHECK_THROWS_AS(spatial.Render(nullptr, nullptr), Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SpatialAA: output depth attachment changes require a compatible pipeline in both directions")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		Output output(device);
		auto desc = output.Texture->getDesc();
		desc.format = nvrhi::Format::D32;
		desc.initialState = nvrhi::ResourceStates::DepthWrite;
		auto depth = device->createTexture(desc);
		auto withDepth = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output.Texture).setDepthAttachment(depth));
		REQUIRE(withDepth);
		PostEffectBudget budget;
		SpatialAA aa(graphics, budget);
		Image reference;
		for (auto* target : {output.Framebuffer.Get(), withDepth.Get(), output.Framebuffer.Get()})
		{
			REQUIRE(aa.Prepare(true, target));
			auto commands = device->createCommandList();
			commands->open();
			auto* input = aa.GetFramebuffer()->getDesc().colorAttachments[0].texture;
			PostEffectTextures::TrackUsage(commands, input, nvrhi::ResourceStates::CopyDest);
			commands->clearTextureFloat(input, nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.5f, 0.75f, 1));
			PostEffectTextures::TrackUsage(commands, depth, nvrhi::ResourceStates::DepthWrite);
			commands->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 0, false, 0);
			aa.Render(commands, target);
			commands->close();
			device->executeCommandList(commands);
			const auto image = ReadbackTexture(device, output.Texture);
			if (reference.IsEmpty())
				reference = image;
			else
				CHECK(Image::MaxChannelDifference(reference, image) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: persistent unsupported format or memory pressure retains Spatial without repeated allocation or lighting regeneration")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class Factory : public PostEffectResourceFactory
		{
		public:
			bool Unsupported = false;
			nvrhi::FormatSupport QueryFormatSupport(nvrhi::IDevice* device, nvrhi::Format format) override
			{
				return Unsupported && format == nvrhi::Format::R32_UINT ? nvrhi::FormatSupport::None : PostEffectResourceFactory::QueryFormatSupport(device, format);
			}
		} factory;
		AssetManager assets;
		Scene scene("PersistentAAFallback");
		scene.CreateEntity("Cube").AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
		Output output(graphics.GetDevice());
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		uint64_t temporalBytes = 0;
		{
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			renderer.GetSettings().GIEnabled = renderer.GetSettings().Shadows = false;
			renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
			renderer.Render(scene, camera, output.Framebuffer);
			temporalBytes = renderer.GetStats().EffectOwnedBytes;
		}
		REQUIRE(temporalBytes > 1);
		// Retiring another effect must invalidate the cached pressure decision. Keep a bloom
		// pyramid alive through the first failed Temporal request, then free it while fallback runs.
		{
			SceneRenderer renderer(graphics, assets, nullptr, nullptr, nullptr, temporalBytes);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.GIEnabled = settings.Shadows = false;
			settings.AntiAliasing = AntiAliasingMode::Spatial;
			settings.BloomEnabled = true;
			settings.BloomIntensity = 0.2f;
			settings.BloomLevels = 2;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().BloomActive);
			settings.AntiAliasing = AntiAliasingMode::Temporal;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Spatial);
			const auto attempts = renderer.GetStats().EffectAllocationAttempts;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
			settings.BloomEnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
		}
		{
			SceneRenderer renderer(graphics, assets, nullptr, nullptr, nullptr, temporalBytes);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.GIEnabled = settings.Shadows = false;
			settings.AntiAliasing = AntiAliasingMode::Spatial;
			settings.BloomEnabled = true;
			settings.BloomIntensity = 0.2f;
			settings.BloomLevels = 2;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().BloomActive);
			// Both settings change together: the just-disabled bloom cannot poison a new failure cache.
			settings.AntiAliasing = AntiAliasingMode::Temporal;
			settings.BloomEnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
			const auto attempts = renderer.GetStats().EffectAllocationAttempts;
			for (int frame = 0; frame < 2; ++frame)
			{
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
				CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
				CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			}
		}
		for (bool unsupported : {true, false})
		{
			factory.Unsupported = unsupported;
			SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory, unsupported ? PostEffectBudget::DefaultCapacity : temporalBytes - 1);
			REQUIRE(renderer.Initialize());
			renderer.GetSettings().GIEnabled = renderer.GetSettings().Shadows = false;
			renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Spatial);
			const auto attempts = renderer.GetStats().EffectAllocationAttempts;
			const auto owned = renderer.GetStats().EffectOwnedBytes;
			for (int frame = 0; frame < 3; ++frame)
			{
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Spatial);
				CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
				CHECK(renderer.GetStats().EffectOwnedBytes == owned);
				CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			}
			if (unsupported)
			{
				factory.Unsupported = false;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
