#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace Paralax;

namespace {
	struct Output
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
		Output(nvrhi::IDevice* device, uint32_t width = 129, uint32_t height = 97)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Texture = device->createTexture(desc);
			REQUIRE(Texture);
			Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Texture));
			REQUIRE(Framebuffer);
		}
	};

	std::vector<glm::vec4> ReadHDR(nvrhi::IDevice* device, nvrhi::ITexture* texture)
	{
		auto staging = device->createStagingTexture(texture->getDesc(), nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		auto commands = device->createCommandList();
		commands->open();
		commands->copyTexture(staging, {}, texture, {});
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t pitch = 0;
		const auto* data = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(data);
		std::vector<glm::vec4> result;
		for (uint32_t y = 0; y < texture->getDesc().height; ++y)
			for (uint32_t x = 0; x < texture->getDesc().width; ++x)
			{
				uint64_t packed = 0;
				std::memcpy(&packed, data + y * pitch + x * 8, 8);
				result.push_back(glm::unpackHalf4x16(packed));
			}
		device->unmapStagingTexture(staging);
		return result;
	}

	void BuildScene(Scene& scene, AssetManager& assets)
	{
		std::array<uint8_t, 8 * 8 * 4> pixels{};
		for (size_t i = 0; i < 64; ++i)
		{
			pixels[i * 4] = uint8_t(80 + (i % 8) * 16);
			pixels[i * 4 + 1] = uint8_t(80 + (i / 8) * 16);
			pixels[i * 4 + 2] = 230;
			pixels[i * 4 + 3] = (i / 8 + i % 8) % 3 ? 255 : 0;
		}
		auto texture = TextureAsset::CreateRGBA8(8, 8, pixels.data(), false);
		texture->Handle = AssetHandle(65001);
		assets.AddMemoryAsset(texture);
		for (uint32_t i = 0; i < 5; ++i)
		{
			auto material = CreateRef<MaterialAsset>();
			material->Handle = AssetHandle(65010 + i);
			material->BaseColorFactor = {0.5f, 0.4f, 0.3f, i >= 3 ? 0.4f : 1.0f};
			material->MetallicFactor = 0.3f;
			material->RoughnessFactor = 0.35f;
			material->DoubleSided = true;
			if (i == 1 || i == 2)
			{
				material->BaseColorTexture = material->HeightTexture = material->NormalTexture = texture->Handle;
				material->HeightScale = 0.05f;
				material->NormalScale = 0.5f;
				material->Alpha = AlphaMode::Mask;
			}
			if (i >= 3)
				material->Alpha = AlphaMode::Blend;
			assets.AddMemoryAsset(material);
			auto entity = scene.CreateEntity("Cluster material");
			entity.GetTransform().Translation = {float(i % 3) - 1, 0, i == 3 ? 1.0f : i == 4 ? -1.0f
																							 : 0.0f};
			entity.GetTransform().Scale = {i == 2 ? -0.8f : 0.8f, 1, 0.3f};
			auto& mesh = entity.AddComponent<MeshComponent>();
			mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
			mesh.Materials = {material->Handle};
		}
		for (uint32_t i = 0; i < 64; ++i)
		{
			auto light = scene.CreateEntity("Cluster point");
			light.GetTransform().Translation = {float(i % 8) - 3.5f, float(i / 8) * 0.4f - 1, float(i % 3) - 0.5f};
			auto& point = light.AddComponent<PointLightComponent>();
			point.Range = 1.4f;
			point.Intensity = 0.15f;
			point.Color = {0.4f + float(i % 2) * 0.5f, 0.6f, 0.8f};
			point.CastShadows = i == 27;
		}
		auto spot = scene.CreateEntity("Cluster spot");
		spot.GetTransform().Translation = {0, 1, 2};
		auto& light = spot.AddComponent<SpotLightComponent>();
		light.Range = 5;
		light.Intensity = 0.5f;
		light.CastShadows = true;
		auto volume = scene.CreateEntity("Frozen offscreen GI");
		volume.GetTransform().Translation = {-2, -1, -2};
		auto& gi = volume.AddComponent<GIVolumeComponent>();
		gi.ProbeCountX = gi.ProbeCountY = gi.ProbeCountZ = 2;
		gi.ProbeSpacing = 4;
	}

	void Configure(SceneRenderer& renderer)
	{
		auto& settings = renderer.GetSettings();
		settings.ShadowResolution = ShadowQuality::Low;
		settings.LocalShadowResolution = LocalShadowQuality::Low;
		settings.GIFreeze = true;
		settings.DefaultEnvironmentIntensity = 0.1f;
	}

	void Parity(RenderPath path, bool orthographic)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			Scene scene("ClusterParity");
			BuildScene(scene, assets);
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			Configure(renderer);
			auto& settings = renderer.GetSettings();
			settings.Path = path;
			Output output(graphics.GetDevice());
			auto camera = CameraView::LookAt({0.1f, 1, 4}, {0, 0, 0}, glm::radians(60.0f), 129.0f / 97, 0.1f, 30);
			if (orthographic)
			{
				camera.Orthographic = true;
				camera.Projection = glm::orthoRH_ZO(-3.0f, 3.0f, -2.25f, 2.25f, camera.Far, camera.Near);
			}
			settings.GIFreeze = false;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetGIState().Updates > 0);
			settings.GIFreeze = true;
			for (auto debug : {RenderDebugView::None, RenderDebugView::DirectDiffuse, RenderDebugView::DirectSpecular, RenderDebugView::LocalShadow, RenderDebugView::Indirect})
			{
				CAPTURE(debug);
				settings.DebugView = debug;
				settings.ClusteredLightingEnabled = false;
				renderer.Render(scene, camera, output.Framebuffer);
				const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
				const auto hdr = ReadHDR(graphics.GetDevice(), renderer.GetHDRTarget());
				const auto ids = renderer.GetStats().ShadowedLocalLightIDs;
				const auto updates = renderer.GetGIState().Updates;
				CHECK(renderer.GetStats().ClusterOwnedBytes == 0);
				settings.ClusteredLightingEnabled = true;
				renderer.Render(scene, camera, output.Framebuffer);
				const auto& stats = renderer.GetStats();
				REQUIRE(stats.EffectivePath == path);
				REQUIRE(stats.ClusteredLightingActive);
				CHECK(stats.ClusterDispatches == 1);
				CHECK(stats.ClusterPrefixUploadBytes == stats.Lights * sizeof(GPU::Light));
				CHECK(stats.ClusterLightTestsUpperBound == uint64_t(stats.ClusterCells) * stats.Lights);
				CHECK(stats.ShadowedLocalLightIDs == ids);
				CHECK(renderer.GetGIState().Updates == updates);
				CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) <= 1);
				const auto actual = ReadHDR(graphics.GetDevice(), renderer.GetHDRTarget());
				REQUIRE(actual.size() == hdr.size());
				float maxDifference = 0;
				for (size_t i = 0; i < hdr.size(); ++i)
					for (int channel = 0; channel < 4; ++channel)
					{
						REQUIRE(std::isfinite(actual[i][channel]));
						maxDifference = std::max(maxDifference, std::abs(actual[i][channel] - hdr[i][channel]));
					}
				CHECK(maxDifference <= 1e-5f);
				const auto attempts = stats.ClusterAllocationAttempts;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().ClusterAllocationAttempts == attempts);
				CHECK(renderer.GetStats().StaticLightingGenerations == 0);
				settings.ClusteredLightingEnabled = false;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixels() == baseline.GetPixels());
				CHECK(renderer.GetStats().ClusterOwnedBytes == 0);
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("ClusteredLighting: Forward perspective preserves HDR material and shadow lighting")
{
	Parity(RenderPath::Forward, false);
}
TEST_CASE("ClusteredLighting: Forward orthographic preserves HDR material and shadow lighting")
{
	Parity(RenderPath::Forward, true);
}
TEST_CASE("ClusteredLighting: Deferred perspective preserves HDR material and shadow lighting")
{
	Parity(RenderPath::Deferred, false);
}
TEST_CASE("ClusteredLighting: Deferred orthographic preserves HDR material and shadow lighting")
{
	Parity(RenderPath::Deferred, true);
}
TEST_CASE("ClusteredLighting: Visibility perspective preserves HDR material and shadow lighting")
{
	Parity(RenderPath::Visibility, false);
}
TEST_CASE("ClusteredLighting: Visibility orthographic preserves HDR material and shadow lighting")
{
	Parity(RenderPath::Visibility, true);
}

TEST_CASE("ClusteredLighting: offscreen probe direct lighting remains populated and identical during real updates")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("OffscreenClusterGI");
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(65100);
		material->BaseColorFactor = glm::vec4(1);
		assets.AddMemoryAsset(material);
		auto cube = scene.CreateEntity("Offscreen white cube");
		cube.GetTransform().Translation = {10, 0, 0};
		auto& mesh = cube.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		mesh.Materials = {material->Handle};
		auto lamp = scene.CreateEntity("Offscreen lamp");
		lamp.GetTransform().Translation = {10, 2, 0};
		auto& light = lamp.AddComponent<PointLightComponent>();
		light.Range = 5;
		light.Intensity = 8;
		auto volume = scene.CreateEntity("Offscreen probes");
		volume.GetTransform().Translation = {9, -1, -1};
		auto& gi = volume.AddComponent<GIVolumeComponent>();
		gi.ProbeCountX = gi.ProbeCountY = gi.ProbeCountZ = 2;
		gi.ProbeSpacing = 2;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto& settings = renderer.GetSettings();
		settings.DefaultEnvironmentIntensity = 0;
		settings.GIFreeze = false;
		settings.GIRaysPerProbe = 256;
		settings.GIHysteresis = 0;
		settings.GIBounceFeedback = false;
		Output output(graphics.GetDevice(), 65, 49);
		const auto camera = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(40.0f), 65.0f / 49, 0.1f, 10);
		renderer.Render(scene, camera, output.Framebuffer);
		const auto baseline = renderer.InspectGI({10, 0.75f, 0}, {0, -1, 0}, {0, 1, 0});
		REQUIRE(baseline);
		REQUIRE(baseline->Irradiance.x > 1e-4f);
		settings.ClusteredLightingEnabled = true;
		REQUIRE(renderer.ResetGIHistory());
		renderer.Render(scene, camera, output.Framebuffer);
		REQUIRE(renderer.GetStats().ClusteredLightingActive);
		const auto clustered = renderer.InspectGI({10, 0.75f, 0}, {0, -1, 0}, {0, 1, 0});
		REQUIRE(clustered);
		CHECK(clustered->Irradiance == baseline->Irradiance);
		CHECK(clustered->Summary == baseline->Summary);
		light.Intensity = 0;
		REQUIRE(renderer.ResetGIHistory());
		renderer.Render(scene, camera, output.Framebuffer);
		const auto unlit = renderer.InspectGI({10, 0.75f, 0}, {0, -1, 0}, {0, 1, 0});
		REQUIRE(unlit);
		CHECK(unlit->Irradiance.x < baseline->Irradiance.x * 0.01f);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

namespace {
	class IntegrationFailureFactory : public PostEffectResourceFactory
	{
	public:
		bool FailBacking = false, FailClone = false, FailDepth = false;
		nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc) override
		{
			if (FailBacking && !desc.isVirtual && desc.debugName == "ClusterLightPrefixAndMasks")
				return nullptr;
			return PostEffectResourceFactory::CreateBuffer(device, desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			if (FailClone)
				for (const auto& item : desc.bindings)
					if (item.type == nvrhi::ResourceType::RawBuffer_SRV && item.slot == 0)
						return nullptr;
			return PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
		}
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			if (FailDepth && desc.debugName == "NativeTextDepth")
				return nullptr;
			return PostEffectResourceFactory::CreateTexture(device, desc);
		}
	};
	class RecoveringShadowFactory : public LocalShadowResourceFactory
	{
	public:
		uint32_t Calls = 0;
		nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
		{
			return ++Calls == 1 ? nullptr : LocalShadowResourceFactory::CreatePipeline(device, desc, info);
		}
	};
}

TEST_CASE("ClusteredLighting: renderer failures preserve baseline and static lighting recovery without camera retry churn")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ClusterFallback");
		BuildScene(scene, assets);
		Output output(graphics.GetDevice(), 65, 49);
		auto camera = CameraView::LookAt({0, 1, 4}, {0, 0, 0}, glm::radians(60.0f), 65.0f / 49);
		for (uint32_t failure = 0; failure < 3; ++failure)
		{
			CAPTURE(failure);
			IntegrationFailureFactory factory;
			factory.FailBacking = failure == 0;
			factory.FailClone = failure == 1;
			SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory, failure == 2 ? 0 : PostEffectBudget::DefaultCapacity);
			REQUIRE(renderer.Initialize());
			Configure(renderer);
			renderer.GetSettings().GIEnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.GetSettings().ClusteredLightingEnabled = true;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK_FALSE(renderer.GetStats().ClusteredLightingActive);
			CHECK(renderer.GetStats().ClusterDispatches == 0);
			CHECK(renderer.GetStats().StaticLightingGenerations == (failure == 2 ? 0 : 1));
			CHECK(ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixels() == baseline.GetPixels());
			const auto attempts = renderer.GetStats().ClusterAllocationAttempts;
			for (int frame = 0; frame < 3; ++frame)
			{
				auto moved = camera;
				moved.View[3].x += 0.01f * float(frame);
				renderer.Render(scene, moved, output.Framebuffer);
				CHECK(renderer.GetStats().ClusterAllocationAttempts == attempts);
				CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			}
			factory.FailBacking = factory.FailClone = false;
			renderer.GetSettings().ClusteredLightingEnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			renderer.GetSettings().ClusteredLightingEnabled = true;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().ClusteredLightingActive == (failure != 2));
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ClusteredLighting: native depth fallback recovers shadow capacity and publishes history once")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ClusterDepthRecovery");
		BuildScene(scene, assets);
		auto text = scene.CreateEntity("World label");
		auto& label = text.AddComponent<TextComponent>();
		label.Text = "Recovery";
		label.ScreenSpace = false;
		IntegrationFailureFactory factory;
		factory.FailDepth = true;
		RecoveringShadowFactory shadows;
		SceneRenderer renderer(graphics, assets, &shadows, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = false;
		settings.ClusteredLightingEnabled = true;
		settings.InternalRenderScale = 0.5f;
		Output output(graphics.GetDevice(), 65, 49);
		const auto camera = CameraView::LookAt({0, 1, 4}, {0, 0, 0}, glm::radians(60.0f), 65.0f / 49);
		LocalShadowHistory history;
		renderer.Render(scene, camera, output.Framebuffer, &history);
		const auto& stats = renderer.GetStats();
		CHECK(stats.EffectiveRenderScale == 1);
		CHECK(stats.LocalShadowCapacity > 0);
		CHECK(stats.ShadowedLocalLights > 0);
		CHECK(stats.ShadowedLocalLightIDs.size() == stats.ShadowedLocalLights);
		CHECK(history.Lights.size() == stats.ShadowedLocalLights);
		for (size_t i = 0; i < history.Lights.size(); ++i)
			CHECK(history.Lights[i].ID == stats.ShadowedLocalLightIDs[i]);
		const auto retained = history.Lights;
		renderer.Render(scene, camera, output.Framebuffer, &history);
		CHECK(history.Lights == retained);
		CHECK(renderer.GetStats().ClusteredLightingActive);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ClusteredLighting: scaled resized and private views retain independent temporal and reflection histories")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ClusterViewTransitions");
		BuildScene(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = settings.Shadows = false;
		settings.ClusteredLightingEnabled = settings.SSREnabled = settings.SSRTemporalEnabled = settings.SSAOEnabled = true;
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		Output output(graphics.GetDevice(), 65, 49), capture(graphics.GetDevice(), 91, 59);
		const auto camera = CameraView::LookAt({0, 1, 4}, {0, 0, 0}, glm::radians(60.0f), 65.0f / 49);
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
			for (float scale : {0.5f, 1.0f, 2.0f})
			{
				CAPTURE(path);
				CAPTURE(scale);
				settings.Path = path;
				settings.InternalRenderScale = scale;
				for (int frame = 0; frame < 3; ++frame)
				{
					renderer.Render(scene, camera, output.Framebuffer);
					const auto& stats = renderer.GetStats();
					REQUIRE(stats.ClusteredLightingActive);
					CHECK(stats.EffectivePath == path);
					CHECK(stats.EffectiveAA == AntiAliasingMode::Temporal);
					CHECK(stats.SSRActive);
					CHECK(stats.SSRHistoryActive);
					CHECK(stats.SSRHistoryEligible == (frame > 0));
					CHECK(stats.ClusterGrid.x == (stats.InternalWidth + 63) / 64);
					CHECK(stats.ClusterGrid.y == (stats.InternalHeight + 63) / 64);
				}
				const auto viewportFrames = renderer.GetStats().AACompletedFrames;
				const auto reflectedFrames = renderer.GetStats().SSRHistoryCompletedFrames;
				auto privateAA = renderer.CreateTemporalHistory();
				auto privateReflections = renderer.CreateReflectionHistory();
				renderer.Render(scene, camera, capture.Framebuffer, nullptr, privateAA.get(), privateReflections.get());
				REQUIRE(renderer.GetStats().ClusteredLightingActive);
				CHECK(renderer.GetStats().AACompletedFrames == 1);
				CHECK(renderer.GetStats().SSRHistoryCompletedFrames == 1);
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().AACompletedFrames == viewportFrames + 1);
				CHECK(renderer.GetStats().SSRHistoryCompletedFrames == reflectedFrames + 1);
				CHECK(renderer.GetStats().ClusteredLightingActive);
				const auto attempts = renderer.GetStats().ClusterAllocationAttempts;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().ClusterAllocationAttempts == attempts);
			}
		const auto backing = renderer.GetStats().ClusterBackingAttempts;
		settings.SSAOEnabled = false;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().ClusteredLightingActive);
		CHECK(renderer.GetStats().ClusterBackingAttempts == backing);
		settings.ClusteredLightingEnabled = false;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().ClusterOwnedBytes == 0);
		CHECK(renderer.GetStats().ClusterDispatches == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
