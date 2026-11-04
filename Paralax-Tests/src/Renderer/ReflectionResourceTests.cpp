#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <cmath>
#include <Paralax/Scene/Scene.h>

#include <algorithm>
#include <string>

using namespace Paralax;

namespace {

	struct ReflectionOutput
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
		explicit ReflectionOutput(nvrhi::IDevice* device, uint32_t size = 32)
		{
			nvrhi::TextureDesc desc;
			desc.width = desc.height = size;
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Texture = device->createTexture(desc);
			Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Texture));
		}
	};

	class ReflectionFactory : public PostEffectResourceFactory
	{
	public:
		std::string FailTexture;
		bool IndependentBlend = true;
		bool FailInspectionFramebuffer = false;
		nvrhi::InputLayoutHandle Input;
		nvrhi::BindingLayoutVector MaterialLayouts;
		nvrhi::TextureHandle Depth, BRDF;
		nvrhi::BufferHandle ViewConstants;
		bool SupportsIndependentBlend(const GraphicsDevice& graphics) override { return IndependentBlend && PostEffectResourceFactory::SupportsIndependentBlend(graphics); }
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			return !desc.isVirtual && desc.debugName == FailTexture ? nullptr : PostEffectResourceFactory::CreateTexture(device, desc);
		}
		nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
		{
			if (desc.inputLayout && desc.bindingLayouts.size() == 3)
			{
				Input = desc.inputLayout;
				MaterialLayouts = desc.bindingLayouts;
			}
			return PostEffectResourceFactory::CreatePipeline(device, desc, info);
		}
		nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
		{
			if (FailInspectionFramebuffer && !desc.colorAttachments.empty() && desc.colorAttachments[0].texture->getDesc().debugName == "ReflectionPixelInspection")
				return nullptr;
			if (desc.depthAttachment.texture)
				Depth = desc.depthAttachment.texture;
			return PostEffectResourceFactory::CreateFramebuffer(device, desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			bool reflection = false;
			for (const auto& binding : desc.bindings)
				if (binding.type == nvrhi::ResourceType::Texture_SRV && binding.slot == 22)
				{
					BRDF = static_cast<nvrhi::ITexture*>(binding.resourceHandle);
					reflection = true;
				}
			if (reflection)
				for (const auto& binding : desc.bindings)
					if ((binding.type == nvrhi::ResourceType::ConstantBuffer || binding.type == nvrhi::ResourceType::VolatileConstantBuffer) && binding.slot == 0)
						ViewConstants = static_cast<nvrhi::IBuffer*>(binding.resourceHandle);
			return PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
		}
	};

}

TEST_CASE("ScreenSpaceReflections: individual target failures preserve a reason release reservations and recover")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ReflectionFailure");
		ReflectionOutput output(graphics.GetDevice());
		ReflectionFactory factory;
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = false;
		const auto camera = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		for (const char* target : {"ReflectionExactEnvironmentSpecular", "ReflectionOpaqueRadiance", "ReflectionCandidate", "ReflectionHitWorld"})
		{
			CAPTURE(target);
			settings.SSREnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			factory.FailTexture = target;
			settings.SSREnabled = true;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK_FALSE(renderer.GetStats().SSRActive);
			CHECK_FALSE(renderer.GetStats().SSRFallbackReason.empty());
			CHECK(renderer.GetStats().SSROwnedBytes == 0);
			const auto attempts = renderer.GetStats().EffectAllocationAttempts;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
			settings.SSREnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			factory.FailTexture.clear();
			settings.SSREnabled = true;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().SSRActive);
			CHECK(renderer.GetStats().SSROwnedBytes > 0);
		}
		factory.IndependentBlend = false;
		settings.Path = RenderPath::Deferred;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK_FALSE(renderer.GetStats().SSRActive);
		CHECK(renderer.GetStats().SSRFallbackReason == "deferred reflections require independent attachment blending");
		CHECK(renderer.GetStats().EffectivePath == RenderPath::Deferred);
		CHECK(renderer.GetStats().SSROwnedBytes == 0);
		settings.Path = RenderPath::Forward;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().SSRActive);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: unchanged targets rebind replacement view constants and BRDF inputs")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	auto* device = graphics.GetDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ReflectionBindings");
		ReflectionOutput output(device);
		ReflectionFactory factory;
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().SSREnabled = true;
		renderer.GetSettings().GIEnabled = false;
		renderer.Render(scene, CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1), output.Framebuffer);
		REQUIRE(renderer.GetStats().SSRActive);
		REQUIRE(factory.ViewConstants);
		REQUIRE(factory.BRDF);
		REQUIRE(factory.MaterialLayouts.size() == 3);
		// Reuse actual renderer layouts, then verify the effect's captured bindings with new resources of the same shape.
		PostEffectBudget budget;
		ReflectionFactory capture;
		ScreenSpaceReflections reflections(graphics, budget, &capture);
		auto prepare = [&](nvrhi::IBuffer* constants, nvrhi::ITexture* brdf) {
			return reflections.Prepare(true, renderer.GetHDRTarget(), factory.Depth, {}, constants, brdf, factory.Input,
									   factory.MaterialLayouts[0], factory.MaterialLayouts[1], factory.MaterialLayouts[2], nullptr);
		};
		REQUIRE(prepare(factory.ViewConstants, factory.BRDF));
		const auto initial = reflections.GetAllocationAttempts();
		REQUIRE(prepare(factory.ViewConstants, factory.BRDF));
		CHECK(reflections.GetAllocationAttempts() == initial);
		auto brdf = device->createTexture(factory.BRDF->getDesc());
		REQUIRE(brdf);
		REQUIRE(prepare(factory.ViewConstants, brdf));
		CHECK(capture.BRDF == brdf);
		CHECK(reflections.GetAllocationAttempts() > initial);
		auto constantsDesc = factory.ViewConstants->getDesc();
		// Backend descriptors expose mapped CPU access, which is not a valid volatile creation request.
		if (constantsDesc.isVolatile)
			constantsDesc.cpuAccess = nvrhi::CpuAccessMode::None;
		auto constants = device->createBuffer(constantsDesc);
		REQUIRE(constants);
		REQUIRE(prepare(constants, brdf));
		CHECK(capture.ViewConstants == constants);
		CHECK_FALSE(prepare(constants, nullptr));
		CHECK_FALSE(reflections.GetFallbackReason().empty());
		CHECK(reflections.GetOwnedBytes() == 0);
		REQUIRE(prepare(constants, brdf));
		CHECK(capture.ViewConstants == constants);
		CHECK(capture.BRDF == brdf);
		reflections.Release();
		CHECK(budget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: disabling old resources before scale and AA allocation matches a fresh tight budget")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ReflectionBudgetTransition");
		ReflectionOutput output(graphics.GetDevice());
		const auto camera = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		uint64_t initial = 0, final = 0;
		for (bool ssr : {true, false})
		{
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.GIEnabled = false;
			settings.SSREnabled = ssr;
			settings.InternalRenderScale = ssr ? 1.0f : 2.0f;
			settings.AntiAliasing = ssr ? AntiAliasingMode::Off : AntiAliasingMode::Temporal;
			renderer.Render(scene, camera, output.Framebuffer);
			if (ssr)
			{
				REQUIRE(renderer.GetStats().SSRActive);
				initial = renderer.GetStats().EffectOwnedBytes;
			}
			else
			{
				REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
				final = renderer.GetStats().EffectOwnedBytes;
			}
		}
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, nullptr, std::max(initial, final));
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = false;
		settings.SSREnabled = true;
		renderer.Render(scene, camera, output.Framebuffer);
		REQUIRE(renderer.GetStats().SSRActive);
		settings.SSREnabled = false;
		settings.InternalRenderScale = 2;
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK_FALSE(renderer.GetStats().SSRActive);
		CHECK(renderer.GetStats().SSROwnedBytes == 0);
		CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
		CHECK(renderer.GetStats().EffectiveRenderScale == 2);
		CHECK(renderer.GetStats().EffectOwnedBytes == final);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: persistent Temporal AA budget fallback does not reallocate working reflections")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class Factory : public PostEffectResourceFactory
		{
		public:
			nvrhi::MemoryRequirements GetTextureMemoryRequirements(nvrhi::IDevice* device, nvrhi::ITexture* texture) override
			{
				auto requirements = PostEffectResourceFactory::GetTextureMemoryRequirements(device, texture);
				if (texture->getDesc().debugName == "TemporalMotionGuide")
					requirements.size = PostEffectBudget::DefaultCapacity;
				return requirements;
			}
		} factory;
		AssetManager assets;
		Scene scene("ReflectionStableFallback");
		ReflectionOutput output(graphics.GetDevice());
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.SSREnabled = true;
		settings.GIEnabled = false;
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		const auto camera = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		for (int i = 0; i < 3; ++i)
			renderer.Render(scene, camera, output.Framebuffer);
		REQUIRE(renderer.GetStats().SSRActive);
		REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Spatial);
		const auto attempts = renderer.GetStats().EffectAllocationAttempts;
		for (int i = 0; i < 3; ++i)
		{
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().SSRActive);
			CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
			CHECK(renderer.GetStats().AAFallbackReason == "renderer effect memory budget exhausted");
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ReflectionHistory: failed owners detach scratch across tight budget captures without retry churn")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class Factory : public PostEffectResourceFactory
		{
		public:
			uint64_t HistoryAttempts = 0;
			nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
			{
				if (!desc.isVirtual && desc.debugName == "ReflectionHistory")
				{
					++HistoryAttempts;
					return nullptr;
				}
				return PostEffectResourceFactory::CreateTexture(device, desc);
			}
		} factory;
		AssetManager assets;
		Scene scene("FailedHistoryCapture");
		ReflectionOutput small(graphics.GetDevice(), 32), large(graphics.GetDevice(), 64);
		const auto camera = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		uint64_t capacity = 0;
		{
			SceneRenderer reference(graphics, assets, nullptr, nullptr, &factory);
			REQUIRE(reference.Initialize());
			reference.GetSettings().GIEnabled = false;
			reference.GetSettings().SSREnabled = true;
			reference.Render(scene, camera, large.Framebuffer);
			REQUIRE(reference.GetStats().SSRActive);
			REQUIRE_FALSE(reference.GetStats().SSRHistoryActive);
			capacity = reference.GetStats().EffectOwnedBytes;
		}
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory, capacity);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().GIEnabled = false;
		renderer.GetSettings().SSREnabled = true;
		renderer.Render(scene, camera, small.Framebuffer);
		REQUIRE(renderer.GetStats().SSRActive);
		REQUIRE_FALSE(renderer.GetStats().SSRHistoryActive);
		const auto attempts = factory.HistoryAttempts;
		renderer.Render(scene, camera, small.Framebuffer);
		CHECK(factory.HistoryAttempts == attempts);
		auto aa = renderer.CreateTemporalHistory();
		auto history = renderer.CreateReflectionHistory();
		renderer.Render(scene, camera, large.Framebuffer, nullptr, aa.get(), history.get());
		REQUIRE(renderer.GetStats().SSRActive);
		CHECK_FALSE(renderer.GetStats().SSRHistoryActive);
		const auto backing = history->GetBackingAttempts();
		const auto privateAttempts = history->GetAllocationAttempts();
		for (uint32_t i = 0; i < 3; ++i)
		{
			renderer.Render(scene, camera, large.Framebuffer, nullptr, aa.get(), history.get());
			CHECK(renderer.GetStats().SSRActive);
			CHECK(history->GetBackingAttempts() == backing);
			CHECK(history->GetAllocationAttempts() == privateAttempts);
		}
		renderer.Render(scene, camera, small.Framebuffer);
		CHECK(renderer.GetStats().SSRActive);
		CHECK_FALSE(renderer.GetStats().SSRHistoryActive);
		CHECK(factory.HistoryAttempts > attempts);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: pixel inspection preserves output and history while rejecting stale inputs and allocation failures")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("Inspection");
		ReflectionOutput output(graphics.GetDevice());
		ReflectionFactory factory;
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		CHECK_FALSE(renderer.InspectReflectionPixel({0, 0}, 0));
		renderer.GetSettings().GIEnabled = false;
		renderer.GetSettings().SSREnabled = true;
		const auto camera = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(scene, camera, output.Framebuffer);
		const auto stats = renderer.GetStats();
		REQUIRE(stats.SSRActive);
		const auto historyFrames = renderer.GetDefaultReflectionHistory().GetState().GetCompletedFrames();
		const auto historyValid = renderer.GetDefaultReflectionHistory().GetState().IsValid();
		const auto historyBytes = renderer.GetDefaultReflectionHistory().GetOwnedBytes();
		const auto before = ReadbackTexture(graphics.GetDevice(), output.Texture);
		CHECK_FALSE(renderer.InspectReflectionPixel({32, 0}, stats.RenderFrame));
		CHECK_FALSE(renderer.InspectReflectionPixel({0, 32}, stats.RenderFrame));
		CHECK_FALSE(renderer.InspectReflectionPixel({0, 0}, stats.RenderFrame + 1));
		for (int i = 0; i < 3; ++i)
		{
			factory.FailTexture = "ReflectionPixelInspection";
			CHECK_FALSE(renderer.InspectReflectionPixel({7, 9}, stats.RenderFrame));
			factory.FailTexture.clear();
			factory.FailInspectionFramebuffer = true;
			CHECK_FALSE(renderer.InspectReflectionPixel({7, 9}, stats.RenderFrame));
			factory.FailInspectionFramebuffer = false;
			const auto probe = renderer.InspectReflectionPixel({7, 9}, stats.RenderFrame);
			REQUIRE(probe);
			CHECK(probe->Frame == stats.RenderFrame);
			CHECK(probe->Generation > 0);
			CHECK(probe->Pixel == glm::uvec2(7, 9));
			CHECK(probe->Size == glm::uvec2(32));
			CHECK(probe->Status == 0);
			CHECK(probe->Confidence == 0);
			CHECK(std::isnan(probe->Distance));
			CHECK(std::isnan(probe->CandidateNormal.x));
			CHECK(std::isnan(probe->TerminationNormal.z));
			CHECK(std::isnan(probe->CandidateNormalDot));
			CHECK(std::isnan(probe->CandidatePlaneOffset));
		}
		CHECK(ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixels() == before.GetPixels());
		CHECK(renderer.GetDefaultReflectionHistory().GetState().GetCompletedFrames() == historyFrames);
		CHECK(renderer.GetDefaultReflectionHistory().GetState().IsValid() == historyValid);
		CHECK(renderer.GetDefaultReflectionHistory().GetOwnedBytes() == historyBytes);
		graphics.WaitForIdle();
		graphics.CollectGarbage();
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectOwnedBytes == stats.EffectOwnedBytes);
		CHECK(renderer.GetStats().EffectAllocationAttempts == stats.EffectAllocationAttempts);
		const auto generation = renderer.InspectReflectionPixel({0, 0}, renderer.GetStats().RenderFrame)->Generation;
		ReflectionOutput resized(graphics.GetDevice(), 16);
		renderer.Render(scene, camera, resized.Framebuffer);
		CHECK_FALSE(renderer.InspectReflectionPixel({0, 0}, stats.RenderFrame));
		const auto probe = renderer.InspectReflectionPixel({0, 0}, renderer.GetStats().RenderFrame);
		REQUIRE(probe);
		CHECK(probe->Generation > generation);
		renderer.GetSettings().SSREnabled = false;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK_FALSE(renderer.InspectReflectionPixel({0, 0}, renderer.GetStats().RenderFrame));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
