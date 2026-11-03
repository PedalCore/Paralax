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

#include <bit>
#include <cstring>
#include <limits>

using namespace Paralax;

namespace {

	struct Target
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
	};

	Target Output(nvrhi::IDevice* device, uint32_t width, uint32_t height)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		Target result;
		result.Texture = device->createTexture(desc);
		result.Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(result.Texture));
		return result;
	}

	Entity Cube(Scene& scene, AssetManager& assets)
	{
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(55001);
		material->BaseColorFactor = {0, 0, 0, 1};
		material->EmissiveFactor = {0.8f, 0.25f, 0.1f};
		assets.AddMemoryAsset(material);
		auto cube = scene.CreateEntity("TemporalCube");
		auto& mesh = cube.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		mesh.Materials = {material->Handle};
		return cube;
	}

	void Configure(SceneRenderer& renderer)
	{
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = false;
		settings.Shadows = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.Tonemapper = TonemapOperator::None;
	}

	std::vector<float> ReadFloat(nvrhi::IDevice* device, nvrhi::ITexture* texture, uint32_t channels)
	{
		auto desc = texture->getDesc();
		desc.isRenderTarget = false;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		auto commands = device->createCommandList();
		commands->open();
		commands->copyTexture(staging, {}, texture, {});
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t pitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(mapped);
		std::vector<float> result(static_cast<size_t>(desc.width) * desc.height * channels);
		for (uint32_t y = 0; y < desc.height; ++y)
			std::memcpy(result.data() + y * desc.width * channels, mapped + y * pitch, desc.width * channels * sizeof(float));
		device->unmapStagingTexture(staging);
		return result;
	}

}

TEST_CASE("TemporalAA: all paths run temporal and spatial modes and restore the exact disabled baseline")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalPaths");
		Cube(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto output = Output(graphics.GetDevice(), 33, 25);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 33.0f / 25);
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			auto& settings = renderer.GetSettings();
			settings.Path = path;
			settings.AntiAliasing = AntiAliasingMode::Off;
			renderer.Render(scene, camera, output.Framebuffer);
			const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
			for (auto mode : {AntiAliasingMode::Temporal, AntiAliasingMode::Spatial})
			{
				settings.AntiAliasing = mode;
				for (uint32_t frame = 0; frame < 4; ++frame)
				{
					renderer.Render(scene, camera, output.Framebuffer);
					CHECK(renderer.GetStats().EffectiveAA == mode);
					CHECK(renderer.GetStats().EffectivePath == path);
					CHECK(renderer.GetStats().AAPasses > 0);
					CHECK(renderer.GetStats().EffectOwnedBytes > 0);
					CHECK(renderer.GetStats().AAHistoryValid == (mode == AntiAliasingMode::Temporal));
				}
			}
			settings.AntiAliasing = AntiAliasingMode::Off;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: rigid translation records previous unjittered UV and signed depth at raster samples")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalMotion");
		auto cube = Cube(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		auto output = Output(graphics.GetDevice(), 33, 33);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1, 0.1f, 10);
		camera.Orthographic = true;
		camera.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 10.0f, 0.1f);
		renderer.Render(scene, camera, output.Framebuffer);
		REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
		cube.GetTransform().Translation.x = 0.1f;
		auto& history = renderer.GetDefaultTemporalHistory();
		const auto jitter = history.GetState().GetJitter();
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK_FALSE(history.GetState().GetDrawRecords()[0].Reactive);
		const auto& targets = history.GetGuideFramebuffer()->getDesc().colorAttachments;
		const auto previousUV = ReadFloat(graphics.GetDevice(), targets[2].texture, 2);
		const auto previousDepth = ReadFloat(graphics.GetDevice(), targets[3].texture, 1);
		const size_t center = 16 * 33 + 16;
		// Vulkan viewport coordinates retain SubPixelPrecisionBits fractional bits. Interpolated attributes
		// may therefore differ from the unsnapped analytic projection by one raster subpixel, not float ULPs.
		// https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html#vertexpostproc-viewport
		const auto subpixelBits = graphics.GetInfo().Limits.SubPixelPrecisionBits;
		const float pixelTolerance = std::ldexp(1.0f, -static_cast<int>(std::min(subpixelBits, 24u))) + 33e-6f;
		INFO("raster subpixel bits=", subpixelBits, ", pixel tolerance=", pixelTolerance);
		CHECK(std::abs(previousUV[center * 2] - ((16.5f - jitter.x) / 33 - 0.05f)) * 33 < pixelTolerance);
		CHECK(std::abs(previousUV[center * 2 + 1] - (16.5f - jitter.y) / 33) * 33 < pixelTolerance);
		CHECK(std::abs(previousDepth[center] - 2.5f) < 1e-5f);
		cube.GetTransform().Rotation = glm::angleAxis(0.2f, glm::vec3(0, 1, 0));
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(history.GetState().GetDrawRecords()[0].Reactive);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: independent capture dimensions and settings do not advance or reset viewport history")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalOwners");
		Cube(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto& settings = renderer.GetSettings();
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		auto viewport = Output(graphics.GetDevice(), 33, 33);
		auto capture = Output(graphics.GetDevice(), 17, 17);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(scene, camera, viewport.Framebuffer);
		renderer.Render(scene, camera, viewport.Framebuffer);
		auto& state = renderer.GetDefaultTemporalHistory().GetState();
		REQUIRE(state.GetCompletedFrames() == 2);
		{
			auto owner = renderer.CreateTemporalHistory();
			settings.InternalRenderScale = 0.75f;
			for (int frame = 0; frame < 3; ++frame)
				renderer.Render(scene, camera, capture.Framebuffer, nullptr, owner.get());
			CHECK(owner->GetState().GetCompletedFrames() == 3);
			CHECK(state.GetCompletedFrames() == 2);
			CHECK(state.GetSampleIndex() == 2);
			CHECK(state.IsValid());
		}
		settings.InternalRenderScale = 1;
		renderer.Render(scene, camera, viewport.Framebuffer);
		CHECK(state.GetCompletedFrames() == 3);
		CHECK(state.GetSampleIndex() == 3);
		CHECK_FALSE(state.GetDrawRecords()[0].Reactive);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: combined bloom and scaled rendering retain backing and static lighting across ping pong frames")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalBloomScale");
		Cube(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto& settings = renderer.GetSettings();
		settings.AntiAliasing = AntiAliasingMode::Temporal;
		settings.InternalRenderScale = 0.75f;
		settings.BloomEnabled = true;
		settings.BloomIntensity = 0.2f;
		settings.BloomThreshold = 0;
		auto output = Output(graphics.GetDevice(), 33, 25);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 33.0f / 25);
		renderer.Render(scene, camera, output.Framebuffer);
		graphics.WaitForIdle();
		settings.ProfilingEnabled = true;
		INFO("timestamp bits=", graphics.GetInfo().GraphicsTimestampValidBits,
			 ", period ns=", graphics.GetInfo().TimestampPeriodNanoseconds);
		const auto owned = renderer.GetStats().EffectOwnedBytes;
		for (uint32_t frame = 0; frame < 8; ++frame)
		{
			const auto previousInvalid = renderer.GetStats().GPUProfilingInvalid;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
			CHECK(renderer.GetStats().BloomActive);
			CHECK(renderer.GetStats().InternalWidth == 25);
			CHECK(renderer.GetStats().EffectOwnedBytes == owned);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			if (frame > 0 && graphics.GetInfo().GraphicsTimestampValidBits >= 32 && graphics.GetInfo().TimestampPeriodNanoseconds > 0)
			{
				const auto& stats = renderer.GetStats();
				INFO("status=", stats.GPUProfilingStatus, ", last=", stats.GPUProfilingLastResult,
					 ", invalid=", stats.GPUProfilingInvalid, ", pending=", stats.GPUProfilingPending,
					 ", skipped=", stats.GPUProfilingSkipped, ", completed submissions=", frame);
				// One unprofiled warmup means the just-waited submission has renderer frame ID frame + 1.
				const uint64_t retiredFrame = frame + 1;
				const bool fresh = stats.GPUTiming && stats.GPUTiming->Context.Frame == retiredFrame;
				CHECK(stats.GPUProfilingInvalid == previousInvalid + (fresh ? 0u : 1u));
				CHECK(stats.GPUProfilingLastResult == (fresh ? "ready" : "invalid: timestamp interval"));
				if (stats.GPUTiming)
				{
					CHECK(stats.GPUTiming->Context.Frame <= retiredFrame);
					const auto& sample = *stats.GPUTiming;
					CHECK(sample.Context.EffectiveAA == static_cast<uint32_t>(AntiAliasingMode::Temporal));
					CHECK(sample.Context.InternalWidth == 25);
					CHECK(sample.Context.OutputWidth == 33);
					CHECK(sample.Context.BloomActive);
					REQUIRE(sample.Milliseconds[static_cast<size_t>(RenderTimingSection::AntiAliasing)]);
					CHECK(std::isfinite(*sample.Milliseconds[static_cast<size_t>(RenderTimingSection::AntiAliasing)]));
					CHECK(*sample.Milliseconds[static_cast<size_t>(RenderTimingSection::AntiAliasing)] >= 0);
				}
				else
				{
					// Wall-clock scheduling can exceed the bounded timestamp interval even after WaitForIdle.
					// Accept only the profiler's explicit rejection, never an unexplained absent measurement.
					CHECK(stats.GPUProfilingInvalid == frame);
					CHECK(stats.GPUProfilingLastResult == "invalid: timestamp interval");
					CHECK(stats.GPUProfilingStatus == "pending");
				}
				CHECK(stats.GPUProfilingPending == 1);
				CHECK(stats.GPUProfilingSkipped == 0);
			}
			graphics.WaitForIdle();
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: diagonal edge error improves against supersampled coverage with spatial and temporal reconstruction")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("AAEdgeQuality");
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(55010);
		material->BaseColorFactor = {0, 0, 0, 1};
		material->EmissiveFactor = {1, 1, 1};
		assets.AddMemoryAsset(material);
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = AssetHandle(55011);
		mesh->Materials = {material->Handle};
		mesh->Vertices = {{{-0.67f, -2, 0}, {0, 0, 1}}, {{2, -2, 0}, {0, 0, 1}}, {{2, 2, 0}, {0, 0, 1}}, {{0.73f, 2, 0}, {0, 0, 1}}};
		mesh->Indices = {0, 1, 2, 0, 2, 3};
		Submesh submesh;
		submesh.IndexCount = 6;
		submesh.VertexCount = 4;
		mesh->Submeshes.push_back(submesh);
		mesh->ComputeBounds();
		assets.AddMemoryAsset(mesh);
		scene.CreateEntity("Diagonal").AddComponent<MeshComponent>().Mesh = mesh->Handle;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1, 0.1f, 10);
		camera.Orthographic = true;
		camera.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 10.0f, 0.1f);
		constexpr uint32_t size = 64, samples = 32;
		std::vector<double> reference(size * size);
		for (uint32_t y = 0; y < size; ++y)
			for (uint32_t x = 0; x < size; ++x)
				for (uint32_t sy = 0; sy < samples; ++sy)
					for (uint32_t sx = 0; sx < samples; ++sx)
					{
						const double px = (double(x) + (double(sx) + 0.5) / samples) / size * 2 - 1;
						const double py = 1 - (double(y) + (double(sy) + 0.5) / samples) / size * 2;
						if (px >= 0.35 * py + 0.03)
							reference[y * size + x] += 1.0 / (samples * samples);
					}
		auto error = [&](const Image& image) {
			double sum = 0;
			for (uint32_t y = 0; y < size; ++y)
				for (uint32_t x = 0; x < size; ++x)
				{
					const double encoded = double(image.GetPixel(x, y).r) / 255;
					const double linear = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
					const double difference = linear - reference[y * size + x];
					sum += difference * difference;
				}
			return sum / (size * size);
		};
		auto output = Output(graphics.GetDevice(), size, size);
		renderer.Render(scene, camera, output.Framebuffer);
		const auto baseline = error(ReadbackTexture(graphics.GetDevice(), output.Texture));
		REQUIRE(baseline > 0.0001);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Spatial;
		renderer.Render(scene, camera, output.Framebuffer);
		const auto spatial = error(ReadbackTexture(graphics.GetDevice(), output.Texture));
		CHECK(spatial < baseline);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		double temporal = 0;
		for (uint32_t frame = 0; frame < 8; ++frame)
		{
			renderer.Render(scene, camera, output.Framebuffer);
			temporal += error(ReadbackTexture(graphics.GetDevice(), output.Texture)) / 8;
		}
		INFO("linear coverage MSE: baseline=" << baseline << " spatial=" << spatial << " temporal sequence mean=" << temporal);
		CHECK(temporal < baseline);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: perspective camera and object translation reproject previous surface positions and leave no disocclusion trail")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("PerspectiveTemporalMotion");
		auto cube = Cube(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		auto output = Output(graphics.GetDevice(), 65, 65);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1, 0.1f, 10);
		const auto previousCamera = camera;
		for (int frame = 0; frame < 3; ++frame)
			renderer.Render(scene, camera, output.Framebuffer);
		cube.GetTransform().Translation = {0.2f, 0, 0.1f};
		camera = CameraView::LookAt({0.1f, 0, 3}, {0.1f, 0, 0}, glm::radians(60.0f), 1, 0.1f, 10);
		auto& history = renderer.GetDefaultTemporalHistory();
		const auto jitter = history.GetState().GetJitter();
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK_FALSE(history.GetState().GetDrawRecords()[0].Reactive);
		const auto& targets = history.GetGuideFramebuffer()->getDesc().colorAttachments;
		const auto previousUV = ReadFloat(graphics.GetDevice(), targets[2].texture, 2);
		const auto previousDepth = ReadFloat(graphics.GetDevice(), targets[3].texture, 1);
		const float ndcX = ((32.5f - jitter.x) / 65) * 2 - 1;
		const float ndcY = 1 - ((32.5f - jitter.y) / 65) * 2;
		const glm::vec4 previousWorld(ndcX * 2.4f / camera.Projection[0][0] - 0.1f, ndcY * 2.4f / camera.Projection[1][1], 0.5f, 1);
		const auto clip = previousCamera.Projection * previousCamera.View * previousWorld;
		const size_t center = 32 * 65 + 32;
		CHECK(std::abs(previousUV[center * 2] - (clip.x / clip.w * 0.5f + 0.5f)) < 1e-5f);
		CHECK(std::abs(previousUV[center * 2 + 1] - (0.5f - clip.y / clip.w * 0.5f)) < 1e-5f);
		CHECK(std::abs(previousDepth[center] - 2.5f) < 1e-5f);
		cube.GetTransform().Translation.x = 1.5f;
		for (int frame = 0; frame < 3; ++frame)
		{
			renderer.Render(scene, camera, output.Framebuffer);
			const auto image = ReadbackTexture(graphics.GetDevice(), output.Texture);
			for (uint32_t y = 28; y <= 36; ++y)
				for (uint32_t x = 28; x <= 36; ++x)
					CHECK(image.GetPixel(x, y).r <= 1);
		}
		cube.GetTransform().Translation = {0, 0, 2.45f};
		renderer.Render(scene, camera, output.Framebuffer);
		cube.GetTransform().Translation = {0, 0, 0};
		renderer.Render(scene, camera, output.Framebuffer);
		const auto ids = ReadFloat(graphics.GetDevice(), history.GetGuideFramebuffer()->getDesc().colorAttachments[1].texture, 1);
		CHECK((std::bit_cast<uint32_t>(ids[center]) & 0x80000000u) != 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: same-address scene replacement and material mutation reject stale history")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		std::optional<Scene> scene;
		scene.emplace("FirstScene");
		Cube(*scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		auto output = Output(graphics.GetDevice(), 17, 17);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(*scene, camera, output.Framebuffer);
		renderer.Render(*scene, camera, output.Framebuffer);
		auto& state = renderer.GetDefaultTemporalHistory().GetState();
		REQUIRE_FALSE(state.GetDrawRecords()[0].Reactive);
		auto material = assets.GetAsset<MaterialAsset>(AssetHandle(55001));
		REQUIRE(material);
		material->EmissiveFactor = {0, 1, 0};
		++material->Version;
		renderer.Render(*scene, camera, output.Framebuffer);
		CHECK(state.GetDrawRecords()[0].Reactive);
		CHECK(ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixel(8, 8).r <= 1);
		const auto* oldAddress = &*scene;
		scene.reset();
		scene.emplace("ReplacementScene");
		REQUIRE(&*scene == oldAddress);
		auto& replacement = scene->CreateEntity("Replacement cube").AddComponent<MeshComponent>();
		replacement.Mesh = AssetHandle(AssetManager::BuiltinCube);
		replacement.Materials = {material->Handle};
		renderer.Render(*scene, camera, output.Framebuffer);
		CHECK(state.GetResetReason() == "scene replaced");
		CHECK(state.GetSampleIndex() == 1);
		CHECK(state.GetDrawRecords()[0].Reactive);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: masked POM mirrored materials preserve guides interior shading and fresh history across paths and minimal sizes")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalMaterialParity");
		auto cube = Cube(scene, assets);
		cube.GetTransform().Scale.x = -1;
		std::array<uint8_t, 8 * 8 * 4> pixels{};
		for (size_t i = 0; i < 64; ++i)
		{
			pixels[i * 4] = 128;
			pixels[i * 4 + 1] = 128;
			pixels[i * 4 + 2] = 255;
			pixels[i * 4 + 3] = (i % 8 / 2 + i / 8 / 2) % 2 ? 255 : 0;
		}
		auto texture = TextureAsset::CreateRGBA8(8, 8, pixels.data(), false);
		texture->Handle = AssetHandle(55020);
		assets.AddMemoryAsset(texture);
		auto material = assets.GetAsset<MaterialAsset>(AssetHandle(55001));
		material->Alpha = AlphaMode::Mask;
		material->DoubleSided = true;
		material->BaseColorTexture = material->HeightTexture = material->NormalTexture = texture->Handle;
		material->HeightScale = 0.1f;
		++material->Version;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		const auto camera = CameraView::LookAt({0.2f, 0.1f, 2}, {0, 0, 0}, glm::radians(60.0f), 1);
		std::array<Image, 3> forward;
		std::array<std::vector<uint32_t>, 3> forwardIDs, forwardNormals;
		std::array<std::vector<float>, 3> forwardDepth;
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
			for (uint32_t size : {1u, 3u, 33u})
			{
				CAPTURE(path);
				CAPTURE(size);
				auto output = Output(graphics.GetDevice(), size, size);
				auto& settings = renderer.GetSettings();
				settings.Path = path;
				settings.AntiAliasing = AntiAliasingMode::Off;
				renderer.Render(scene, camera, output.Framebuffer);
				REQUIRE(renderer.GetStats().EffectivePath == path);
				const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
				settings.AntiAliasing = AntiAliasingMode::Temporal;
				renderer.Render(scene, camera, output.Framebuffer);
				REQUIRE(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
				REQUIRE(renderer.GetStats().EffectivePath == path);
				const auto reconstructed = ReadbackTexture(graphics.GetDevice(), output.Texture);
				auto& history = renderer.GetDefaultTemporalHistory();
				REQUIRE(history.GetGuideFramebuffer());
				const auto& guide = history.GetGuideFramebuffer()->getDesc();
				// These two formats contain one 32-bit word per texel. Preserve the raw
				// identity/half-normal bits without interpreting them as floating values.
				auto words = [&](nvrhi::ITexture* guideTexture) {
					const auto raw = ReadFloat(graphics.GetDevice(), guideTexture, 1);
					std::vector<uint32_t> result(raw.size());
					for (size_t i = 0; i < raw.size(); ++i)
						result[i] = std::bit_cast<uint32_t>(raw[i]);
					return result;
				};
				auto ids = words(guide.colorAttachments[1].texture);
				for (auto& id : ids)
					id &= 0x7fffffffu;
				const auto normals = words(guide.colorAttachments[0].texture);
				const auto depth = ReadFloat(graphics.GetDevice(), guide.depthAttachment.texture, 1);
				uint32_t interior = 0;
				for (uint32_t y = 0; y < size; ++y)
					for (uint32_t x = 0; x < size; ++x)
					{
						const size_t index = static_cast<size_t>(y) * size + x;
						CHECK((ids[index] != 0) == (baseline.GetPixel(x, y).r > 1));
						if (x == 0 || y == 0 || x + 1 == size || y + 1 == size || ids[index] == 0)
							continue;
						bool homogeneous = true;
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx)
							{
								const auto nx = static_cast<uint32_t>(int(x) + dx), ny = static_cast<uint32_t>(int(y) + dy);
								const size_t neighbor = static_cast<size_t>(ny) * size + nx;
								homogeneous &= ids[neighbor] == ids[index] && normals[neighbor] == normals[index];
							}
						if (homogeneous)
						{
							++interior;
							CHECK(reconstructed.GetPixel(x, y) == baseline.GetPixel(x, y));
						}
					}
				if (size == 33)
					CHECK(interior >= 4);
				const size_t sizeIndex = size == 1 ? 0 : size == 3 ? 1
																   : 2;
				if (path == RenderPath::Forward)
				{
					forward[sizeIndex] = reconstructed;
					forwardIDs[sizeIndex] = ids;
					forwardNormals[sizeIndex] = normals;
					forwardDepth[sizeIndex] = depth;
				}
				else
				{
					CHECK(ids == forwardIDs[sizeIndex]);
					CHECK(normals == forwardNormals[sizeIndex]);
					for (size_t i = 0; i < depth.size(); ++i)
						CHECK(std::abs(depth[i] - forwardDepth[sizeIndex][i]) <= 8 * std::numeric_limits<float>::epsilon());
					// Deferred material storage can move the final sRGB quantization by one code value.
					CHECK(Image::MaxChannelDifference(forward[sizeIndex], reconstructed) <= 1);
				}
				for (int frame = 0; frame < 2; ++frame)
					renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().AAHistoryValid);
				history.GetState().Reset("fresh history determinism test");
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(Image::MaxChannelDifference(reconstructed, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
				settings.AntiAliasing = AntiAliasingMode::Off;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: a capture that exhausts the shared budget cannot invalidate or advance the viewport")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalSharedBudget");
		Cube(scene, assets);
		auto output = Output(graphics.GetDevice(), 17, 17);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		uint64_t bytes = 0;
		{
			SceneRenderer measure(graphics, assets);
			REQUIRE(measure.Initialize());
			Configure(measure);
			measure.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
			measure.Render(scene, camera, output.Framebuffer);
			bytes = measure.GetStats().EffectOwnedBytes;
		}
		REQUIRE(bytes > 0);
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, nullptr, bytes);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		renderer.Render(scene, camera, output.Framebuffer);
		renderer.Render(scene, camera, output.Framebuffer);
		auto& state = renderer.GetDefaultTemporalHistory().GetState();
		REQUIRE(state.IsValid());
		{
			auto capture = renderer.CreateTemporalHistory();
			renderer.Render(scene, camera, output.Framebuffer, nullptr, capture.get());
			CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Off);
			CHECK(capture->GetState().GetCompletedFrames() == 0);
			CHECK(capture->GetOwnedBytes() == 0);
			CHECK(state.IsValid());
			CHECK(state.GetCompletedFrames() == 2);
			CHECK(state.GetSampleIndex() == 2);
		}
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
		CHECK(state.GetCompletedFrames() == 3);
		CHECK_FALSE(state.GetDrawRecords()[0].Reactive);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Off;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectOwnedBytes == 0);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		auto capture = renderer.CreateTemporalHistory();
		renderer.Render(scene, camera, output.Framebuffer, nullptr, capture.get());
		CHECK(renderer.GetStats().EffectiveAA == AntiAliasingMode::Temporal);
		CHECK(capture->GetState().GetCompletedFrames() == 1);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: moving transparent overlays mark current reactivity and cannot leave color in uncovered pixels")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalTransparency");
		auto receiver = Cube(scene, assets);
		receiver.GetTransform().Scale = {3, 3, 1};
		auto material = assets.GetAsset<MaterialAsset>(AssetHandle(55001));
		material->EmissiveFactor = {0, 0.5f, 0};
		++material->Version;
		auto glass = CreateRef<MaterialAsset>();
		glass->Handle = AssetHandle(55030);
		glass->BaseColorFactor = {0, 0, 0, 0.5f};
		glass->EmissiveFactor = {1, 0, 0};
		glass->Alpha = AlphaMode::Blend;
		assets.AddMemoryAsset(glass);
		auto overlay = scene.CreateEntity("MovingGlass");
		auto& mesh = overlay.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		mesh.Materials = {glass->Handle};
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		auto output = Output(graphics.GetDevice(), 33, 33);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		auto reactiveCenter = [&]() {
			auto* texture = renderer.GetDefaultTemporalHistory().GetReactiveFramebuffer()->getDesc().colorAttachments[0].texture;
			auto desc = texture->getDesc();
			desc.isRenderTarget = false;
			auto staging = graphics.GetDevice()->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			auto commands = graphics.GetDevice()->createCommandList();
			commands->open();
			commands->copyTexture(staging, {}, texture, {});
			commands->close();
			graphics.GetDevice()->executeCommandList(commands);
			graphics.WaitForIdle();
			size_t pitch = 0;
			const auto* bytes = static_cast<const uint8_t*>(graphics.GetDevice()->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
			REQUIRE(bytes);
			const auto value = bytes[16 * pitch + 16];
			graphics.GetDevice()->unmapStagingTexture(staging);
			return value;
		};
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			renderer.GetSettings().Path = path;
			overlay.GetTransform().Translation = {0, 0, 1};
			for (int frame = 0; frame < 3; ++frame)
				renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(reactiveCenter() == 255);
			REQUIRE(ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixel(16, 16).r > 100);
			overlay.GetTransform().Translation.x = 2;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(reactiveCenter() == 0);
			const auto center = ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixel(16, 16);
			CHECK(center.r <= 1);
			CHECK(center.g > 100);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: toggling profiling preserves accumulated history and matches an identically progressed control")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalProfilingToggle");
		Cube(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		auto control = renderer.CreateTemporalHistory();
		auto measured = renderer.CreateTemporalHistory();
		auto output = Output(graphics.GetDevice(), 33, 25);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 33.0f / 25);
		for (uint32_t frame = 0; frame < 8; ++frame)
		{
			renderer.GetSettings().ProfilingEnabled = false;
			renderer.Render(scene, camera, output.Framebuffer, nullptr, control.get());
			const auto reference = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.GetSettings().ProfilingEnabled = frame >= 3 && frame < 6;
			renderer.Render(scene, camera, output.Framebuffer, nullptr, measured.get());
			CHECK(measured->GetState().GetSampleIndex() == control->GetState().GetSampleIndex());
			CHECK(measured->GetState().GetCompletedFrames() == frame + 1);
			if (frame > 0)
				CHECK_FALSE(measured->GetState().GetDrawRecords()[0].Reactive);
			CHECK(Image::MaxChannelDifference(reference, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalAA: pass counts include only executed opaque reactive and resolve stages")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("TemporalPasses");
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		Configure(renderer);
		renderer.GetSettings().AntiAliasing = AntiAliasingMode::Temporal;
		auto output = Output(graphics.GetDevice(), 17, 17);
		const auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().AAPasses == 1);
		Cube(scene, assets);
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().AAPasses == 2);
		auto material = assets.GetAsset<MaterialAsset>(AssetHandle(55001));
		material->Alpha = AlphaMode::Blend;
		material->BaseColorFactor.a = 0.5f;
		++material->Version;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().AAPasses == 2);
		auto& opaque = scene.CreateEntity("Opaque").AddComponent<MeshComponent>();
		opaque.Mesh = AssetHandle(AssetManager::BuiltinCube);
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().AAPasses == 3);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
