#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/SceneRendererTypes.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/packing.hpp>

#include <array>
#include <cmath>
#include <chrono>
#include <fstream>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Paralax;

namespace {

	struct RenderTarget
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
	};

	RenderTarget CreateOutput(nvrhi::IDevice* device, uint32_t width, uint32_t height)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::SRGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.debugName = "TestOutput";
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		RenderTarget target;
		target.Texture = device->createTexture(desc);
		target.Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target.Texture));
		return target;
	}

	// Ground plane, a red sphere, a metal cube, a sun with shadows and a point light.
	void BuildTestScene(Scene& scene, AssetManager& assets)
	{
		auto red = CreateRef<MaterialAsset>();
		red->Handle = AssetHandle(50001);
		red->BaseColorFactor = {0.8f, 0.1f, 0.1f, 1.0f};
		red->RoughnessFactor = 0.35f;
		assets.AddMemoryAsset(red);

		auto metal = CreateRef<MaterialAsset>();
		metal->Handle = AssetHandle(50002);
		metal->BaseColorFactor = {0.95f, 0.8f, 0.4f, 1.0f};
		metal->MetallicFactor = 1.0f;
		metal->RoughnessFactor = 0.25f;
		assets.AddMemoryAsset(metal);

		Entity ground = scene.CreateEntity("Ground");
		auto& groundMesh = ground.AddComponent<MeshComponent>();
		groundMesh.Mesh = AssetHandle(AssetManager::BuiltinPlane);
		groundMesh.Materials = {AssetHandle(AssetManager::BuiltinGroundMaterial)};

		Entity sphere = scene.CreateEntity("Sphere");
		sphere.GetTransform().Translation = {-0.8f, 0.5f, 0.0f};
		auto& sphereMesh = sphere.AddComponent<MeshComponent>();
		sphereMesh.Mesh = AssetHandle(AssetManager::BuiltinSphere);
		sphereMesh.Materials = {red->Handle};

		Entity cube = scene.CreateEntity("Cube");
		cube.GetTransform().Translation = {0.9f, 0.5f, 0.2f};
		cube.GetTransform().Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		auto& cubeMesh = cube.AddComponent<MeshComponent>();
		cubeMesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		cubeMesh.Materials = {metal->Handle};

		Entity sun = scene.CreateEntity("Sun");
		sun.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-50.0f, 35.0f, 0.0f)));
		sun.AddComponent<DirectionalLightComponent>().Intensity = 3.0f;

		Entity lamp = scene.CreateEntity("Lamp");
		lamp.GetTransform().Translation = {0.0f, 1.2f, 1.2f};
		auto& point = lamp.AddComponent<PointLightComponent>();
		point.Color = {0.3f, 0.6f, 1.0f};
		point.Intensity = 4.0f;
		point.Range = 5.0f;
	}

	float Luminance(glm::u8vec4 c)
	{
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	}

	void CheckLightingSnapshot(nvrhi::IDevice* device, nvrhi::IStagingTexture* staging, uint32_t channels, const char* name,
							   uint32_t mip, uint32_t face, const std::filesystem::path& directory, std::ostream& report)
	{
		REQUIRE(staging);
		const auto& desc = staging->getDesc();
		size_t pitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(mapped);
		std::ofstream raw(directory / (std::string(name) + "-mip-" + std::to_string(mip) + "-face-" + std::to_string(face) + ".bin"), std::ios::binary);
		for (uint32_t row = 0; row < desc.height; row++)
			raw.write(reinterpret_cast<const char*>(mapped + row * pitch), static_cast<std::streamsize>(desc.width * channels * sizeof(uint16_t)));
		raw.close();
		const bool rawSaved = raw.good();
		float minimumEnergy = std::numeric_limits<float>::max();
		float maximumEnergy = 0.0f;
		double sum = 0.0;
		size_t invalid = 0;
		for (uint32_t y = 0; y < desc.height; y++)
			for (uint32_t x = 0; x < desc.width; x++)
			{
				float energy = 0.0f;
				for (uint32_t channel = 0; channel < std::min(channels, 3u); channel++)
				{
					uint16_t bits;
					std::memcpy(&bits, mapped + y * pitch + (x * channels + channel) * sizeof(bits), sizeof(bits));
					const float value = glm::unpackHalf1x16(bits);
					invalid += !std::isfinite(value) || value < 0.0f ? 1 : 0;
					energy += value;
				}
				minimumEnergy = std::min(minimumEnergy, energy);
				maximumEnergy = std::max(maximumEnergy, energy);
				sum += static_cast<double>(energy);
			}
		device->unmapStagingTexture(staging);
		report << name << " mip=" << mip << " face=" << face << " min=" << minimumEnergy << " max=" << maximumEnergy
			   << " width=" << desc.width << " height=" << desc.height << " channels=" << channels << " rawSaved=" << rawSaved
			   << " mean=" << sum / (static_cast<double>(desc.width) * desc.height) << " invalid=" << invalid << '\n';
		report.flush();
	}

	void CheckLightingTexture(nvrhi::IDevice* device, nvrhi::ITexture* texture, uint32_t channels, const char* name, const std::filesystem::path& directory, std::ostream& report)
	{
		REQUIRE(texture);
		const auto& source = texture->getDesc();
		for (uint32_t mip = 0; mip < source.mipLevels; mip++)
			for (uint32_t face = 0; face < source.arraySize; face++)
			{
				CAPTURE(name);
				CAPTURE(mip);
				CAPTURE(face);
				nvrhi::TextureDesc desc;
				desc.width = std::max(1u, source.width >> mip);
				desc.height = std::max(1u, source.height >> mip);
				desc.format = source.format;
				desc.initialState = nvrhi::ResourceStates::CopyDest;
				desc.keepInitialState = true;
				auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
				REQUIRE(staging);
				auto commands = device->createCommandList();
				REQUIRE(commands);
				commands->open();
				commands->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice().setMipLevel(mip).setArraySlice(face));
				commands->close();
				device->executeCommandList(commands);
				device->waitForIdle();
				CheckLightingSnapshot(device, staging, channels, name, mip, face, directory, report);
			}
	}

	void CheckMetalLighting(GraphicsDevice& graphics, SceneRenderer& renderer, Scene& scene, const CameraView& camera,
							const RenderTarget& output, const Image& first, const std::string& name, bool checkAppearance = true)
	{
		auto* device = graphics.GetDevice();
		const auto metal = first.GetPixel(195, 95);
		INFO("metal RGB: ", int(metal.r), ", ", int(metal.g), ", ", int(metal.b));
		const bool lit = Luminance(metal) > 25.0f && metal.r > metal.b + 10 && metal.g > metal.b + 10;
		if (checkAppearance)
			CHECK(lit);

		// Tonemapping can hide NaNs as black: inspect the linear half-float render target as well.
		nvrhi::TextureDesc desc = renderer.GetHDRTarget()->getDesc();
		desc.isRenderTarget = false;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->copyTexture(staging, nvrhi::TextureSlice(), renderer.GetHDRTarget(), nvrhi::TextureSlice());
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t pitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(mapped);
		size_t invalid = 0;
		glm::vec3 hdrMetal(0.0f);
		for (uint32_t y = 0; y < desc.height; y++)
			for (uint32_t x = 0; x < desc.width; x++)
				for (uint32_t channel = 0; channel < 3; channel++)
				{
					uint16_t bits;
					std::memcpy(&bits, mapped + y * pitch + (x * 4 + channel) * sizeof(bits), sizeof(bits));
					const float value = glm::unpackHalf1x16(bits);
					invalid += !std::isfinite(value) || value < 0.0f ? 1 : 0;
					if (x == 195 && y == 95)
						hdrMetal[channel] = value;
				}
		device->unmapStagingTexture(staging);
		INFO("linear metal RGB: ", hdrMetal.r, ", ", hdrMetal.g, ", ", hdrMetal.b);
		CHECK(invalid == 0);
		const bool hdrLit = glm::dot(hdrMetal, glm::vec3(0.2126f, 0.7152f, 0.0722f)) > 0.01f;
		CHECK(hdrLit);

		if (!lit || !hdrLit || invalid != 0)
		{
			const auto originalSettings = renderer.GetSettings();
			const auto directory = Testing::GetTestOutputDirectory() / name;
			REQUIRE(first.SavePNG(directory / "Lit.png"));
			for (auto debug : {RenderDebugView::BaseColor, RenderDebugView::Normals})
			{
				renderer.GetSettings().DebugView = debug;
				renderer.Render(scene, camera, output.Framebuffer);
				REQUIRE(ReadbackTexture(device, output.Texture).SavePNG(directory / (debug == RenderDebugView::BaseColor ? "BaseColor.png" : "Normals.png")));
			}
			renderer.GetSettings().DebugView = RenderDebugView::None;
			renderer.GetSettings().DefaultEnvironmentIntensity = 0.0f;
			renderer.GetSettings().Shadows = false;
			SkyLightComponent* sky = nullptr;
			auto skies = scene.GetAllEntitiesWith<SkyLightComponent>();
			if (auto skyIt = skies.begin(); skyIt != skies.end())
				sky = &Entity(*skyIt, &scene).GetComponent<SkyLightComponent>();
			const float skyIntensity = sky ? sky->Intensity : 0.0f;
			if (sky)
				sky->Intensity = 0.0f;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(ReadbackTexture(device, output.Texture).SavePNG(directory / "DirectOnly.png"));
			if (sky)
				sky->Intensity = skyIntensity;
			renderer.GetSettings() = originalSettings;
		}
	}

	void CaptureLightingFailure(GraphicsDevice& graphics, SceneRenderer& renderer, Scene& scene, const CameraView& camera,
								const RenderTarget& output, const Image& image, const std::string& name, const char* testCase)
	{
		const auto directory = Testing::GetTestOutputDirectory() / name;
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		REQUIRE_FALSE(error);
		std::ofstream report(directory / "LightingTextures.txt");
		REQUIRE(report.is_open());
		report << "TestCase=" << testCase << '\n';
		report << "CapturedEpochMilliseconds=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << '\n';
		report << "adapter=" << graphics.GetInfo().AdapterName << " driver=" << graphics.GetInfo().DriverName << '\n';
		report << "Raw data: tightly packed native half-float components, dimensions and channels below\n";
		report << "BRDFTrackedStateBeforeInitializationClose=" << static_cast<uint32_t>(renderer.GetBRDFStateBeforeInitializationClose())
			   << " BRDFTrackedStateAtFirstFrame=" << static_cast<uint32_t>(renderer.GetBRDFStateAtFirstFrame())
			   << " ExpectedCopySource=" << static_cast<uint32_t>(nvrhi::ResourceStates::CopySource)
			   << " ExpectedShaderResource=" << static_cast<uint32_t>(nvrhi::ResourceStates::ShaderResource) << '\n';
		graphics.GetDevice()->waitForIdle();
		CheckLightingSnapshot(graphics.GetDevice(), renderer.GetInitialBRDFLut(), 2, "BRDFLutAtInitialization", 0, 0, directory, report);
		CheckLightingSnapshot(graphics.GetDevice(), renderer.GetFirstFrameBRDFLut(), 2, "BRDFLutAtFirstFrame", 0, 0, directory, report);
		const auto& maps = renderer.GetEnvironmentMaps();
		CheckLightingTexture(graphics.GetDevice(), renderer.GetBRDFLut(), 2, "BRDFLut", directory, report);
		CheckLightingTexture(graphics.GetDevice(), maps.Specular, 4, "Specular", directory, report);
		CheckLightingTexture(graphics.GetDevice(), maps.Diffuse, 4, "Diffuse", directory, report);
		CheckLightingTexture(graphics.GetDevice(), maps.Radiance, 4, "Radiance", directory, report);
		// The caller already checked appearance; HDR environments need not have the procedural sky's gold hue.
		CheckMetalLighting(graphics, renderer, scene, camera, output, image, name, false);

		// Preserve all original evidence before changing the failed texture. This is a diagnostic, never a recovery path.
		auto* device = graphics.GetDevice();
		auto* lut = renderer.GetBRDFLut();
		auto* pipeline = renderer.GetBRDFLutPipeline();
		REQUIRE(pipeline);
		REQUIRE(pipeline->getDesc().bindingLayouts.size() == 1);
		report << "Probe: same texture and compute pipeline; fresh binding set and command list\n";
		report << "LUT description: format=" << static_cast<uint32_t>(lut->getDesc().format) << " isUAV=" << lut->getDesc().isUAV << '\n';
		report << "Sentinel expected per texel: R=4 G=8\n";
		auto backupDesc = lut->getDesc();
		backupDesc.isUAV = false;
		backupDesc.debugName = "BRDFLutDiagnosticBackup";
		auto backup = device->createTexture(backupDesc);
		REQUIRE(backup);
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->copyTexture(backup, nvrhi::TextureSlice(), lut, nvrhi::TextureSlice());
		commands->clearTextureFloat(lut, nvrhi::AllSubresources, nvrhi::Color(4.0f, 8.0f, 0.0f, 0.0f));
		commands->close();
		device->executeCommandList(commands);
		CheckLightingTexture(device, lut, 2, "BRDFLutSentinel", directory, report);

		nvrhi::BindingSetDesc bindingDesc;
		bindingDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::IBLConstants)), nvrhi::BindingSetItem::Texture_UAV(1, lut)};
		auto bindings = device->createBindingSet(bindingDesc, pipeline->getDesc().bindingLayouts[0]);
		REQUIRE(bindings);
		commands->open();
		nvrhi::ComputeState state;
		state.pipeline = pipeline;
		state.bindings = {bindings};
		commands->setComputeState(state);
		GPU::IBLConstants constants{};
		commands->setPushConstants(&constants, sizeof(constants));
		commands->dispatch(16, 16, 1);
		commands->close();
		device->executeCommandList(commands);
		CheckLightingTexture(device, lut, 2, "BRDFLutRedispatched", directory, report);
		renderer.Render(scene, camera, output.Framebuffer);
		REQUIRE(ReadbackTexture(device, output.Texture).SavePNG(directory / "Redispatched.png"));
		// Later checks in the same test must still observe the original contents, even if redispatch repaired this LUT.
		commands->open();
		commands->copyTexture(lut, nvrhi::TextureSlice(), backup, nvrhi::TextureSlice());
		commands->close();
		device->executeCommandList(commands);
		CheckLightingTexture(device, lut, 2, "BRDFLutRestored", directory, report);
	}

}

TEST_CASE("SceneRenderer: renders a lit, shadowed PBR scene without validation errors")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t initialErrors = graphics.GetValidationErrorCount();

	AssetManager assets;
	Scene scene("RenderTest");
	BuildTestScene(scene, assets);

	constexpr uint32_t width = 320, height = 180;
	{
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		RenderTarget output = CreateOutput(graphics.GetDevice(), width, height);
		const CameraView camera = CameraView::LookAt({0.0f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), float(width) / float(height));

		renderer.Render(scene, camera, output.Framebuffer);
		// A second frame reuses cached materials, meshes and the environment.
		renderer.Render(scene, camera, output.Framebuffer);

		const SceneRenderStats& stats = renderer.GetStats();
		CHECK(stats.DrawCalls == 3);
		CHECK(stats.ShadowDrawCalls > 0);
		CHECK(stats.Lights == 1);

		Image image = ReadbackTexture(graphics.GetDevice(), output.Texture);
		REQUIRE_FALSE(image.IsEmpty());
		image.SavePNG(Paralax::Testing::GetTestOutputDirectory() / "SceneRenderer.png");

		// Sky at the top is bright and blue-ish.
		const glm::u8vec4 sky = image.GetPixel(width / 2, 4);
		CHECK(sky.b > sky.r);
		CHECK(Luminance(sky) > 60.0f);

		// The red sphere covers a few hundred clearly red pixels.
		uint32_t redPixels = 0;
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width; x++)
			{
				const glm::u8vec4 c = image.GetPixel(x, y);
				if (c.r > c.g + 60 && c.r > c.b + 60)
					redPixels++;
			}
		CHECK(redPixels > 400);
		CHECK(redPixels < 6000);

		// Everything rendered is opaque, nothing is NaN-black in the middle of the frame.
		for (uint32_t y = 20; y < height; y += 20)
			for (uint32_t x = 20; x < width; x += 20)
				REQUIRE(image.GetPixel(x, y).a == 255);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

namespace {
	enum class TargetPreparation
	{
		DuringRender,
		BeforeRender,
		DuringPreparation,
		BeforeLutGeneration
	};

	void CheckBRDFTargetOrdering(TargetPreparation preparation)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto initialErrors = graphics.GetValidationErrorCount();
		{
			auto* device = graphics.GetDevice();
			AssetManager assets;
			Scene scene("AllocationCheckpoints");
			BuildTestScene(scene, assets);
			SceneRenderer renderer(graphics, assets);
			if (preparation == TargetPreparation::BeforeLutGeneration)
				REQUIRE(renderer.InitializeWithTargetsForDiagnostics(320, 180));
			else
				REQUIRE(renderer.Initialize());

			const auto readSnapshot = [device](nvrhi::IStagingTexture* staging) {
				REQUIRE(staging);
				const auto& desc = staging->getDesc();
				REQUIRE(desc.format == nvrhi::Format::RG16_FLOAT);
				REQUIRE(desc.width == 128);
				REQUIRE(desc.height == 128);
				std::vector<uint16_t> pixels(128 * 128 * 2);
				constexpr size_t RowBytes = 128 * 2 * sizeof(uint16_t);
				size_t pitch = 0;
				const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
				REQUIRE(mapped);
				const bool validPitch = pitch >= RowBytes;
				if (validPitch)
					for (size_t row = 0; row < 128; row++)
						std::memcpy(pixels.data() + row * 128 * 2, mapped + row * pitch, RowBytes);
				device->unmapStagingTexture(staging);
				REQUIRE(validPitch);
				return pixels;
			};
			const auto readCurrent = [&] {
				auto staging = device->createStagingTexture(renderer.GetInitialBRDFLut()->getDesc(), nvrhi::CpuAccessMode::Read);
				REQUIRE(staging);
				auto commands = device->createCommandList();
				REQUIRE(commands);
				commands->open();
				commands->copyTexture(staging, nvrhi::TextureSlice(), renderer.GetBRDFLut(), nvrhi::TextureSlice());
				commands->close();
				device->executeCommandList(commands);
				device->waitForIdle();
				return readSnapshot(staging);
			};

			// Waiting at each observation narrows the loss interval, but can change the intermittent failure's timing.
			device->waitForIdle();
			const auto initial = readSnapshot(renderer.GetInitialBRDFLut());
			const auto afterInitialization = readCurrent();
			auto output = CreateOutput(device, 320, 180);
			REQUIRE(output.Texture);
			REQUIRE(output.Framebuffer);
			const auto afterOutputAllocation = readCurrent();
			std::vector<uint16_t> afterTargetPreparation;
			std::vector<std::pair<std::string, std::vector<uint16_t>>> preparationSnapshots;
			if (preparation != TargetPreparation::DuringRender)
			{
				if (preparation == TargetPreparation::DuringPreparation)
				{
					renderer.PrepareTargetsForDiagnostics(320, 180, [&](const char* checkpoint) {
						preparationSnapshots.emplace_back(checkpoint, readCurrent());
					});
				}
				else
					renderer.PrepareTargetsForDiagnostics(320, 180);
				afterTargetPreparation = readCurrent();
			}
			const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 320.0f / 180.0f);
			renderer.Render(scene, camera, output.Framebuffer);
			device->waitForIdle();
			const auto atFirstFrame = readSnapshot(renderer.GetFirstFrameBRDFLut());
			const auto afterRendering = readCurrent();

			struct Observation
			{
				const char* Name;
				const std::vector<uint16_t>& Pixels;
			};
			std::vector<Observation> observations = {{"initial dispatch snapshot", initial}, {"after initialization submission", afterInitialization}, {"after output allocation", afterOutputAllocation}};
			if (preparation == TargetPreparation::DuringPreparation)
			{
				REQUIRE(preparationSnapshots.size() == 4);
				CHECK(preparationSnapshots[0].first == "after target preparation idle wait");
				CHECK(preparationSnapshots[1].first == "after scene textures before framebuffer");
				CHECK(preparationSnapshots[2].first == "after scene textures and framebuffer");
				CHECK(preparationSnapshots[3].first == "after shadow texture and framebuffers");
				for (const auto& [name, pixels] : preparationSnapshots)
					observations.push_back({name.c_str(), pixels});
			}
			if (preparation != TargetPreparation::DuringRender)
				observations.push_back({"after target preparation before Render", afterTargetPreparation});
			observations.push_back({"first frame after internal target allocation", atFirstFrame});
			observations.push_back({"after rendering", afterRendering});
			// Emit diagnostics after collecting every observation, so logging does not delay later GPU work.
			for (const auto& observation : observations)
			{
				INFO("checkpoint: ", std::string(observation.Name));
				REQUIRE(observation.Pixels.size() == initial.size());
				size_t mismatches = 0, invalid = 0, firstMismatch = initial.size();
				double energy = 0.0;
				for (size_t component = 0; component < initial.size(); component++)
				{
					const float value = glm::unpackHalf1x16(observation.Pixels[component]);
					invalid += !std::isfinite(value) || value < 0.0f ? 1 : 0;
					energy += static_cast<double>(value);
					if (observation.Pixels[component] != initial[component])
					{
						firstMismatch = std::min(firstMismatch, component);
						mismatches++;
					}
				}
				CAPTURE(energy);
				CAPTURE(mismatches);
				if (firstMismatch != initial.size())
				{
					INFO("first differing component: ", firstMismatch, ", initial: ", glm::unpackHalf1x16(initial[firstMismatch]),
						 ", observed: ", glm::unpackHalf1x16(observation.Pixels[firstMismatch]));
					CHECK(mismatches == 0);
				}
				CHECK(invalid == 0);
				CHECK(energy > 1.0);
			}
		}
		CHECK(graphics.GetValidationErrorCount() == initialErrors);
	}
}

TEST_CASE("SceneRenderer: BRDF LUT survives output and internal target allocation")
{
	CheckBRDFTargetOrdering(TargetPreparation::DuringRender);
}

TEST_CASE("SceneRenderer: BRDF LUT survives target preparation before frame recording")
{
	CheckBRDFTargetOrdering(TargetPreparation::BeforeRender);
}

TEST_CASE("SceneRenderer: BRDF LUT survives each target preparation stage")
{
	CheckBRDFTargetOrdering(TargetPreparation::DuringPreparation);
}

TEST_CASE("SceneRenderer: BRDF LUT survives when targets precede LUT generation")
{
	CheckBRDFTargetOrdering(TargetPreparation::BeforeLutGeneration);
}

TEST_CASE("SceneRenderer: diagnostic target preparation requires valid state and dimensions")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		SceneRenderer renderer(graphics, assets);
		Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(renderer.PrepareTargetsForDiagnostics(320, 180), Testing::AssertionFailedError);
		CHECK_THROWS_AS(renderer.InitializeWithTargetsForDiagnostics(0, 180), Testing::AssertionFailedError);
		CHECK_THROWS_AS(renderer.InitializeWithTargetsForDiagnostics(320, 0), Testing::AssertionFailedError);
		REQUIRE(renderer.Initialize());
		CHECK_THROWS_AS(renderer.PrepareTargetsForDiagnostics(0, 180), Testing::AssertionFailedError);
		CHECK_THROWS_AS(renderer.PrepareTargetsForDiagnostics(320, 0), Testing::AssertionFailedError);
		CHECK_THROWS_AS(renderer.InitializeWithTargetsForDiagnostics(320, 180), Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: cascade blend settings round trip and reject invalid input")
{
	SceneRendererSettings settings;
	const auto fields = SceneRendererSettings::GetFields();
	const auto* field = FindField(fields, "ShadowCascadeBlend");
	REQUIRE(field);
	CHECK(settings.ShadowCascadeBlend == 0.1f);
	std::string error;
	for (float value : {0.0f, 0.1f, 0.3f})
	{
		REQUIRE(Reflection::SetField(*field, &settings, value, error));
		SceneRendererSettings restored;
		REQUIRE(Reflection::FromJson(fields, "Settings", &restored, Reflection::ToJson(fields, &settings), error));
		CHECK(restored.ShadowCascadeBlend == value);
	}
	for (const nlohmann::json& value : {nlohmann::json(-0.1f), nlohmann::json(0.31f), nlohmann::json("bad"), nlohmann::json(nullptr)})
	{
		CHECK_FALSE(Reflection::SetField(*field, &settings, value, error));
		CHECK(settings.ShadowCascadeBlend == 0.3f);
	}
}

TEST_CASE("SceneRenderer: fixed look matches the reviewed reference image")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("LookReference");
		BuildTestScene(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto output = CreateOutput(graphics.GetDevice(), 320, 180);
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 320.0f / 180.0f);
		renderer.Render(scene, camera, output.Framebuffer);
		const Image first = ReadbackTexture(graphics.GetDevice(), output.Texture);
		renderer.Render(scene, camera, output.Framebuffer);
		const Image actual = ReadbackTexture(graphics.GetDevice(), output.Texture);
		CHECK(Image::MaxChannelDifference(first, actual) == 0);
		REQUIRE(actual.SavePNG(Testing::GetTestOutputDirectory() / "R1-Reference.png"));
		// Every GPU must retain the scene's lighting, including the metal which has no diffuse fallback.
		const auto sky = actual.GetPixel(160, 5);
		CHECK(sky.b > sky.r);
		CHECK(Luminance(sky) > 70.0f);
		const auto sphere = actual.GetPixel(125, 85);
		CHECK(sphere.r > sphere.g + 40);
		CHECK(sphere.r > sphere.b + 40);
		const auto metal = actual.GetPixel(195, 95);
		CHECK(Luminance(metal) > 25.0f);
		CHECK(metal.r > metal.b + 10);
		CHECK(metal.g > metal.b + 10);
		CHECK(Luminance(actual.GetPixel(112, 102)) + 8.0f < Luminance(actual.GetPixel(95, 102)));
		size_t blackPixels = 0;
		for (uint32_t y = 0; y < actual.GetHeight(); y++)
			for (uint32_t x = 0; x < actual.GetWidth(); x++)
			{
				const auto pixel = actual.GetPixel(x, y);
				REQUIRE(pixel.a == 255);
				blackPixels += Luminance(pixel) < 1.0f ? 1 : 0;
			}
		CHECK(blackPixels < 10);
		if (Luminance(metal) <= 25.0f || metal.r <= metal.b + 10 || metal.g <= metal.b + 10)
			CaptureLightingFailure(graphics, renderer, scene, camera, output, actual, "ReferenceLightingFailure",
								   "SceneRenderer: fixed look matches the reviewed reference image");

		const auto& info = graphics.GetInfo();
		const bool referenceDevice = info.AdapterName.find("llvmpipe") != std::string::npos ||
									 info.DriverName.find("llvmpipe") != std::string::npos || info.DriverName.find("lavapipe") != std::string::npos;
		if (referenceDevice)
		{
			const Image expected = Image::LoadFromFile(std::filesystem::path(__FILE__).parent_path() / "Golden" / "R1-Reference.png");
			REQUIRE(expected.GetWidth() == actual.GetWidth());
			REQUIRE(expected.GetHeight() == actual.GetHeight());
			// Permit sparse raster-edge differences within the reference driver family, but no broad color shift.
			uint64_t totalDifference = 0;
			size_t outliers = 0;
			for (size_t i = 0; i < actual.GetPixels().size(); i++)
			{
				const int difference = std::abs(int(actual.GetPixels()[i]) - int(expected.GetPixels()[i]));
				totalDifference += static_cast<uint64_t>(difference);
				outliers += difference > 12 ? 1 : 0;
			}
			CHECK(double(totalDifference) / double(actual.GetPixels().size()) < 2.0);
			CHECK(double(outliers) / double(actual.GetPixels().size()) < 0.01);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: wide shadow filters do not shadow an unobstructed plane")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ReceiverPlane");
		auto ground = scene.CreateEntity("Ground");
		ground.GetTransform().Scale = glm::vec3(20.0f);
		auto& mesh = ground.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinPlane);
		mesh.Materials = {AssetHandle(AssetManager::BuiltinGroundMaterial)};
		auto sun = scene.CreateEntity("Sun");
		sun.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-35, 30, 0)));
		sun.AddComponent<DirectionalLightComponent>().Intensity = 3.0f;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.ShadowSoftness = 0.5f;
		settings.ShadowDistance = 30.0f;
		settings.ShadowResolution = ShadowQuality::Low;
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0, 0}, glm::radians(60.0f), 2.0f);
		for (uint32_t width : {160u, 240u})
		{
			auto output = CreateOutput(graphics.GetDevice(), width, width / 2);
			settings.Shadows = false;
			renderer.Render(scene, camera, output.Framebuffer);
			const Image unshadowed = ReadbackTexture(graphics.GetDevice(), output.Texture);
			settings.Shadows = true;
			for (float blend : {0.0f, 0.1f, 0.3f, -1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()})
			{
				CAPTURE(width);
				CAPTURE(blend);
				settings.ShadowCascadeBlend = blend;
				renderer.Render(scene, camera, output.Framebuffer);
				const Image shadowed = ReadbackTexture(graphics.GetDevice(), output.Texture);
				if (width == 240 && blend == 0)
				{
					unshadowed.SavePNG(Testing::GetTestOutputDirectory() / "R1-Unshadowed.png");
					shadowed.SavePNG(Testing::GetTestOutputDirectory() / "R1-Plane.png");
				}
				// Allow one percent quantization error at raster/filter edges.
				CHECK(Image::MaxChannelDifference(unshadowed, shadowed) <= 3);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: the last cascade fades smoothly to unshadowed lighting")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ShadowDistanceFade");
		auto ground = scene.CreateEntity("Ground");
		ground.GetTransform().Scale = glm::vec3(20.0f);
		ground.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinPlane);
		// The camera is below this roof: it sees only the fully shadowed receiver, spanning all cascades.
		auto roof = scene.CreateEntity("Roof");
		roof.GetTransform().Translation = {0, 10, 0};
		roof.GetTransform().Scale = {100, 1, 100};
		roof.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
		auto sun = scene.CreateEntity("Sun");
		sun.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-90, 0, 0)));
		sun.AddComponent<DirectionalLightComponent>().Intensity = 3.0f;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.ShadowDistance = 8.0f;
		settings.DefaultEnvironmentIntensity = 0.1f;
		const auto camera = CameraView::LookAt({0, 2, 4}, {0, 0, 0}, glm::radians(60.0f), 1.0f);
		auto output = CreateOutput(graphics.GetDevice(), 256, 256);
		auto maxStep = [](const Image& image) {
			float result = 0;
			for (uint32_t y = 40; y < 220; y++)
				result = std::max(result, std::abs(Luminance(image.GetPixel(128, y)) - Luminance(image.GetPixel(128, y + 1))));
			return result;
		};
		settings.ShadowCascadeBlend = 0.0f;
		renderer.Render(scene, camera, output.Framebuffer);
		const auto hard = ReadbackTexture(graphics.GetDevice(), output.Texture);
		settings.ShadowCascadeBlend = 0.3f;
		renderer.Render(scene, camera, output.Framebuffer);
		const auto smooth = ReadbackTexture(graphics.GetDevice(), output.Texture);
		REQUIRE(smooth.SavePNG(Testing::GetTestOutputDirectory() / "R1-ShadowFade.png"));
		CHECK(maxStep(hard) > 30.0f);
		CHECK(maxStep(smooth) < maxStep(hard) * 0.4f);
		CHECK(Luminance(smooth.GetPixel(128, 220)) < Luminance(smooth.GetPixel(128, 80)));
		for (float invalid : {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
		{
			settings.ShadowCascadeBlend = invalid;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(hard, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
		settings.ShadowCascadeBlend = 1.0f;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(Image::MaxChannelDifference(smooth, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: internal cascade transitions remain continuous as splits move")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("InternalCascadeBlend");
		BuildTestScene(scene, assets);
		for (auto handle : scene.GetAllEntitiesWith<DirectionalLightComponent>())
		{
			Entity sun(handle, &scene);
			sun.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-10, 35, 0)));
			sun.GetComponent<DirectionalLightComponent>().Intensity = 10.0f;
		}
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.ShadowResolution = ShadowQuality::Low;
		settings.ShadowSoftness = 0;
		settings.DefaultEnvironmentIntensity = 0;
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 1.5f);
		auto output = CreateOutput(graphics.GetDevice(), 144, 96);
		// These receiver pixels lie on the cube's long grazing-angle shadow edge. The first split sweeps
		// across them; other pixels mostly measure shadow-map texel snapping rather than the transition.
		constexpr std::array<glm::uvec2, 3> samples = {{{95, 63}, {101, 61}, {104, 60}}};
		std::array<std::array<float, 3>, 2> jumps{};
		for (size_t mode = 0; mode < 2; mode++)
		{
			settings.ShadowCascadeBlend = mode == 0 ? 0.0f : 0.3f;
			Image previous;
			for (int step = 0; step < 201; step++)
			{
				settings.ShadowDistance = 60.0f + static_cast<float>(step) * 0.1f;
				renderer.Render(scene, camera, output.Framebuffer);
				Image image = ReadbackTexture(graphics.GetDevice(), output.Texture);
				if (!previous.IsEmpty())
					for (size_t i = 0; i < samples.size(); i++)
					{
						const auto pixel = samples[i];
						const float difference = std::abs(Luminance(image.GetPixel(pixel.x, pixel.y)) - Luminance(previous.GetPixel(pixel.x, pixel.y)));
						jumps[mode][i] = std::max(jumps[mode][i], difference);
					}
				previous = std::move(image);
			}
		}
		for (size_t i = 0; i < samples.size(); i++)
		{
			CAPTURE(i);
			CHECK(jumps[0][i] > 3.0f);
			CHECK(jumps[1][i] < jumps[0][i] * 0.6f);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: built-in HDR rotation and intensity control image based surface lighting")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("HDRSurfaceLighting");
		BuildTestScene(scene, assets);
		scene.DestroyEntity(*scene.FindEntityByName("Sun"));
		scene.DestroyEntity(*scene.FindEntityByName("Lamp"));
		auto sky = scene.CreateEntity("Sky");
		auto& environment = sky.AddComponent<SkyLightComponent>();
		environment.ShowBackground = false; // Only surface lighting may account for image differences.
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto output = CreateOutput(graphics.GetDevice(), 320, 180);
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 320.0f / 180.0f);
		auto capture = [&] {
			renderer.Render(scene, camera, output.Framebuffer);
			return ReadbackTexture(graphics.GetDevice(), output.Texture);
		};
		auto mean = [](const Image& image) {
			double sum = 0;
			for (uint32_t y = 0; y < image.GetHeight(); y++)
				for (uint32_t x = 0; x < image.GetWidth(); x++)
					sum += static_cast<double>(Luminance(image.GetPixel(x, y)));
			return sum / static_cast<double>(image.GetWidth() * image.GetHeight());
		};
		for (uint64_t handle : {AssetManager::BuiltinSkyEnvironment, AssetManager::BuiltinStudioEnvironment})
		{
			CAPTURE(handle);
			REQUIRE(assets.GetAsset(AssetHandle(handle)));
			environment.Environment = AssetHandle(handle);
			environment.Intensity = 1.0f;
			environment.RotationDegrees = 0.0f;
			const auto original = capture();
			REQUIRE_FALSE(original.IsEmpty());
			CHECK(mean(original) > 15.0);
			CHECK(Luminance(original.GetPixel(195, 95)) > 20.0f);
			CHECK(Image::MaxChannelDifference(original, capture()) == 0);
			REQUIRE(original.SavePNG(Testing::GetTestOutputDirectory() / ("R2-Environment-" + std::to_string(handle) + ".png")));
			if (Luminance(original.GetPixel(195, 95)) <= 20.0f)
				CaptureLightingFailure(graphics, renderer, scene, camera, output, original, "HDRLightingFailure-" + std::to_string(handle),
									   "SceneRenderer: built-in HDR rotation and intensity control image based surface lighting");
			environment.RotationDegrees = 180.0f;
			const auto rotated = capture();
			CHECK(Image::MaxChannelDifference(original, rotated) > 10);
			REQUIRE(rotated.SavePNG(Testing::GetTestOutputDirectory() / ("R2-Rotated-" + std::to_string(handle) + ".png")));
			environment.RotationDegrees = 0.0f;
			environment.Intensity = 0.25f;
			const auto dimmed = capture();
			CHECK(mean(dimmed) + 5.0 < mean(original));
			environment.Intensity = 0.0f;
			CHECK(mean(capture()) < 1.0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: missing built-in environment uses the procedural sky and reports once")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		Testing::ScopedEnvironmentVariable resources("PARALAX_RESOURCE_DIR", Testing::CreateTestDirectory("MissingRendererResources").string());
		AssetManager assets;
		Scene scene("HDRFallback");
		BuildTestScene(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto output = CreateOutput(graphics.GetDevice(), 160, 90);
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 160.0f / 90.0f);
		renderer.Render(scene, camera, output.Framebuffer);
		const auto procedural = ReadbackTexture(graphics.GetDevice(), output.Texture);
		auto& sky = scene.CreateEntity("MissingSky").AddComponent<SkyLightComponent>();
		sky.Environment = AssetHandle(AssetManager::BuiltinSkyEnvironment);
		sky.Intensity = renderer.GetSettings().DefaultEnvironmentIntensity;
		Testing::ScopedLogCapture log;
		for (int frame = 0; frame < 3; frame++)
			renderer.Render(scene, camera, output.Framebuffer);
		CHECK(Image::MaxChannelDifference(procedural, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		size_t errors = 0;
		for (const auto& entry : log.GetEntries())
			errors += entry.Level == spdlog::level::err ? 1 : 0;
		CHECK(errors == 1);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: fresh and regenerated environments retain finite metallic lighting immediately")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	auto* device = graphics.GetDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	INFO("adapter: ", graphics.GetInfo().AdapterName, ", driver: ", graphics.GetInfo().DriverName);
	for (int iteration = 0; iteration < 4; iteration++)
	{
		CAPTURE(iteration);
		AssetManager assets;
		Scene scene("ImmediateEnvironment");
		BuildTestScene(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto output = CreateOutput(device, 320, 180);
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 320.0f / 180.0f);
		const auto sun = scene.FindEntityByName("Sun");
		REQUIRE(sun);
		for (int generation = 0; generation < 3; generation++)
		{
			CAPTURE(generation);
			// No readback or CPU wait between convolution and the forward pass consuming the new maps.
			if (generation > 0)
				sun->GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-50.0f, 35.0f + 20.0f * static_cast<float>(generation), 0.0f)));
			renderer.Render(scene, camera, output.Framebuffer);
			const Image first = ReadbackTexture(device, output.Texture);
			REQUIRE_FALSE(first.IsEmpty());
			CheckMetalLighting(graphics, renderer, scene, camera, output, first,
							   "LightingFailure-" + std::to_string(iteration) + "-" + std::to_string(generation));
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(first, ReadbackTexture(device, output.Texture)) == 0);
		}
		device->waitForIdle();
		device->runGarbageCollection();
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}
