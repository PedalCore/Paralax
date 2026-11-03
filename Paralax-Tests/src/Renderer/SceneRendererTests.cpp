#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/ShaderLibrary.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <tuple>
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

	std::vector<glm::vec3> ReadLinearHDR(nvrhi::IDevice* device, nvrhi::ITexture* texture)
	{
		auto desc = texture->getDesc();
		desc.isRenderTarget = false;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
		auto commands = device->createCommandList();
		REQUIRE(staging);
		REQUIRE(commands);
		commands->open();
		commands->copyTexture(staging, {}, texture, {});
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t pitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(mapped);
		std::vector<glm::vec3> pixels(static_cast<size_t>(desc.width) * desc.height);
		for (uint32_t y = 0; y < desc.height; ++y)
			for (uint32_t x = 0; x < desc.width; ++x)
				for (uint32_t channel = 0; channel < 3; ++channel)
				{
					uint16_t bits;
					std::memcpy(&bits, mapped + y * pitch + (x * 4 + channel) * sizeof(bits), sizeof(bits));
					pixels[y * desc.width + x][channel] = glm::unpackHalf1x16(bits);
				}
		device->unmapStagingTexture(staging);
		return pixels;
	}

	float Luminance(glm::u8vec4 c)
	{
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	}

}

TEST_CASE("SceneRenderer: rendering before Initialize or without an output is an asserted precondition")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		if (!Paralax::Testing::RequireTestGraphicsDevice())
			return;
		GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
		const uint32_t initialErrors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			Scene scene("PreconditionTest");
			const CameraView camera = CameraView::LookAt({0.0f, 1.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, glm::radians(60.0f), 1.0f);
			RenderTarget target = CreateOutput(graphics.GetDevice(), 16, 16);
			Paralax::Testing::ScopedAssertionCapture capture;

			// The runtime drops a renderer whose Initialize failed; one that is kept must not render uninitialised
			// pipelines.
			SceneRenderer renderer(graphics, assets);
			CHECK_THROWS_AS(renderer.Render(scene, camera, target.Framebuffer), Paralax::Testing::AssertionFailedError);

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());
			CHECK_THROWS_AS(renderer.Render(scene, camera, nullptr), Paralax::Testing::AssertionFailedError);
			// Still usable after the refused calls.
			renderer.Render(scene, camera, target.Framebuffer);
		}
		CHECK(graphics.GetValidationErrorCount() == initialErrors);
	}
}

TEST_CASE("SceneRenderer: renders a lit, shadowed PBR scene without validation errors")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
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

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());
			RenderTarget output = CreateOutput(graphics.GetDevice(), width, height);
			const CameraView camera = CameraView::LookAt({0.0f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), float(width) / float(height));

			renderer.Render(scene, camera, output.Framebuffer);
			// A second frame reuses cached materials, meshes and the environment.
			renderer.Render(scene, camera, output.Framebuffer);

			const SceneRenderStats& stats = renderer.GetStats();
			CHECK(stats.DrawCalls == 3 + stats.SurfaceResolveDrawCalls + stats.VisibilityResolveDrawCalls);
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
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
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

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());
			auto output = CreateOutput(graphics.GetDevice(), 320, 180);
			const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 320.0f / 180.0f);
			renderer.Render(scene, camera, output.Framebuffer);
			const Image first = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.Render(scene, camera, output.Framebuffer);
			const Image actual = ReadbackTexture(graphics.GetDevice(), output.Texture);
			CHECK(Image::MaxChannelDifference(first, actual) == 0);
			REQUIRE(actual.SavePNG(Testing::GetTestOutputDirectory() / "R1-Reference.png"));
			if (path == RenderPath::Forward)
				REQUIRE(actual.SavePNG(Testing::GetTestOutputDirectory() / "R11-Forward-Reference.png"));
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

			const auto& info = graphics.GetInfo();
			const bool referenceDevice = info.AdapterName.find("llvmpipe") != std::string::npos ||
										 info.DriverName.find("llvmpipe") != std::string::npos || info.DriverName.find("lavapipe") != std::string::npos;
			if (referenceDevice)
			{
				const Image expected = Image::LoadFromFile(std::filesystem::path(PX_SOURCE_ROOT) / "Paralax-Tests" / "src" / "Renderer" / "Golden" / "R1-Reference.png");
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
}

TEST_CASE("SceneRenderer: wide shadow filters do not shadow an unobstructed plane")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
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

			renderer.GetSettings().Path = path;
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
}

TEST_CASE("SceneRenderer: the last cascade fades smoothly to unshadowed lighting")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
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

			renderer.GetSettings().Path = path;
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
}

TEST_CASE("SceneRenderer: internal cascade transitions remain continuous as splits move")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
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

			renderer.GetSettings().Path = path;
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
}

TEST_CASE("SceneRenderer: built-in HDR rotation and intensity control image based surface lighting")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
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

			renderer.GetSettings().Path = path;
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
}

TEST_CASE("SceneRenderer: missing built-in environment uses the procedural sky and reports once")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto initialErrors = graphics.GetValidationErrorCount();
		{
			Testing::ScopedEnvironmentVariable resources("PARALAX_RESOURCE_DIR", ToUtf8(Testing::CreateTestDirectory("MissingRendererResources")));
			AssetManager assets;
			Scene scene("HDRFallback");
			BuildTestScene(scene, assets);
			SceneRenderer renderer(graphics, assets);

			renderer.GetSettings().Path = path;
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
}

TEST_CASE("SceneRenderer: static lighting is regenerated after render targets are allocated, with identical contents")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		if (!Paralax::Testing::RequireTestGraphicsDevice())
			return;
		GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
		const uint32_t initialErrors = graphics.GetValidationErrorCount();
		{
			nvrhi::IDevice* device = graphics.GetDevice();
			AssetManager assets;
			Scene scene("StaticLighting");
			BuildTestScene(scene, assets);
			SceneRenderer renderer(graphics, assets);

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());

			const auto readLut = [&] {
				nvrhi::ITexture* lut = renderer.GetBRDFLut();
				REQUIRE(lut);
				nvrhi::TextureDesc desc = lut->getDesc();
				desc.isUAV = false;
				desc.debugName = "BRDFLutReadback";
				auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
				auto commands = device->createCommandList();
				REQUIRE(staging);
				REQUIRE(commands);
				commands->open();
				commands->copyTexture(staging, nvrhi::TextureSlice(), lut, nvrhi::TextureSlice());
				commands->close();
				device->executeCommandList(commands);
				device->waitForIdle();
				const size_t rowBytes = size_t(desc.width) * 2 * sizeof(uint16_t);
				size_t pitch = 0;
				const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
				REQUIRE(mapped);
				REQUIRE(pitch >= rowBytes);
				std::vector<uint16_t> pixels(size_t(desc.width) * desc.height * 2);
				for (uint32_t row = 0; row < desc.height; row++)
					std::memcpy(pixels.data() + size_t(row) * desc.width * 2, mapped + row * pitch, rowBytes);
				device->unmapStagingTexture(staging);
				return pixels;
			};
			const auto energy = [](const std::vector<uint16_t>& pixels) {
				double sum = 0.0;
				for (uint16_t value : pixels)
					sum += static_cast<double>(glm::unpackHalf1x16(value));
				return sum;
			};

			// Zeroes the textures behind the renderer's back, so a frame can only show them non-zero by regenerating them.
			const auto clearStaticLighting = [&] {
				const EnvironmentMaps& maps = renderer.GetEnvironmentMaps();
				REQUIRE(maps.Radiance);
				REQUIRE(maps.Specular);
				REQUIRE(maps.Diffuse);
				auto commands = device->createCommandList();
				REQUIRE(commands);
				commands->open();
				for (nvrhi::ITexture* texture : {renderer.GetBRDFLut(), maps.Radiance.Get(), maps.Specular.Get(), maps.Diffuse.Get()})
					commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
				commands->close();
				device->executeCommandList(commands);
				device->waitForIdle();
			};
			// The sky is drawn from the radiance map: black when the maps are zero, bright when they were regenerated.
			const auto skyLuminance = [&](nvrhi::ITexture* texture, uint32_t width) {
				Image image = ReadbackTexture(device, texture);
				REQUIRE_FALSE(image.IsEmpty());
				return Luminance(image.GetPixel(width / 2, 4));
			};

			const CameraView camera = CameraView::LookAt({0.0f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f}, glm::radians(50.0f), 16.0f / 9.0f);
			RenderTarget small = CreateOutput(device, 320, 180);
			REQUIRE(small.Framebuffer);
			// The first frame allocates the internal targets and regenerates the LUT and environment maps after them.
			renderer.Render(scene, camera, small.Framebuffer);
			CHECK(renderer.GetStats().StaticLightingGenerations == 1);
			const std::vector<uint16_t> first = readLut();
			CHECK(energy(first) > 1.0);
			CHECK(skyLuminance(small.Texture, 320) > 60.0f);

			// A frame that allocates nothing regenerates nothing: zeroed textures stay zero.
			clearStaticLighting();
			renderer.Render(scene, camera, small.Framebuffer);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			CHECK(energy(readLut()) == 0.0);
			CHECK(skyLuminance(small.Texture, 320) < 5.0f);

			// A new output size reallocates the targets, and the same frame rewrites both, the LUT bit-identically.
			RenderTarget large = CreateOutput(device, 640, 360);
			REQUIRE(large.Framebuffer);
			renderer.Render(scene, camera, large.Framebuffer);
			CHECK(renderer.GetStats().StaticLightingGenerations == 1);
			CHECK(readLut() == first);
			CHECK(skyLuminance(large.Texture, 640) > 60.0f);
			renderer.Render(scene, camera, large.Framebuffer);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			CHECK(readLut() == first);
		}
		CHECK(graphics.GetValidationErrorCount() == initialErrors);
	}
}

TEST_CASE("SceneRenderer: diagnostic settings round trip and reject invalid values")
{
	SceneRendererSettings settings;
	const auto fields = SceneRendererSettings::GetFields();
	std::string error;
	for (const char* name : {"GIFreeze", "GIVisibility", "GIBounceFeedback", "EnvironmentSpecular", "GIShowProbes"})
	{
		const auto* field = FindField(fields, name);
		REQUIRE(field);
		for (bool value : {false, true})
		{
			REQUIRE(Reflection::SetField(*field, &settings, value, error));
			SceneRendererSettings restored;
			REQUIRE(Reflection::FromJson(fields, "Settings", &restored, Reflection::ToJson(fields, &settings), error));
			CHECK(Reflection::ToJson(fields, &restored)[name] == value);
		}
		CHECK_FALSE(Reflection::SetField(*field, &settings, "true", error));
	}
	const auto* debug = FindField(fields, "DebugView");
	REQUIRE(debug);
	for (const char* name : {"Lit", "Normals", "BaseColor", "Indirect", "GICoverage", "DirectDiffuse", "DirectSpecular", "EnvironmentDiffuse", "EnvironmentSpecular", "Emissive", "IndirectDiffuse", "GIWeightSum", "GIValidNeighbours", "GIProbeWeight", "GIMeanDistance", "GISecondMoment", "GIVariance", "LocalShadow"})
	{
		REQUIRE(Reflection::SetField(*debug, &settings, name, error));
		SceneRendererSettings restored;
		REQUIRE(Reflection::FromJson(fields, "Settings", &restored, Reflection::ToJson(fields, &settings), error));
		CHECK(Reflection::ToJson(fields, &restored)["DebugView"] == name);
	}
	CHECK_FALSE(Reflection::SetField(*debug, &settings, "Unknown", error));
	for (const auto& [name, low, high] : {std::tuple{"GIDebugProbe", -2.0, 1728.0}, std::tuple{"GIDebugScale", 0.0, 1001.0}})
	{
		const auto* field = FindField(fields, name);
		REQUIRE(field);
		CHECK_FALSE(Reflection::SetField(*field, &settings, low, error));
		CHECK_FALSE(Reflection::SetField(*field, &settings, high, error));
		CHECK_FALSE(Reflection::SetField(*field, &settings, nullptr, error));
	}
}

TEST_CASE("SceneRenderer: material debug views preserve sky while lighting diagnostics leave background black")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		AssetManager assets;
		Scene scene;
		{
			SceneRenderer renderer(graphics, assets);

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());
			auto output = CreateOutput(graphics.GetDevice(), 32, 32);
			const auto camera = CameraView::LookAt({0, 1, 4}, {0, 1, 0}, glm::radians(50.0f), 1.0f);
			renderer.Render(scene, camera, output.Framebuffer);
			const auto sky = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
			REQUIRE(sky.size() == 32 * 32);
			CHECK(glm::length(sky[16 * 32 + 16]) > 0.01f);
			for (auto mode : {RenderDebugView::Normals, RenderDebugView::BaseColor})
			{
				renderer.GetSettings().DebugView = mode;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()) == sky);
			}
			for (auto mode : {RenderDebugView::DirectDiffuse, RenderDebugView::IndirectDiffuse})
			{
				renderer.GetSettings().DebugView = mode;
				renderer.Render(scene, camera, output.Framebuffer);
				for (const auto& pixel : ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()))
					CHECK(pixel == glm::vec3(0.0f));
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("SceneRenderer: lighting diagnostic terms sum to the linear HDR composite")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		AssetManager assets;
		Scene scene;
		BuildTestScene(scene, assets);
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().ShowBackground = false;
		Entity volume = scene.CreateEntity("GI");
		volume.GetTransform().Translation = glm::vec3(-2.0f);
		volume.AddComponent<GIVolumeComponent>().ProbeSpacing = 2.0f;
		{
			SceneRenderer renderer(graphics, assets);

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());
			auto output = CreateOutput(graphics.GetDevice(), 64, 64);
			const auto camera = CameraView::LookAt({0, 2, 4}, {0, 0.4f, 0}, glm::radians(50.0f), 1.0f);
			auto& settings = renderer.GetSettings();
			settings.GIFreeze = true;
			settings.ShadowResolution = ShadowQuality::Low;
			for (bool gi : {false, true})
			{
				settings.GIEnabled = gi;
				settings.DebugView = RenderDebugView::None;
				renderer.Render(scene, camera, output.Framebuffer);
				const auto lit = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
				std::vector<glm::vec3> sum(lit.size(), glm::vec3(0));
				for (auto mode : {RenderDebugView::DirectDiffuse, RenderDebugView::DirectSpecular, RenderDebugView::EnvironmentDiffuse,
								  RenderDebugView::EnvironmentSpecular, RenderDebugView::Emissive, RenderDebugView::IndirectDiffuse})
				{
					settings.DebugView = mode;
					renderer.Render(scene, camera, output.Framebuffer);
					const auto term = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
					for (size_t pixel = 0; pixel < sum.size(); ++pixel)
						sum[pixel] += term[pixel];
				}
				size_t mismatches = 0, illuminated = 0;
				for (size_t pixel = 0; pixel < sum.size(); ++pixel)
				{
					illuminated += glm::length(lit[pixel]) > 0.05f ? 1 : 0;
					for (int channel = 0; channel < 3; ++channel)
						mismatches += std::abs(lit[pixel][channel] - sum[pixel][channel]) > 0.003f * std::max(1.0f, lit[pixel][channel]) ? 1 : 0;
				}
				CHECK(illuminated > 200);
				CHECK(mismatches == 0);
			}
			settings.DebugView = RenderDebugView::IndirectDiffuse;
			renderer.Render(scene, camera, output.Framebuffer);
			const auto diagnostic = ReadbackTexture(graphics.GetDevice(), output.Texture);
			settings.Exposure = 32.0f;
			settings.Tonemapper = TonemapOperator::ACES;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(diagnostic, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			settings.GIShowProbes = true;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(diagnostic, ReadbackTexture(graphics.GetDevice(), output.Texture)) > 10);
			settings.GIShowProbes = false;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(diagnostic, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("SceneRenderer: frozen GI stepping matches an exact unfrozen update sequence")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		AssetManager assets;
		Scene scene;
		BuildTestScene(scene, assets);
		Entity volume = scene.CreateEntity("GI");
		volume.GetTransform().Translation = glm::vec3(-2.0f);
		volume.AddComponent<GIVolumeComponent>().ProbeSpacing = 2.0f;
		Image expected;
		for (bool frozen : {false, true})
		{
			SceneRenderer renderer(graphics, assets);

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());
			CHECK_FALSE(renderer.QueueGIUpdates(1));
			CHECK_FALSE(renderer.ResetGIHistory());
			renderer.GetSettings().GIFreeze = frozen;
			renderer.GetSettings().ShadowResolution = ShadowQuality::Low;
			auto output = CreateOutput(graphics.GetDevice(), 64, 64);
			const auto camera = CameraView::LookAt({0, 2, 4}, {0, 0.4f, 0}, glm::radians(50.0f), 1.0f);
			auto render = [&] {
				renderer.Render(scene, camera, output.Framebuffer);
				graphics.WaitForIdle();
				graphics.CollectGarbage();
			};
			render();
			CHECK(renderer.GetGIState().Updates == 1);
			CHECK_FALSE(renderer.QueueGIUpdates(0));
			CHECK_FALSE(renderer.QueueGIUpdates(1025));
			if (frozen)
				REQUIRE(renderer.QueueGIUpdates(4));
			else
				CHECK_FALSE(renderer.QueueGIUpdates(4));
			for (int frame = 0; frame < 4; ++frame)
				render();
			CHECK(renderer.GetGIState().Updates == 5);
			CHECK(renderer.GetGIState().Queued == 0);
			const Image actual = ReadbackTexture(graphics.GetDevice(), output.Texture);
			if (!frozen)
				expected = actual;
			else
			{
				CHECK(Image::MaxChannelDifference(expected, actual) == 0);
				render();
				CHECK(renderer.GetGIState().Updates == 5);
				CHECK(Image::MaxChannelDifference(actual, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
				REQUIRE(renderer.ResetGIHistory());
				CHECK_FALSE(renderer.GetGIState().HistoryValid);
				render();
				CHECK(renderer.GetGIState().Updates == 1);
				REQUIRE(renderer.QueueGIUpdates(1024));
				CHECK_FALSE(renderer.QueueGIUpdates(1));
				renderer.GetSettings().GIEnabled = false;
				render();
				CHECK(renderer.GetGIState().Queued == 0);
				CHECK_FALSE(renderer.QueueGIUpdates(1));
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("SceneRenderer: orthographic GI uses parallel view rays without a finite camera-distance clamp")
{
	for (RenderPath path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const uint32_t errors = graphics.GetValidationErrorCount();
		for (bool rotated : {false, true})
		{
			glm::quat rotation = rotated ? glm::angleAxis(0.5f, glm::normalize(glm::vec3(1, 2, 0))) : glm::quat(1, 0, 0, 0);
			AssetManager assets;
			Scene scene;
			Entity slab = scene.CreateEntity("Receiver");
			slab.GetTransform().Translation = rotation * glm::vec3(0, 0, -0.1f);
			slab.GetTransform().Rotation = rotation;
			slab.GetTransform().Scale = {4, 4, 0.2f};
			slab.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
			Entity volume = scene.CreateEntity("GI");
			volume.GetTransform().Translation = {-2, -2, -2};
			auto& grid = volume.AddComponent<GIVolumeComponent>();
			grid.ProbeSpacing = 2.0f;
			grid.ProbeCountX = grid.ProbeCountY = grid.ProbeCountZ = 3;
			SceneRenderer renderer(graphics, assets);

			renderer.GetSettings().Path = path;
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.GIFreeze = true;
			settings.DebugView = RenderDebugView::GIProbeWeight;
			settings.GIDebugProbe = 22;
			settings.GINormalBias = 0.0f;
			auto target = CreateOutput(graphics.GetDevice(), 32, 32);
			CameraView camera = CameraView::LookAt({0, 0, 0.1f}, {}, glm::radians(60.0f), 1.0f, 0.01f, 20.0f);
			camera.Position = rotation * camera.Position;
			camera.View = glm::lookAt(camera.Position, glm::vec3(0), rotation * glm::vec3(0, 1, 0));
			camera.Orthographic = true;
			camera.Projection = glm::orthoRH_ZO(-1.5f, 1.5f, -1.5f, 1.5f, camera.Far, camera.Near);
			renderer.Render(scene, camera, target.Framebuffer);
			const auto close = ReadbackTexture(graphics.GetDevice(), target.Texture);
			REQUIRE(renderer.GetStats().EffectivePath == path);
			const auto closeLinear = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK(close.GetPixels() == ReadbackTexture(graphics.GetDevice(), target.Texture).GetPixels());
			camera.Position = rotation * glm::vec3(0, 0, 10);
			camera.View = glm::lookAt(camera.Position, glm::vec3(0), rotation * glm::vec3(0, 1, 0));
			renderer.Render(scene, camera, target.Framebuffer);
			const auto far = ReadbackTexture(graphics.GetDevice(), target.Texture);
			REQUIRE(renderer.GetStats().EffectivePath == path);
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK(far.GetPixels() == ReadbackTexture(graphics.GetDevice(), target.Texture).GetPixels());
			REQUIRE_FALSE(close.IsEmpty());
			CHECK(close.GetPixel(16, 16).r > 10);
			const auto maxDifference = Image::MaxChannelDifference(close, far);
			CAPTURE(static_cast<int>(path));
			CAPTURE(rotated);
			CAPTURE(maxDifference);
			REQUIRE(close.SavePNG(Testing::GetTestOutputDirectory() / ("R11-Ortho-close-" + std::to_string(static_cast<int>(path)) + "-" + std::to_string(rotated) + ".png")));
			REQUIRE(far.SavePNG(Testing::GetTestOutputDirectory() / ("R11-Ortho-far-" + std::to_string(static_cast<int>(path)) + "-" + std::to_string(rotated) + ".png")));
			// A translated camera changes depth reconstruction rounding; fixed-camera repeats above remain exact.
			if (path == RenderPath::Forward || !rotated)
				CHECK(close.GetPixels() == far.GetPixels());
			else
			{
				CHECK(maxDifference <= 1);
				for (size_t alpha = 3; alpha < close.GetPixels().size(); alpha += 4)
					CHECK(close.GetPixels()[alpha] == far.GetPixels()[alpha]);
			}
			const auto query = renderer.InspectGI(rotation * glm::vec3(0.046875f, -0.046875f, 0), rotation * glm::vec3(0, 0, 1), rotation * glm::vec3(0, 0, 1));
			REQUIRE(query);
			float expected = 0.0f;
			for (const auto& neighbour : query->Neighbours)
				if (neighbour.Metadata.x == 22)
					expected = neighbour.IrradianceWeight.w;
			const auto linear = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
			CHECK(linear[16 * 32 + 16].r == doctest::Approx(static_cast<double>(expected)).epsilon(0.003));
			CHECK(closeLinear[16 * 32 + 16].r == doctest::Approx(static_cast<double>(expected)).epsilon(0.003));
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("SceneRenderer: render paths round trip and reject hostile settings")
{
	SceneRendererSettings settings;
	CHECK(settings.Path == RenderPath::Forward);
	const auto fields = SceneRendererSettings::GetFields();
	const auto* field = FindField(fields, "RenderPath");
	REQUIRE(field);
	std::string error;
	for (const char* value : {"Forward", "Deferred", "Visibility"})
	{
		REQUIRE(Reflection::SetField(*field, &settings, value, error));
		SceneRendererSettings restored;
		REQUIRE(Reflection::FromJson(fields, "Settings", &restored, Reflection::ToJson(fields, &settings), error));
		CHECK(restored.Path == settings.Path);
	}
	for (const nlohmann::json& value : {nlohmann::json("Invalid"), nlohmann::json(-1), nlohmann::json(3), nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json::object(), nlohmann::json(true)})
	{
		const auto before = settings.Path;
		CHECK_FALSE(Reflection::SetField(*field, &settings, value, error));
		CHECK(settings.Path == before);
	}
}

TEST_CASE("SceneRenderer: Deferred preserves forward lighting and diagnostics across resize and path switches")
{
	RenderPath candidate = RenderPath::Deferred;
	SUBCASE("Deferred") {}
	SUBCASE("Visibility")
	{
		candidate = RenderPath::Visibility;
	}

	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("DeferredParity");
		BuildTestScene(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().ShadowResolution = ShadowQuality::Low;
		renderer.GetSettings().LocalShadowResolution = LocalShadowQuality::Low;
		for (const glm::uvec2 size : {glm::uvec2(320, 180), glm::uvec2(127, 93)})
		{
			auto output = CreateOutput(graphics.GetDevice(), size.x, size.y);
			const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), float(size.x) / float(size.y));
			for (int debug = 0; debug <= 17; ++debug)
			{
				CAPTURE(debug);
				CAPTURE(size.x);
				CAPTURE(size.y);
				renderer.GetSettings().DebugView = static_cast<RenderDebugView>(debug);
				renderer.GetSettings().Path = RenderPath::Forward;
				renderer.Render(scene, camera, output.Framebuffer);
				const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
				renderer.GetSettings().Path = candidate;
				renderer.Render(scene, camera, output.Framebuffer);
				REQUIRE(renderer.GetStats().EffectivePath == candidate);
				CHECK(renderer.GetStats().PathFallbackReason.empty());
				CHECK(renderer.GetStats().SurfaceOwnedBytes >= uint64_t(size.x) * size.y * 24);
				CHECK(renderer.GetStats().StaticLightingGenerations == 1);
				CHECK(renderer.GetStats().SurfaceResolveDrawCalls == 1);
				const auto allocations = renderer.GetStats().SurfaceAllocationAttempts;
				const Image deferred = ReadbackTexture(graphics.GetDevice(), output.Texture);
				uint64_t difference = 0, outliers = 0;
				for (uint32_t y = 0; y < size.y; ++y)
					for (uint32_t x = 0; x < size.x; ++x)
					{
						const auto a = forward.GetPixel(x, y), b = deferred.GetPixel(x, y);
						int largest = 0;
						for (int channel = 0; channel < 3; ++channel)
						{
							const int delta = std::abs(int(a[channel]) - int(b[channel]));
							difference += uint64_t(delta);
							largest = std::max(largest, delta);
						}
						outliers += largest > 8 ? 1 : 0;
					}
				CHECK(double(difference) / (double(size.x) * size.y * 3) < 1.0);
				CHECK(double(outliers) / (double(size.x) * size.y) < 0.001);
				if (size.x == 320 && debug <= 2)
					REQUIRE(deferred.SavePNG(Testing::GetTestOutputDirectory() / (std::string(candidate == RenderPath::Visibility ? "R11-Visibility-" : "R11-Deferred-") + std::to_string(debug) + ".png")));
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().SurfaceAllocationAttempts == allocations);
				CHECK(renderer.GetStats().StaticLightingGenerations == 0);
				CHECK(Image::MaxChannelDifference(deferred, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
				renderer.GetSettings().Path = RenderPath::Forward;
				renderer.Render(scene, camera, output.Framebuffer);
				CHECK(renderer.GetStats().SurfaceOwnedBytes == 0);
				CHECK(Image::MaxChannelDifference(forward, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

namespace {

	class FailingDeferredFactory : public DeferredResourceFactory
	{
	public:
		int Calls = 0, FailAt = -1;
		bool Unsupported = false;
		nvrhi::Format RejectedFormat = nvrhi::Format::UNKNOWN;
		nvrhi::FormatSupport QueryFormatSupport(nvrhi::IDevice* device, nvrhi::Format format) override
		{
			return Unsupported || format == RejectedFormat ? nvrhi::FormatSupport::None : device->queryFormatSupport(format);
		}
		nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
		{
			return Calls++ == FailAt ? nullptr : device->createTexture(desc);
		}
		nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
		{
			return Calls++ == FailAt ? nullptr : device->createFramebuffer(desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			return Calls++ == FailAt ? nullptr : device->createBindingSet(desc, layout);
		}
		nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
		{
			return Calls++ == FailAt ? nullptr : device->createGraphicsPipeline(desc, info);
		}
	};

}

TEST_CASE("SceneRenderer: Deferred failures preserve Forward output and recover without stable allocation churn")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("DeferredFallback");
		BuildTestScene(scene, assets);
		auto output = CreateOutput(graphics.GetDevice(), 64, 64);
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 1.0f);
		for (int failure = -1; failure < 10; ++failure)
		{
			CAPTURE(failure);
			FailingDeferredFactory factory;
			factory.FailAt = failure;
			factory.Unsupported = failure == -1;
			SceneRenderer renderer(graphics, assets, nullptr, &factory);
			REQUIRE(renderer.Initialize());
			renderer.GetSettings().ShadowResolution = ShadowQuality::Low;
			renderer.GetSettings().MaxShadowedLocalLights = 0;
			renderer.Render(scene, camera, output.Framebuffer);
			const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.GetSettings().Path = RenderPath::Deferred;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().RequestedPath == RenderPath::Deferred);
			CHECK(renderer.GetStats().EffectivePath == RenderPath::Forward);
			CHECK_FALSE(renderer.GetStats().PathFallbackReason.empty());
			CHECK(renderer.GetStats().SurfaceOwnedBytes == 0);
			CHECK(Image::MaxChannelDifference(forward, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			factory.Unsupported = false;
			factory.FailAt = -1;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectivePath == RenderPath::Deferred);
			CHECK(renderer.GetStats().PathFallbackReason.empty());
			const int calls = factory.Calls;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(factory.Calls == calls);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			renderer.GetSettings().Path = static_cast<RenderPath>(-1);
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectivePath == RenderPath::Forward);
			CHECK(renderer.GetStats().PathFallbackReason == "invalid render path");
			CHECK(renderer.GetStats().SurfaceOwnedBytes == 0);
			CHECK(Image::MaxChannelDifference(forward, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("DeferredSurfaces: capacity arithmetic rejects empty oversized and overflowing extents")
{
	CHECK(DeferredSurfaces::FitsCapacity(3840, 2160));
	CHECK_FALSE(DeferredSurfaces::FitsCapacity(0, 1));
	CHECK_FALSE(DeferredSurfaces::FitsCapacity(1, 0));
	CHECK_FALSE(DeferredSurfaces::FitsCapacity(16384, 16384));
	CHECK_FALSE(DeferredSurfaces::FitsCapacity(UINT32_MAX, UINT32_MAX));
	CHECK(DeferredSurfaces::FitsCapacity(11184810, 1));
	CHECK_FALSE(DeferredSurfaces::FitsCapacity(11184811, 1));
}

TEST_CASE("DeferredSurfaces: resolve requires prepared targets")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	DeferredSurfaces surfaces(graphics);
	Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(surfaces.Resolve(nullptr, nullptr, nullptr), Testing::AssertionFailedError);
}

TEST_CASE("SceneRenderer: Deferred preserves masked POM normal mapped mirrored and transparent materials with frozen GI")
{
	RenderPath candidate = RenderPath::Deferred;
	SUBCASE("Deferred") {}
	SUBCASE("Visibility")
	{
		candidate = RenderPath::Visibility;
	}

	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("SurfaceStress");
		BuildTestScene(scene, assets);
		std::array<uint8_t, 16 * 16 * 4> pixels{};
		for (size_t i = 0; i < 16 * 16; ++i)
		{
			pixels[i * 4] = uint8_t((i % 16) * 17);
			pixels[i * 4 + 1] = uint8_t((i / 16) * 17);
			pixels[i * 4 + 2] = 200;
			pixels[i * 4 + 3] = (i % 16 / 4 + i / 16 / 4) % 2 ? 255 : 0;
		}
		auto texture = TextureAsset::CreateRGBA8(16, 16, pixels.data(), false);
		texture->Handle = AssetHandle(50200);
		assets.AddMemoryAsset(texture);
		auto material = assets.GetAsset<MaterialAsset>(AssetHandle(50001));
		REQUIRE(material);
		material->BaseColorTexture = texture->Handle;
		material->NormalTexture = texture->Handle;
		material->HeightTexture = texture->Handle;
		material->HeightScale = 0.05f;
		material->NormalScale = 0.5f;
		material->OcclusionTexture = texture->Handle;
		material->EmissiveTexture = texture->Handle;
		material->EmissiveFactor = {0.3f, 0.1f, 0.05f};
		material->Alpha = AlphaMode::Mask;
		material->DoubleSided = true;
		material->Version++;
		Entity back = scene.CreateEntity("Mirrored back face");
		back.GetTransform().Translation = {-0.5f, 0.8f, -0.8f};
		back.GetTransform().Scale = {-1.6f, 0.7f, 1.0f};
		back.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(115, 20, 0)));
		auto& mesh = back.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinPlane);
		mesh.Materials = {material->Handle};
		auto glass = CreateRef<MaterialAsset>();
		glass->Handle = AssetHandle(50201);
		glass->BaseColorFactor = {0.1f, 0.6f, 0.9f, 0.3f};
		glass->EmissiveFactor = {0.03f, 0.05f, 0.1f};
		glass->Alpha = AlphaMode::Blend;
		assets.AddMemoryAsset(glass);
		Entity transparent = scene.CreateEntity("Glass");
		transparent.GetTransform().Translation = {0.7f, 0.6f, 0.8f};
		transparent.GetTransform().Scale = {1.2f, 1.1f, 0.2f};
		auto& glassMesh = transparent.AddComponent<MeshComponent>();
		glassMesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		glassMesh.Materials = {glass->Handle};
		Entity lamp = scene.CreateEntity("Shadowed point");
		lamp.GetTransform().Translation = {1, 2, 1};
		lamp.AddComponent<PointLightComponent>().CastShadows = true;
		Entity spot = scene.CreateEntity("Shadowed spot");
		spot.GetTransform().Translation = {-1, 2, 2};
		spot.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-35, -15, 0)));
		spot.AddComponent<SpotLightComponent>().CastShadows = true;
		Entity volume = scene.CreateEntity("GI");
		volume.GetTransform().Translation = {-2, -1, -2};
		volume.AddComponent<GIVolumeComponent>().ProbeSpacing = 2;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.GIFreeze = true;
		settings.ShadowResolution = ShadowQuality::Low;
		settings.LocalShadowResolution = LocalShadowQuality::Low;
		auto output = CreateOutput(graphics.GetDevice(), 192, 144);
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 192.0f / 144.0f);
		for (bool environmentSpecular : {true, false})
			for (int debug = 0; debug <= 17; ++debug)
			{
				CAPTURE(debug);
				CAPTURE(environmentSpecular);
				settings.EnvironmentSpecular = environmentSpecular;
				settings.DebugView = static_cast<RenderDebugView>(debug);
				settings.Path = RenderPath::Forward;
				renderer.Render(scene, camera, output.Framebuffer);
				const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
				settings.Path = candidate;
				renderer.Render(scene, camera, output.Framebuffer);
				REQUIRE(renderer.GetStats().EffectivePath == candidate);
				const Image deferred = ReadbackTexture(graphics.GetDevice(), output.Texture);
				uint64_t difference = 0, outliers = 0;
				for (uint32_t y = 0; y < 144; ++y)
					for (uint32_t x = 0; x < 192; ++x)
					{
						const auto a = forward.GetPixel(x, y), b = deferred.GetPixel(x, y);
						int largest = 0;
						for (int c = 0; c < 3; ++c)
						{
							const int delta = std::abs(int(a[c]) - int(b[c]));
							difference += uint64_t(delta);
							largest = std::max(largest, delta);
						}
						outliers += largest > 12 ? 1 : 0;
					}
				CHECK(double(difference) / (192 * 144 * 3) < 2.0);
				CHECK(double(outliers) / (192 * 144) < 0.005);
				if (environmentSpecular && debug <= 2)
				{
					REQUIRE(forward.SavePNG(Testing::GetTestOutputDirectory() / ("R11-Stress-Forward-" + std::to_string(debug) + ".png")));
					REQUIRE(deferred.SavePNG(Testing::GetTestOutputDirectory() / (std::string(candidate == RenderPath::Visibility ? "R11-Stress-Visibility-" : "R11-Stress-Deferred-") + std::to_string(debug) + ".png")));
				}
			}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("SceneRenderer: Deferred retains exact far plane surfaces in lighting diagnostics")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("FarPlane");
		scene.CreateEntity("Plane").AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinPlane);
		auto sun = scene.CreateEntity("Sun");
		sun.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-90, 0, 0)));
		sun.AddComponent<DirectionalLightComponent>();
		CameraView camera;
		camera.Position = {0, 1, 0};
		camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 0, -1));
		camera.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
		camera.Orthographic = true;
		camera.Near = 0;
		camera.Far = 1;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().Shadows = false;
		renderer.GetSettings().DebugView = RenderDebugView::DirectDiffuse;
		auto output = CreateOutput(graphics.GetDevice(), 32, 32);
		renderer.Render(scene, camera, output.Framebuffer);
		const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
		CHECK(forward.GetPixel(16, 16).r > 80);
		for (auto path : {RenderPath::Deferred, RenderPath::Visibility})
		{
			renderer.GetSettings().Path = path;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectivePath == path);
			CHECK(Image::MaxChannelDifference(forward, ReadbackTexture(graphics.GetDevice(), output.Texture)) <= 1);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: compact paths preserve accepted HDR and negative material colors through explicit fallback")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("MaterialRange");
		BuildTestScene(scene, assets);
		auto material = assets.GetAsset<MaterialAsset>(AssetHandle(50001));
		REQUIRE(material);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().Shadows = false;
		auto output = CreateOutput(graphics.GetDevice(), 64, 64);
		const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 1);
		for (const glm::vec4 color : {glm::vec4(2, 0.5f, 0.25f, 1), glm::vec4(-0.2f, 0.5f, 0.25f, 1), glm::vec4(100000, 0.5f, 0.25f, 1)})
		{
			material->BaseColorFactor = color;
			material->Version++;
			for (auto debug : {RenderDebugView::None, RenderDebugView::BaseColor, RenderDebugView::DirectDiffuse})
			{
				renderer.GetSettings().DebugView = debug;
				renderer.GetSettings().Path = RenderPath::Forward;
				renderer.Render(scene, camera, output.Framebuffer);
				const auto expected = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
				for (auto path : {RenderPath::Deferred, RenderPath::Visibility})
				{
					renderer.GetSettings().Path = path;
					renderer.Render(scene, camera, output.Framebuffer);
					CHECK(renderer.GetStats().EffectivePath == RenderPath::Forward);
					CHECK(renderer.GetStats().PathFallbackReason == "material exceeds compact surface range");
					CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()) == expected);
				}
			}
		}
		material->BaseColorFactor = {0.8f, 0.1f, 0.1f, 1};
		material->Version++;
		renderer.GetSettings().Path = RenderPath::Deferred;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectivePath == RenderPath::Deferred);
		CHECK(renderer.GetStats().PathFallbackReason.empty());
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: Deferred preserves dark textured albedo under strong illumination")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("DarkAlbedo");
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50300);
		material->RoughnessFactor = 1;
		assets.AddMemoryAsset(material);
		const std::array<uint8_t, 4> pixel{1, 2, 3, 255};
		auto texture = TextureAsset::CreateRGBA8(1, 1, pixel.data(), false);
		texture->Handle = AssetHandle(50301);
		assets.AddMemoryAsset(texture);
		auto& mesh = scene.CreateEntity("Receiver").AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinPlane);
		mesh.Materials = {material->Handle};
		auto sun = scene.CreateEntity("Sun");
		sun.GetTransform().Rotation = Math::EulerToQuat(glm::radians(glm::vec3(-90, 0, 0)));
		sun.AddComponent<DirectionalLightComponent>().Intensity = 200;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().Shadows = false;
		renderer.GetSettings().Exposure = 4;
		renderer.GetSettings().DefaultEnvironmentIntensity = 0;
		auto output = CreateOutput(graphics.GetDevice(), 64, 64);
		const auto camera = CameraView::LookAt({0, 1, 1}, {}, glm::radians(45.0f), 1);
		for (bool textured : {false, true})
		{
			material->BaseColorFactor = textured ? glm::vec4(0.3f, 0.2f, 0.1f, 1) : glm::vec4(0.001f, 0.0015f, 0.002f, 1);
			material->BaseColorTexture = textured ? texture->Handle : AssetHandle(0);
			material->Version++;
			for (auto debug : {RenderDebugView::None, RenderDebugView::DirectDiffuse, RenderDebugView::BaseColor})
			{
				renderer.GetSettings().DebugView = debug;
				renderer.GetSettings().Path = RenderPath::Forward;
				renderer.Render(scene, camera, output.Framebuffer);
				const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
				for (auto path : {RenderPath::Deferred, RenderPath::Visibility})
				{
					renderer.GetSettings().Path = path;
					renderer.Render(scene, camera, output.Framebuffer);
					REQUIRE(renderer.GetStats().EffectivePath == path);
					CHECK(Image::MaxChannelDifference(forward, ReadbackTexture(graphics.GetDevice(), output.Texture)) <= 1);
				}
				if (debug == RenderDebugView::None)
					CHECK(forward.GetPixel(32, 32).r > 50);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("VisibilitySurfaces: portable triangle IDs and raw buffer addressing are bounded")
{
	CHECK(VisibilitySurfaces::FitsTriangleIDs(0, 0));
	CHECK(VisibilitySurfaces::FitsTriangleIDs(UINT32_MAX - 1, 1));
	CHECK_FALSE(VisibilitySurfaces::FitsTriangleIDs(UINT32_MAX, 1));
	CHECK_FALSE(VisibilitySurfaces::FitsTriangleIDs(UINT64_MAX, 1));
	CHECK_FALSE(VisibilitySurfaces::FitsTriangleIDs(0, 0x80000000u));
	CHECK(VisibilitySurfaces::FitsGeometry(144, 12, 144));
	CHECK_FALSE(VisibilitySurfaces::FitsGeometry(145, 12, 144));
	CHECK_FALSE(VisibilitySurfaces::FitsGeometry(144, 145, 144));
	CHECK_FALSE(VisibilitySurfaces::FitsGeometry(0, 12, 144));
	CHECK_FALSE(VisibilitySurfaces::FitsGeometry(144, 0, 144));
	CHECK_FALSE(VisibilitySurfaces::FitsGeometry(uint64_t(UINT32_MAX) + 1, 12, UINT64_MAX));
}

TEST_CASE("VisibilitySurfaces: command recording and mesh preparation require prepared targets")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	VisibilitySurfaces surfaces(graphics);
	Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(surfaces.Clear(nullptr), Testing::AssertionFailedError);
	CHECK_THROWS_AS(surfaces.PrepareMesh(GPUMesh{}), Testing::AssertionFailedError);
}

namespace {

	// Register failure sites independently so slower drivers have the full test timeout for each recovery.
	void CheckVisibilityFailureRecovery(int failure)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto initialErrors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			Scene scene("DeferredFallback");
			BuildTestScene(scene, assets);
			auto output = CreateOutput(graphics.GetDevice(), 64, 64);
			const auto camera = CameraView::LookAt({0, 2, 4.5f}, {0, 0.4f, 0}, glm::radians(50.0f), 1.0f);
			CAPTURE(failure);
			FailingDeferredFactory factory;
			factory.FailAt = failure;
			factory.RejectedFormat = failure == 9 ? nvrhi::Format::R32_UINT : nvrhi::Format::UNKNOWN;
			factory.FailAt = failure == 9 ? -1 : failure;
			SceneRenderer renderer(graphics, assets, nullptr, &factory);
			REQUIRE(renderer.Initialize());
			renderer.GetSettings().ShadowResolution = ShadowQuality::Low;
			renderer.GetSettings().MaxShadowedLocalLights = 0;
			renderer.Render(scene, camera, output.Framebuffer);
			const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
			renderer.GetSettings().Path = RenderPath::Visibility;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().RequestedPath == RenderPath::Visibility);
			CHECK(renderer.GetStats().EffectivePath == RenderPath::Forward);
			CHECK_FALSE(renderer.GetStats().PathFallbackReason.empty());
			CHECK(renderer.GetStats().VisibilityResolveDrawCalls == 0);
			CHECK(Image::MaxChannelDifference(forward, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			factory.RejectedFormat = nvrhi::Format::UNKNOWN;
			factory.FailAt = -1;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectivePath == RenderPath::Visibility);
			CHECK(renderer.GetStats().PathFallbackReason.empty());
			const int calls = factory.Calls;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(factory.Calls == calls);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			renderer.GetSettings().Path = static_cast<RenderPath>(-1);
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectivePath == RenderPath::Forward);
			CHECK(renderer.GetStats().PathFallbackReason == "invalid render path");
			CHECK(renderer.GetStats().SurfaceOwnedBytes == 0);
			CHECK(Image::MaxChannelDifference(forward, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
		CHECK(graphics.GetValidationErrorCount() == initialErrors);
	}

}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after an unsupported ID format")
{
	CheckVisibilityFailureRecovery(9);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after an ID texture allocation failure")
{
	CheckVisibilityFailureRecovery(10);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after an ID framebuffer allocation failure")
{
	CheckVisibilityFailureRecovery(11);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after a normal ID pipeline allocation failure")
{
	CheckVisibilityFailureRecovery(12);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after a mirrored ID pipeline allocation failure")
{
	CheckVisibilityFailureRecovery(13);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after a two-sided ID pipeline allocation failure")
{
	CheckVisibilityFailureRecovery(14);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after a material resolve pipeline allocation failure")
{
	CheckVisibilityFailureRecovery(15);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after the first mesh ID binding allocation failure")
{
	CheckVisibilityFailureRecovery(16);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after the first mesh resolve binding allocation failure")
{
	CheckVisibilityFailureRecovery(17);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after the second mesh ID binding allocation failure")
{
	CheckVisibilityFailureRecovery(18);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after the second mesh resolve binding allocation failure")
{
	CheckVisibilityFailureRecovery(19);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after the third mesh ID binding allocation failure")
{
	CheckVisibilityFailureRecovery(20);
}

TEST_CASE("SceneRenderer: Visibility preserves Forward output and recovers after the third mesh resolve binding allocation failure")
{
	CheckVisibilityFailureRecovery(21);
}

TEST_CASE("SceneRenderer: Visibility reconstructs clipped tiny and UV seam triangles without losing interior coverage")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("VisibilityEdges");
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50400);
		material->DoubleSided = true;
		std::array<uint8_t, 32 * 32 * 4> pixels{};
		for (size_t i = 0; i < 32 * 32; ++i)
		{
			const bool check = ((i % 32) / 4 + (i / 32) / 4) % 2 != 0;
			pixels[i * 4] = check ? 255 : 40;
			pixels[i * 4 + 1] = check ? 60 : 220;
			pixels[i * 4 + 2] = 120;
			pixels[i * 4 + 3] = check ? 255 : 0;
		}
		auto texture = TextureAsset::CreateRGBA8(32, 32, pixels.data(), true);
		texture->Handle = AssetHandle(50401);
		assets.AddMemoryAsset(texture);
		material->BaseColorTexture = texture->Handle;
		assets.AddMemoryAsset(material);
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = AssetHandle(50402);
		mesh->Materials = {material->Handle};
		// A deliberately unused prefix exercises both vertex and index offsets in the pulling path.
		mesh->Vertices.resize(3);
		mesh->Indices = {0, 0, 0};
		for (const auto& vertex : std::array<MeshVertex, 9>{
				 MeshVertex{{-0.8f, -0.8f, -1}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}},
				 MeshVertex{{0.8f, -0.8f, -1}, {0, 0, 1}, {1, 0, 0, 1}, {6, 0}},
				 MeshVertex{{0, 0.8f, 0}, {0, 0, 1}, {1, 0, 0, 1}, {3, 6}},
				 MeshVertex{{-0.8f, 0.1f, -2}, {0, 0, 1}, {1, 0, 0, 1}, {0, 5}},
				 MeshVertex{{0.8f, 0.1f, -2}, {0, 0, 1}, {1, 0, 0, 1}, {4, 5}},
				 MeshVertex{{0, 0.9f, -2}, {0, 0, 1}, {1, 0, 0, 1}, {2, 0}},
				 MeshVertex{{0.6f, 0.6f, -1}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}},
				 MeshVertex{{0.64f, 0.6f, -1}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}},
				 MeshVertex{{0.62f, 0.64f, -1}, {0, 0, 1}, {1, 0, 0, 1}, {0.5f, 1}}})
			mesh->Vertices.push_back(vertex);
		for (uint32_t i = 0; i < 9; ++i)
			mesh->Indices.push_back(i);
		Submesh submesh;
		submesh.IndexOffset = 3;
		submesh.IndexCount = 9;
		submesh.VertexOffset = 3;
		submesh.VertexCount = 9;
		mesh->Submeshes.push_back(submesh);
		mesh->ComputeBounds();
		assets.AddMemoryAsset(mesh);
		scene.CreateEntity("Triangles").AddComponent<MeshComponent>().Mesh = mesh->Handle;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().Shadows = false;
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().ShowBackground = false;
		const auto camera = CameraView::LookAt({}, {0, 0, -1}, glm::radians(90.0f), 1, 0.1f, 10);
		auto output = CreateOutput(graphics.GetDevice(), 128, 128);
		for (float vertexZ : {0.0f, 0.2f, -0.11f})
		{
			mesh->Vertices[5].Position.z = vertexZ;
			mesh->ComputeBounds();
			mesh->Version++;
			for (bool mask : {false, true})
			{
				material->Alpha = mask ? AlphaMode::Mask : AlphaMode::Opaque;
				material->Version++;
				for (auto debug : {RenderDebugView::BaseColor, RenderDebugView::Normals})
				{
					CAPTURE(vertexZ);
					CAPTURE(mask);
					CAPTURE(static_cast<int>(debug));
					renderer.GetSettings().DebugView = debug;
					renderer.GetSettings().Path = RenderPath::Forward;
					renderer.Render(scene, camera, output.Framebuffer);
					const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
					renderer.GetSettings().Path = RenderPath::Visibility;
					renderer.Render(scene, camera, output.Framebuffer);
					REQUIRE(renderer.GetStats().EffectivePath == RenderPath::Visibility);
					const Image visibility = ReadbackTexture(graphics.GetDevice(), output.Texture);
					uint64_t error = 0, outliers = 0, missingInterior = 0;
					const auto covered = [](glm::u8vec4 p) { return p.r != 0 || p.g != 0 || p.b != 0; };
					for (uint32_t y = 0; y < 128; ++y)
						for (uint32_t x = 0; x < 128; ++x)
						{
							const auto a = forward.GetPixel(x, y), b = visibility.GetPixel(x, y);
							int largest = 0;
							for (int c = 0; c < 3; ++c)
							{
								const int delta = std::abs(int(a[c]) - int(b[c]));
								error += uint64_t(delta);
								largest = std::max(largest, delta);
							}
							outliers += largest > 12 ? 1 : 0;
							if (covered(a) != covered(b))
							{
								bool boundary = false;
								for (int dy = -1; dy <= 1; ++dy)
									for (int dx = -1; dx <= 1; ++dx)
										if (int(x) + dx >= 0 && int(x) + dx < 128 && int(y) + dy >= 0 && int(y) + dy < 128)
											boundary |= covered(forward.GetPixel(uint32_t(int(x) + dx), uint32_t(int(y) + dy))) != covered(a);
								missingInterior += boundary ? 0 : 1;
							}
						}
					CHECK(double(error) / (128 * 128 * 3) < 2.0);
					CHECK(double(outliers) / (128 * 128) < 0.005);
					CHECK(missingInterior == 0);
					const auto prefix = "R11-Edges-" + std::to_string(vertexZ) + "-" + std::to_string(mask) + "-" + std::to_string(int(debug));
					REQUIRE(forward.SavePNG(Testing::GetTestOutputDirectory() / (prefix + "-Forward.png")));
					REQUIRE(visibility.SavePNG(Testing::GetTestOutputDirectory() / (prefix + "-Visibility.png")));
				}
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: malformed mutable mesh indices and offsets are rejected once per version and recover")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
	{
		AssetManager assets;
		Scene scene("MalformedMesh");
		auto mesh = MeshPrimitives::CreateCube();
		mesh->Handle = AssetHandle(50500);
		assets.AddMemoryAsset(mesh);
		scene.CreateEntity("Mesh").AddComponent<MeshComponent>().Mesh = mesh->Handle;
		const auto indices = mesh->Indices;
		const auto submeshes = mesh->Submeshes;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().Path = path;
		renderer.GetSettings().Shadows = false;
		auto output = CreateOutput(graphics.GetDevice(), 32, 32);
		const auto camera = CameraView::LookAt({0, 0, 3}, {}, glm::radians(60.0f), 1);
		for (int invalid = 0; invalid < 4; ++invalid)
		{
			CAPTURE(invalid);
			CAPTURE(static_cast<int>(path));
			mesh->Indices = indices;
			mesh->Submeshes = submeshes;
			if (invalid == 0)
				mesh->Indices[0] = UINT32_MAX;
			if (invalid == 1)
				mesh->Submeshes[0].VertexOffset = UINT32_MAX;
			if (invalid == 2)
				mesh->Submeshes[0].IndexOffset = UINT32_MAX;
			if (invalid == 3)
				mesh->Submeshes[0].VertexCount = 1;
			mesh->Version++;
			Testing::ScopedLogCapture logs;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().Triangles == 0);
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().Triangles == 0);
			CHECK(logs.Count(spdlog::level::err, "Rejected renderer mesh") == 1);
			mesh->Indices = indices;
			mesh->Submeshes = submeshes;
			mesh->Version++;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().Triangles == 12);
			CHECK(renderer.GetStats().EffectivePath == path);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: failed Visibility preparation prunes replaced geometry and recovers without retained growth")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class RejectCubeBindings : public DeferredResourceFactory
		{
		public:
			bool Fail = true;
			nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
			{
				if (Fail && layout->getDesc()->registerSpace == 3 && !desc.bindings.empty() && desc.bindings[0].type == nvrhi::ResourceType::RawBuffer_SRV &&
					static_cast<nvrhi::IBuffer*>(desc.bindings[0].resourceHandle)->getDesc().byteSize == 24 * sizeof(MeshVertex))
					return nullptr;
				return device->createBindingSet(desc, layout);
			}
		} factory;
		AssetManager assets;
		Scene scene("Retirement");
		auto mesh = MeshPrimitives::CreatePlane();
		mesh->Handle = AssetHandle(50501);
		assets.AddMemoryAsset(mesh);
		auto early = scene.CreateEntityWithUUID(UUID(1), "Changing first mesh");
		early.AddComponent<MeshComponent>().Mesh = mesh->Handle;
		auto late = scene.CreateEntityWithUUID(UUID(2), "Failing later mesh");
		late.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
		SceneRenderer renderer(graphics, assets, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().Path = RenderPath::Visibility;
		renderer.GetSettings().Shadows = false;
		renderer.GetSettings().DebugView = RenderDebugView::DirectDiffuse;
		auto output = CreateOutput(graphics.GetDevice(), 32, 32);
		const auto camera = CameraView::LookAt({0, 2, 3}, {}, glm::radians(60.0f), 1);
		uint64_t retained = 0;
		for (int frame = 0; frame < 8; ++frame)
		{
			mesh->Vertices[0].Position.x -= 0.01f;
			mesh->ComputeBounds();
			mesh->Version++;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectivePath == RenderPath::Forward);
			CHECK(renderer.GetStats().PathFallbackReason == "geometry binding creation failed");
			if (frame == 0)
				retained = renderer.GetStats().VisibilityGeometryBytes;
			CHECK(retained > 0);
			CHECK(renderer.GetStats().VisibilityGeometryBytes == retained);
		}
		factory.Fail = false;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectivePath == RenderPath::Visibility);
		CHECK(renderer.GetStats().PathFallbackReason.empty());
		CHECK(renderer.GetStats().VisibilityGeometryBytes == retained);
		const auto allocations = renderer.GetStats().VisibilityAllocationAttempts;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().VisibilityAllocationAttempts == allocations);
		scene.DestroyEntity(early);
		scene.DestroyEntity(late);
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().VisibilityGeometryBytes == 0);
		CHECK(ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixel(16, 16) == glm::u8vec4(0, 0, 0, 255));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: post POM fine footprints preserve masked occlusion across primitive boundaries")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ParallaxFootprints");
		std::array<uint8_t, 64 * 64 * 4> pixels{};
		for (size_t i = 0; i < 64 * 64; ++i)
		{
			const bool check = (i % 64 / 2 + i / 64 / 2) % 2 != 0;
			pixels[i * 4] = check ? 255 : 24;
			pixels[i * 4 + 1] = check ? 40 : 80;
			pixels[i * 4 + 2] = check ? 180 : 240;
			pixels[i * 4 + 3] = check ? 255 : 0;
		}
		auto texture = TextureAsset::CreateRGBA8(64, 64, pixels.data(), false);
		texture->Handle = AssetHandle(50600);
		assets.AddMemoryAsset(texture);
		const std::array<uint8_t, 4> white{255, 255, 255, 255};
		auto constant = TextureAsset::CreateRGBA8(1, 1, white.data(), false);
		constant->Handle = AssetHandle(50601);
		assets.AddMemoryAsset(constant);
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50602);
		material->BaseColorFactor = {0.8f, 0.1f, 0.05f, 1};
		material->DoubleSided = true;
		material->HeightScale = 0.15f;
		material->NormalScale = 0.4f;
		assets.AddMemoryAsset(material);
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = AssetHandle(50603);
		mesh->Materials = {material->Handle};
		// Separate seam vertices and varying tangent frames force helpers to evaluate their own POM rays.
		mesh->Vertices = {
			{{-1.8f, -0.8f, -1}, {0, 0.8f, 0.6f}, {1, 0, 0, 1}, {0, 0}},
			{{1.8f, -0.8f, -1}, {0.2f, 0.8f, 0.6f}, {1, 0, 0, 1}, {8, 0}},
			{{1.8f, 1.2f, -6}, {-0.2f, 0.9f, 0.5f}, {1, 0, 0, 1}, {8, 8}},
			{{-1.8f, -0.8f, -1}, {0, 0.8f, 0.6f}, {1, 0, 0, 1}, {10, 12}},
			{{1.8f, 1.2f, -6}, {-0.2f, 0.9f, 0.5f}, {1, 0, 0, 1}, {18, 4}},
			{{-1.8f, 1.2f, -6}, {0.2f, 0.9f, 0.5f}, {1, 0, 0, 1}, {10, 4}}};
		mesh->Indices = {0, 1, 2, 3, 4, 5};
		Submesh submesh;
		submesh.IndexCount = submesh.VertexCount = 6;
		mesh->Submeshes.push_back(submesh);
		mesh->ComputeBounds();
		assets.AddMemoryAsset(mesh);
		scene.CreateEntity("Seamed grazing surface").AddComponent<MeshComponent>().Mesh = mesh->Handle;
		auto backing = CreateRef<MaterialAsset>();
		backing->Handle = AssetHandle(50604);
		backing->BaseColorFactor = {0, 1, 0, 1};
		assets.AddMemoryAsset(backing);
		auto behind = scene.CreateEntity("Occluded green backing");
		behind.GetTransform().Translation = {0, 0, -8};
		behind.GetTransform().Scale = {30, 30, 0.1f};
		auto& backingMesh = behind.AddComponent<MeshComponent>();
		backingMesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		backingMesh.Materials = {backing->Handle};
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		auto target = CreateOutput(graphics.GetDevice(), 192, 192);
		const auto camera = CameraView::LookAt({}, {0, 0, -1}, glm::radians(70.0f), 1);
		std::array<Image, 2> controls;
		for (int sampling = 0; sampling < 4; ++sampling)
			for (bool mask : {false, true})
				for (auto debug : {RenderDebugView::BaseColor, RenderDebugView::Normals})
				{
					CAPTURE(sampling);
					CAPTURE(mask);
					CAPTURE(static_cast<int>(debug));
					material->BaseColorTexture = sampling == 0 ? AssetHandle(0) : sampling == 1 ? constant->Handle
																								: texture->Handle;
					material->NormalTexture = sampling >= 2 ? texture->Handle : AssetHandle(0);
					material->HeightTexture = sampling == 3 ? texture->Handle : AssetHandle(0);
					material->Alpha = mask ? AlphaMode::Mask : AlphaMode::Opaque;
					material->Version++;
					settings.DebugView = debug;
					settings.Path = RenderPath::Forward;
					renderer.Render(scene, camera, target.Framebuffer);
					const Image forward = ReadbackTexture(graphics.GetDevice(), target.Texture);
					const size_t control = debug == RenderDebugView::BaseColor ? 0 : 1;
					if (sampling == 0 && !mask)
						controls[control] = forward;
					if (sampling <= 1)
						CHECK(forward.GetPixels() == controls[control].GetPixels());
					const std::string prefix = "R11-POM-" + std::to_string(sampling) + "-" + std::to_string(mask) + "-" + std::to_string(int(debug));
					REQUIRE(forward.SavePNG(Testing::GetTestOutputDirectory() / (prefix + "-Forward.png")));
					for (auto path : {RenderPath::Deferred, RenderPath::Visibility})
					{
						CAPTURE(static_cast<int>(path));
						settings.Path = path;
						renderer.Render(scene, camera, target.Framebuffer);
						REQUIRE(renderer.GetStats().EffectivePath == path);
						const Image candidate = ReadbackTexture(graphics.GetDevice(), target.Texture);
						renderer.Render(scene, camera, target.Framebuffer);
						CHECK(candidate.GetPixels() == ReadbackTexture(graphics.GetDevice(), target.Texture).GetPixels());
						uint64_t difference = 0, outliers = 0, missingInterior = 0;
						const auto isBacking = [](glm::u8vec4 p) { return p.g > 200 && p.r == 0 && p.b == 0; };
						for (uint32_t y = 0; y < 192; ++y)
							for (uint32_t x = 0; x < 192; ++x)
							{
								const auto a = forward.GetPixel(x, y), b = candidate.GetPixel(x, y);
								int largest = 0;
								for (int c = 0; c < 3; ++c)
								{
									const int delta = std::abs(int(a[c]) - int(b[c]));
									difference += uint64_t(delta);
									largest = std::max(largest, delta);
								}
								outliers += largest > 12 ? 1 : 0;
								if (debug == RenderDebugView::BaseColor && isBacking(a) != isBacking(b))
								{
									bool boundary = false;
									for (int dy = -1; dy <= 1; ++dy)
										for (int dx = -1; dx <= 1; ++dx)
											if (int(x) + dx >= 0 && int(x) + dx < 192 && int(y) + dy >= 0 && int(y) + dy < 192)
												boundary |= isBacking(forward.GetPixel(uint32_t(int(x) + dx), uint32_t(int(y) + dy))) != isBacking(a);
									missingInterior += boundary ? 0 : 1;
								}
							}
						CHECK(double(difference) / (192 * 192 * 3) < 2.0);
						CHECK(double(outliers) / (192 * 192) < 0.005);
						CHECK(missingInterior == 0);
						REQUIRE(candidate.SavePNG(Testing::GetTestOutputDirectory() / (prefix + (path == RenderPath::Visibility ? "-Visibility.png" : "-Deferred.png"))));
					}
				}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: Visibility tracks many material texture and mesh edits without stale resolves")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("MaterialEdits");
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().ShowBackground = false;
		auto mesh = MeshPrimitives::CreateCube();
		mesh->Handle = AssetHandle(50700);
		assets.AddMemoryAsset(mesh);
		std::array<Ref<MaterialAsset>, 32> materials;
		std::array<Entity, 32> entities;
		for (size_t i = 0; i < materials.size(); ++i)
		{
			auto material = CreateRef<MaterialAsset>();
			material->Handle = AssetHandle(50800 + i);
			material->BaseColorFactor = {float(i + 1) / 33, 0.3f, float(32 - i) / 33, 1};
			assets.AddMemoryAsset(material);
			materials[i] = material;
			// Creation order deliberately differs from the stable triangle-prefix order.
			auto entity = scene.CreateEntityWithUUID(UUID(32 - i), "Cube");
			entity.GetTransform().Translation = {float(i % 8) - 3.5f, float(i / 8) - 1.5f, 0};
			entity.GetTransform().Scale = glm::vec3(0.8f);
			auto& component = entity.AddComponent<MeshComponent>();
			component.Mesh = mesh->Handle;
			component.Materials = {material->Handle};
			entities[i] = entity;
		}
		const std::array<uint8_t, 4> red{255, 32, 64, 255};
		auto texture = TextureAsset::CreateRGBA8(1, 1, red.data(), false);
		texture->Handle = AssetHandle(50701);
		assets.AddMemoryAsset(texture);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		settings.DebugView = RenderDebugView::BaseColor;
		auto output = CreateOutput(graphics.GetDevice(), 160, 96);
		const auto camera = CameraView::LookAt({0, 0, 8}, {}, glm::radians(50.0f), 160.0f / 96);
		Image previous;
		for (int edit = 0; edit < 6; ++edit)
		{
			CAPTURE(edit);
			if (edit == 1)
				for (auto& material : materials)
				{
					material->BaseColorTexture = texture->Handle;
					material->Version++;
				}
			if (edit == 2)
			{
				texture->Mips[0][0] = 64;
				texture->Mips[0][1] = 255;
				texture->Version++;
			}
			if (edit == 3)
			{
				for (auto& vertex : mesh->Vertices)
					vertex.Position.x *= 0.6f;
				mesh->ComputeBounds();
				mesh->Version++;
			}
			if (edit == 4)
				for (size_t i = 0; i < entities.size(); i += 2)
					scene.DestroyEntity(entities[i]);
			if (edit == 5)
			{
				materials[1]->Alpha = AlphaMode::Blend;
				materials[1]->BaseColorFactor.a = 0.3f;
				materials[1]->Version++;
			}
			settings.Path = RenderPath::Forward;
			renderer.Render(scene, camera, output.Framebuffer);
			const Image forward = ReadbackTexture(graphics.GetDevice(), output.Texture);
			if (edit != 0)
				CHECK(previous.GetPixels() != forward.GetPixels());
			previous = forward;
			settings.Path = RenderPath::Visibility;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectivePath == RenderPath::Visibility);
			const Image visibility = ReadbackTexture(graphics.GetDevice(), output.Texture);
			CHECK(Image::MaxChannelDifference(forward, visibility) <= 1);
			CHECK(renderer.GetStats().VisibilityResolveDrawCalls == (edit < 4 ? 32 : edit == 4 ? 16
																							   : 15));
			const auto allocations = renderer.GetStats().VisibilityAllocationAttempts;
			const auto geometry = renderer.GetStats().VisibilityGeometryBytes;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().VisibilityAllocationAttempts == allocations);
			CHECK(renderer.GetStats().VisibilityGeometryBytes == geometry);
			CHECK(ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixels() == visibility.GetPixels());
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: clipped POM helpers at a projective pole preserve finite covered surfaces and mask depth")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ProjectivePole");
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50900);
		material->BaseColorFactor = {1, 0, 0, 1};
		material->DoubleSided = true;
		material->Alpha = AlphaMode::Mask;
		material->HeightScale = 0.1f;
		const std::array<uint8_t, 4> white{255, 255, 255, 255};
		auto opacity = TextureAsset::CreateRGBA8(1, 1, white.data(), false);
		opacity->Handle = AssetHandle(50901);
		assets.AddMemoryAsset(opacity);
		material->BaseColorTexture = opacity->Handle;
		std::array<uint8_t, 8 * 8 * 4> heights{};
		for (size_t i = 0; i < 8 * 8; ++i)
		{
			heights[i * 4] = (i % 8 + i / 8) % 2 ? 255 : 64;
			heights[i * 4 + 3] = 255;
		}
		auto height = TextureAsset::CreateRGBA8(8, 8, heights.data(), false);
		height->Handle = AssetHandle(50902);
		assets.AddMemoryAsset(height);
		material->HeightTexture = height->Handle;
		assets.AddMemoryAsset(material);
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = AssetHandle(50903);
		mesh->Materials = {material->Handle};
		// Plane y-z/128=-1/16 has its projective pole at helper row64.5 for this128px90degree camera.
		// Covered row65.5 is at depth4; the third vertex is behind the eye, so hardware clipping participates.
		const glm::vec3 normal = glm::normalize(glm::vec3(0, 1, -1.0f / 128));
		mesh->Vertices = {
			{{-1, -0.125f, -8}, normal, {1, 0, 0, 1}, {0, 0}},
			{{1, -0.125f, -8}, normal, {1, 0, 0, 1}, {8, 0}},
			{{0, -0.060546875f, 0.25f}, normal, {1, 0, 0, 1}, {4, 8}}};
		mesh->Indices = {0, 1, 2};
		Submesh submesh;
		submesh.IndexCount = submesh.VertexCount = 3;
		mesh->Submeshes.push_back(submesh);
		mesh->ComputeBounds();
		assets.AddMemoryAsset(mesh);
		scene.CreateEntity("Clipped grazing triangle").AddComponent<MeshComponent>().Mesh = mesh->Handle;
		auto backing = CreateRef<MaterialAsset>();
		backing->Handle = AssetHandle(50904);
		backing->BaseColorFactor = {0, 1, 0, 1};
		assets.AddMemoryAsset(backing);
		auto behind = scene.CreateEntity("Green depth witness");
		behind.GetTransform().Translation = {0, 0, -8};
		behind.GetTransform().Scale = {30, 30, 0.1f};
		auto& backMesh = behind.AddComponent<MeshComponent>();
		backMesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		backMesh.Materials = {backing->Handle};
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		renderer.GetSettings().Shadows = false;
		renderer.GetSettings().GIEnabled = false;
		renderer.GetSettings().DebugView = RenderDebugView::BaseColor;
		auto output = CreateOutput(graphics.GetDevice(), 128, 128);
		auto camera = CameraView::LookAt({}, {0, 0, -1}, glm::radians(90.0f), 1, 0.1f, 10);
		// Exact binary coefficients put the helper on the pole instead of relying on tan(pi/4) rounding.
		camera.Projection[0][0] = camera.Projection[1][1] = 1.0f;
		for (bool textured : {false, true})
			for (bool maskedAway : {false, true})
			{
				material->BaseColorTexture = textured ? height->Handle : opacity->Handle;
				material->Version++;
				for (auto& mip : height->Mips)
					for (size_t alpha = 3; alpha < mip.size(); alpha += 4)
						mip[alpha] = maskedAway ? 0 : 255;
				height->Version++;
				opacity->Mips[0][3] = maskedAway ? 0 : 255;
				opacity->Version++;
				Image forward;
				for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
				{
					CAPTURE(static_cast<int>(path));
					CAPTURE(maskedAway);
					CAPTURE(textured);
					renderer.GetSettings().Path = path;
					renderer.Render(scene, camera, output.Framebuffer);
					REQUIRE(renderer.GetStats().EffectivePath == path);
					const Image current = ReadbackTexture(graphics.GetDevice(), output.Texture);
					REQUIRE(current.SavePNG(Testing::GetTestOutputDirectory() / ("R11-Pole-" + std::to_string(textured) + "-" + std::to_string(maskedAway) + "-" + std::to_string(static_cast<int>(path)) + ".png")));
					const auto linear = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
					CHECK(std::all_of(linear.begin(), linear.end(), [](const glm::vec3& color) { return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b); }));
					const auto witness = linear[65 * 128 + 64];
					if (textured && !maskedAway)
						CHECK(witness.r == doctest::Approx(double(height->Mips.back()[0]) / 255.0).epsilon(0.003));
					else
						CHECK(witness.r == doctest::Approx(maskedAway ? 0.0 : 1.0));
					CHECK(witness.g == doctest::Approx(maskedAway ? 1.0 : 0.0));
					if (path == RenderPath::Forward)
						forward = current;
					else if (textured && !maskedAway && path == RenderPath::Visibility)
					{
						// Use the established POM interpolation budget away from the pole; its helper contract stays exact.
						uint64_t difference = 0, outliers = 0;
						bool alphaExact = true, coverageExact = true, poleBandExact = true;
						for (uint32_t y = 0; y < 128; ++y)
							for (uint32_t x = 0; x < 128; ++x)
							{
								const auto a = forward.GetPixel(x, y), b = current.GetPixel(x, y);
								int largest = 0;
								for (int channel = 0; channel < 3; ++channel)
								{
									const int delta = std::abs(int(a[channel]) - int(b[channel]));
									difference += uint64_t(delta);
									largest = std::max(largest, delta);
								}
								outliers += largest > 12 ? 1 : 0;
								alphaExact &= a.a == b.a;
								coverageExact &= (a.g > a.r) == (b.g > b.r);
								// Rows64/65 are the pole quad; include its one-row border on both sides.
								if (y >= 63 && y <= 66)
									poleBandExact &= a == b;
							}
						CHECK(double(difference) / (128 * 128 * 3) < 2.0);
						CHECK(double(outliers) / (128 * 128) < 0.005);
						CHECK(alphaExact);
						CHECK(coverageExact);
						CHECK(poleBandExact);
					}
					else
						CHECK(current.GetPixels() == forward.GetPixels());
					renderer.Render(scene, camera, output.Framebuffer);
					CHECK(current.GetPixels() == ReadbackTexture(graphics.GetDevice(), output.Texture).GetPixels());
				}
			}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

namespace {

	void CheckAmbientEffects(RenderPath path)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			Scene scene("AmbientEffects");
			BuildTestScene(scene, assets);
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.Path = path;
			settings.Shadows = false;
			settings.GIEnabled = false;
			auto target = CreateOutput(graphics.GetDevice(), 97, 65);
			auto camera = CameraView::LookAt({2.5f, 2.0f, 3.5f}, {0, 0.3f, 0}, glm::radians(55.0f), 97.0f / 65.0f);
			auto render = [&]() {
				renderer.Render(scene, camera, target.Framebuffer);
				return ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
			};
			const auto baseline = render();
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			settings.SSAOEnabled = true;
			settings.SSAOStrength = 0;
			CHECK(render() == baseline);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			settings.SSAOStrength = 2;
			settings.DebugView = RenderDebugView::AmbientOcclusion;
			const auto ao = render();
			const auto aoDisplay = ReadbackTexture(graphics.GetDevice(), target.Texture);
			settings.GIDebugScale = 8;
			render();
			CHECK(Image::MaxChannelDifference(aoDisplay, ReadbackTexture(graphics.GetDevice(), target.Texture)) == 0);
			settings.GIDebugScale = 1;
			REQUIRE(renderer.GetStats().SSAOActive);
			CHECK(renderer.GetStats().SSAOPasses == (path == RenderPath::Forward ? 6 : 5));
			CHECK(renderer.GetStats().EffectOwnedBytes > 0);
			CHECK(renderer.GetStats().EffectPeakBytes <= PostEffectBudget::DefaultCapacity);
			uint32_t contactPixels = 0;
			for (const auto& pixel : ao)
			{
				CHECK(std::isfinite(pixel.x));
				CHECK(pixel.x >= 0);
				CHECK(pixel.x <= 1);
				contactPixels += pixel.x > 0 && pixel.x < 0.98f;
			}
			CHECK(contactPixels > 5);
			const auto attempts = renderer.GetStats().EffectAllocationAttempts;
			CHECK(render() == ao);
			CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
			for (auto diagnostic : {RenderDebugView::DirectDiffuse, RenderDebugView::DirectSpecular, RenderDebugView::Emissive, RenderDebugView::EnvironmentSpecular})
			{
				settings.DebugView = diagnostic;
				const auto enabled = render();
				settings.SSAOEnabled = false;
				CHECK(render() == enabled);
				CHECK(renderer.GetStats().EffectOwnedBytes == 0);
				settings.SSAOEnabled = true;
			}
			settings.SSAOEnabled = false;
			settings.DebugView = RenderDebugView::None;
			CHECK(render() == baseline);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			settings.GIFreeze = true;
			for (auto mode : {RenderDebugView::EnvironmentDiffuse, RenderDebugView::AmbientFill, RenderDebugView::IndirectDiffuse})
			{
				CAPTURE(mode);
				settings.DebugView = mode;
				settings.AmbientFillIntensity = mode == RenderDebugView::AmbientFill ? 0.5f : 0;
				if (mode == RenderDebugView::IndirectDiffuse)
				{
					auto volume = scene.CreateEntity("AmbientProbeVolume");
					volume.GetTransform().Translation = glm::vec3(-2);
					volume.AddComponent<GIVolumeComponent>().ProbeSpacing = 2;
					settings.GIEnabled = true;
				}
				settings.SSAOEnabled = true;
				const auto attenuated = render();
				settings.SSAOEnabled = false;
				const auto unoccluded = render();
				uint32_t darkened = 0;
				for (size_t i = 0; i < ao.size(); ++i)
				{
					darkened += glm::length(unoccluded[i] - attenuated[i]) > 0.001f;
					for (int channel = 0; channel < 3; ++channel)
						CHECK(std::abs(attenuated[i][channel] - unoccluded[i][channel] * ao[i].x) < 0.003f * std::max(1.0f, unoccluded[i][channel]));
				}
				CHECK(darkened > 5);
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}

}

TEST_CASE("SceneRenderer: Forward SSAO is deterministic and affects only diffuse ambient lighting")
{
	CheckAmbientEffects(RenderPath::Forward);
}

TEST_CASE("SceneRenderer: Deferred SSAO is deterministic and affects only diffuse ambient lighting")
{
	CheckAmbientEffects(RenderPath::Deferred);
}

TEST_CASE("SceneRenderer: Visibility SSAO is deterministic and affects only diffuse ambient lighting")
{
	CheckAmbientEffects(RenderPath::Visibility);
}

TEST_CASE("SceneRenderer: artistic fill works without environment or GI and obeys diffuse material response")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(51001);
		material->BaseColorFactor = {0.5f, 0.25f, 0.125f, 1};
		material->RoughnessFactor = 1;
		assets.AddMemoryAsset(material);
		Scene scene("ArtisticFill");
		auto entity = scene.CreateEntity("DiffuseCube");
		auto& mesh = entity.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		mesh.Materials = {material->Handle};
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.DebugView = RenderDebugView::AmbientFill;
		auto target = CreateOutput(graphics.GetDevice(), 33, 33);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			settings.Path = path;
			settings.AmbientFillColor = {1, 0.5f, 0.25f};
			settings.AmbientFillIntensity = 2;
			renderer.Render(scene, camera, target.Framebuffer);
			const auto hdr = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
			const glm::vec3 expected = glm::vec3(material->BaseColorFactor) * settings.AmbientFillColor * 2.0f * 0.96f;
			for (int channel = 0; channel < 3; ++channel)
				CHECK(hdr[16 * 33 + 16][channel] == doctest::Approx(double(expected[channel])).epsilon(0.002));
			settings.AmbientFillIntensity = 0;
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget())[16 * 33 + 16] == glm::vec3(0));
			settings.AmbientFillIntensity = 2;
			material->MetallicFactor = 1;
			++material->Version;
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget())[16 * 33 + 16] == glm::vec3(0));
			material->MetallicFactor = 0;
			++material->Version;
		}
		auto volume = scene.CreateEntity("FillGIVolume");
		volume.GetTransform().Translation = glm::vec3(-2);
		volume.AddComponent<GIVolumeComponent>().ProbeSpacing = 2;
		settings.GIFreeze = true;
		settings.GIEnabled = true;
		renderer.Render(scene, camera, target.Framebuffer);
		const auto coveredFill = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
		CHECK(renderer.GetGIState().Available);
		settings.GIEnabled = false;
		renderer.Render(scene, camera, target.Framebuffer);
		CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()) == coveredFill);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: SSAO separates coverage from signed orthographic depth on tiny and odd targets")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("OrthographicAO");
		auto cube = scene.CreateEntity("FlatReceiver");
		cube.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.SSAOEnabled = true;
		settings.DebugView = RenderDebugView::AmbientOcclusion;
		CameraView camera;
		camera.Orthographic = true;
		camera.Near = -100;
		camera.Far = 100;
		camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, camera.Far, camera.Near);
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			settings.Path = path;
			for (uint32_t size : {1u, 3u, 17u})
			{
				auto output = CreateOutput(graphics.GetDevice(), size, size);
				// Front face at signed negative, zero, positive and exactly far-plane depth.
				for (float z : {1.0f, -0.5f, -2.0f, -100.5f})
				{
					CAPTURE(path);
					CAPTURE(size);
					CAPTURE(z);
					cube.GetTransform().Translation.z = z;
					renderer.Render(scene, camera, output.Framebuffer);
					REQUIRE(renderer.GetStats().SSAOActive);
					const auto hdr = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
					CHECK(hdr[(size / 2) * size + size / 2] == glm::vec3(1));
					for (const auto& pixel : hdr)
					{
						CHECK(std::isfinite(pixel.x));
						CHECK(pixel.x >= 0);
						CHECK(pixel.x <= 1);
					}
				}
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: SSAO allocation failure falls back locally and recovers without changing render path")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class FailureFactory : public PostEffectResourceFactory
		{
		public:
			uint32_t FailAt = 0, Calls = 0;
			int FailLayoutSpace = -1;
			bool Unsupported = false;
			bool SupportsVirtualResources(nvrhi::IDevice* device) override { return !Unsupported && PostEffectResourceFactory::SupportsVirtualResources(device); }
			nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc) override
			{
				if (int(desc.registerSpace) == FailLayoutSpace)
				{
					FailLayoutSpace = -1;
					return nullptr;
				}
				return PostEffectResourceFactory::CreateBindingLayout(device, desc);
			}
			nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateTexture(device, desc);
			}
			nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateFramebuffer(device, desc);
			}
			nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
			}
			nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreatePipeline(device, desc, info);
			}
		} factory;
		AssetManager assets;
		Scene scene("FailedAO");
		BuildTestScene(scene, assets);
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		settings.Path = RenderPath::Deferred;
		auto target = CreateOutput(graphics.GetDevice(), 17, 17);
		auto camera = CameraView::LookAt({2, 2, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(scene, camera, target.Framebuffer);
		const auto baseline = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
		// Query, backing, framebuffer, binding and pipeline failures after partial allocation.
		for (uint32_t failAt : {1u, 4u, 12u, 13u, 14u})
		{
			CAPTURE(failAt);
			factory.Calls = 0;
			factory.FailAt = failAt;
			settings.SSAOEnabled = true;
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK_FALSE(renderer.GetStats().SSAOActive);
			CHECK_FALSE(std::string(renderer.GetStats().SSAOFallbackReason).empty());
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			CHECK(renderer.GetStats().EffectivePath == RenderPath::Deferred);
			CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()) == baseline);
			factory.FailAt = 0;
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK(renderer.GetStats().SSAOActive);
			settings.SSAOEnabled = false;
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
		}
		factory.Unsupported = true;
		settings.SSAOEnabled = true;
		for (int frame = 0; frame < 3; ++frame)
		{
			renderer.Render(scene, camera, target.Framebuffer);
			CHECK_FALSE(renderer.GetStats().SSAOActive);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
			CHECK_FALSE(std::string(renderer.GetStats().SSAOFallbackReason).empty());
		}
		factory.Unsupported = false;
		for (int space : {0, 1})
		{
			SceneRenderer retry(graphics, assets, nullptr, nullptr, &factory);
			REQUIRE(retry.Initialize());
			retry.GetSettings().Shadows = false;
			retry.GetSettings().GIEnabled = false;
			retry.GetSettings().SSAOEnabled = true;
			factory.FailLayoutSpace = space;
			retry.Render(scene, camera, target.Framebuffer);
			CHECK_FALSE(retry.GetStats().SSAOActive);
			CHECK(retry.GetStats().EffectOwnedBytes == 0);
			retry.Render(scene, camera, target.Framebuffer);
			CHECK(retry.GetStats().SSAOActive);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: optional profiling records completed GPU samples without changing pixels")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ProfiledScene");
		BuildTestScene(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		auto target = CreateOutput(graphics.GetDevice(), 33, 33);
		auto camera = CameraView::LookAt({2, 2, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		renderer.Render(scene, camera, target.Framebuffer);
		const auto baseline = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
		CHECK_FALSE(renderer.GetStats().CPURenderMilliseconds);
		CHECK_FALSE(renderer.GetStats().GPUTiming);
		settings.ProfilingEnabled = true;
		INFO("timestamp bits=", graphics.GetInfo().GraphicsTimestampValidBits,
			 ", period ns=", graphics.GetInfo().TimestampPeriodNanoseconds);
		for (int frame = 0; frame < 3; ++frame)
		{
			const auto previousInvalid = renderer.GetStats().GPUProfilingInvalid;
			renderer.Render(scene, camera, target.Framebuffer);
			if (frame > 0 && graphics.GetInfo().GraphicsTimestampValidBits >= 32 && graphics.GetInfo().TimestampPeriodNanoseconds > 0)
			{
				const auto& stats = renderer.GetStats();
				INFO("status=", stats.GPUProfilingStatus, ", last=", stats.GPUProfilingLastResult,
					 ", invalid=", stats.GPUProfilingInvalid, ", previous invalid=", previousInvalid,
					 ", pending=", stats.GPUProfilingPending, ", skipped=", stats.GPUProfilingSkipped);
				const uint64_t retiredFrame = static_cast<uint64_t>(frame + 1);
				const bool fresh = stats.GPUTiming && stats.GPUTiming->Context.Frame == retiredFrame;
				CHECK(stats.GPUProfilingInvalid == previousInvalid + (fresh ? 0u : 1u));
				CHECK(stats.GPUProfilingLastResult == (fresh ? "ready" : "invalid: timestamp interval"));
				if (stats.GPUTiming)
					CHECK(stats.GPUTiming->Context.Frame <= retiredFrame);
				CHECK(stats.GPUProfilingPending == 1);
				CHECK(stats.GPUProfilingSkipped == 0);
			}
			// Tests may retire deterministically; production Render never waits for profiling queries.
			graphics.WaitForIdle();
			CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()) == baseline);
			REQUIRE(renderer.GetStats().CPURenderMilliseconds);
			CHECK(std::isfinite(*renderer.GetStats().CPURenderMilliseconds));
			CHECK(*renderer.GetStats().CPURenderMilliseconds >= 0);
		}
		if (graphics.GetInfo().GraphicsTimestampValidBits >= 32 && graphics.GetInfo().TimestampPeriodNanoseconds > 0)
		{
			const auto& stats = renderer.GetStats();
			INFO("status=", stats.GPUProfilingStatus, ", last=", stats.GPUProfilingLastResult,
				 ", invalid=", stats.GPUProfilingInvalid, ", pending=", stats.GPUProfilingPending,
				 ", skipped=", stats.GPUProfilingSkipped);
			if (stats.GPUTiming)
			{
				CHECK(stats.GPUTiming->Context.InternalWidth == 33);
				REQUIRE(stats.GPUTiming->Milliseconds[0]);
				CHECK(std::isfinite(*stats.GPUTiming->Milliseconds[0]));
				CHECK(*stats.GPUTiming->Milliseconds[0] >= 0);
				CHECK_FALSE(stats.GPUTiming->Milliseconds[static_cast<size_t>(RenderTimingSection::AmbientOcclusion)]);
			}
			else
			{
				CHECK(stats.GPUProfilingInvalid == 2);
				CHECK(stats.GPUProfilingLastResult == "invalid: timestamp interval");
				CHECK(stats.GPUProfilingStatus == "pending");
			}
			CHECK(stats.GPUProfilingPending == 1);
			CHECK(stats.GPUProfilingSkipped == 0);
			REQUIRE(stats.ProfilingDevice);
			CHECK(stats.ProfilingDevice->AdapterName == graphics.GetInfo().AdapterName);
		}
		else
			CHECK(renderer.GetStats().GPUProfilingStatus == "timestamps unavailable");
		settings.ProfilingEnabled = false;
		renderer.Render(scene, camera, target.Framebuffer);
		CHECK_FALSE(renderer.GetStats().CPURenderMilliseconds);
		CHECK(renderer.GetStats().GPUProfilingStatus == "disabled");
		CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()) == baseline);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: AO rejects sloped self occlusion and keeps separated foreground edges unoccluded")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("AOGuides");
		auto makePlane = [&](uint64_t handle, float left, float right, float depth, float slope) {
			auto mesh = CreateRef<MeshAsset>();
			mesh->Handle = AssetHandle(handle);
			const auto normal = glm::normalize(glm::vec3(0, -slope, 1));
			mesh->Vertices = {{{left, -2, depth - 2 * slope}, normal, {1, 0, 0, 1}, {0, 1}},
							  {{right, -2, depth - 2 * slope}, normal, {1, 0, 0, 1}, {1, 1}},
							  {{right, 2, depth + 2 * slope}, normal, {1, 0, 0, 1}, {1, 0}},
							  {{left, 2, depth + 2 * slope}, normal, {1, 0, 0, 1}, {0, 0}}};
			mesh->Indices = {0, 1, 2, 0, 2, 3};
			Submesh submesh;
			submesh.VertexCount = 4;
			submesh.IndexCount = 6;
			mesh->Submeshes.push_back(submesh);
			mesh->ComputeBounds();
			assets.AddMemoryAsset(mesh);
			return mesh;
		};
		auto sloped = makePlane(52001, -2, 2, -3, 0.3f);
		auto receiver = scene.CreateEntity("Receiver");
		receiver.AddComponent<MeshComponent>().Mesh = sloped->Handle;
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.SSAOEnabled = true;
		settings.SSAORadius = 0.6f;
		settings.SSAOStrength = 2;
		settings.DebugView = RenderDebugView::AmbientOcclusion;
		auto output = CreateOutput(graphics.GetDevice(), 97, 65);
		CameraView camera;
		camera.Orthographic = true;
		camera.Near = 0.1f;
		camera.Far = 10;
		camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, camera.Far, camera.Near);
		auto render = [&]() {
			renderer.Render(scene, camera, output.Framebuffer);
			return ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
		};
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			settings.Path = path;
			for (const auto& pixel : render())
			{
				INFO("path=", static_cast<int>(path), ", AO=", pixel.x, ",", pixel.y, ",", pixel.z);
				CHECK(pixel == glm::vec3(1));
			}
		}
		auto flat = makePlane(52002, -2, 2, -3, 0);
		receiver.GetComponent<MeshComponent>().Mesh = flat->Handle;
		auto occluder = scene.CreateEntity("Contact");
		occluder.GetTransform().Translation = {-0.1f, 0, -2.8f};
		occluder.GetTransform().Scale = {0.4f, 1, 0.4f};
		occluder.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
		auto foregroundMesh = makePlane(52003, 0, 2, -1, 0);
		auto foreground = scene.CreateEntity("SeparatedForeground");
		foreground.AddComponent<MeshComponent>().Mesh = foregroundMesh->Handle;
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			settings.Path = path;
			const auto contact = render();
			uint32_t dark = 0;
			for (uint32_t y = 0; y < 65; ++y)
				for (uint32_t x = 0; x < 97; ++x)
				{
					const auto value = contact[y * 97 + x].x;
					if (x >= 49)
						CHECK(value == 1);
					else
						dark += value < 0.98f;
				}
			CHECK(dark > 5);
			occluder.GetTransform().Translation.z = -6;
			for (const auto& pixel : render())
			{
				INFO("path=", static_cast<int>(path), ", AO=", pixel.x, ",", pixel.y, ",", pixel.z);
				CHECK(pixel == glm::vec3(1));
			}
			occluder.GetTransform().Translation.z = -2.8f;
		}
		std::array<uint8_t, 32 * 32 * 4> checks{};
		for (size_t i = 0; i < 32 * 32; ++i)
		{
			const bool high = (i % 32 / 4 + i / 32 / 4) % 2 != 0;
			checks[4 * i] = high ? 220 : 30;
			checks[4 * i + 1] = checks[4 * i + 2] = 128;
			checks[4 * i + 3] = high ? 255 : 0;
		}
		auto texture = TextureAsset::CreateRGBA8(32, 32, checks.data(), false);
		texture->Handle = AssetHandle(52004);
		assets.AddMemoryAsset(texture);
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(52005);
		material->Alpha = AlphaMode::Mask;
		material->BaseColorTexture = material->HeightTexture = texture->Handle;
		material->HeightScale = 0.12f;
		material->DoubleSided = true;
		assets.AddMemoryAsset(material);
		foreground.GetComponent<MeshComponent>().Materials = {material->Handle};
		for (float handedness : {1.0f, -1.0f})
		{
			foreground.GetTransform().Scale.x = handedness;
			settings.Path = RenderPath::Forward;
			const auto reference = render();
			for (auto path : {RenderPath::Deferred, RenderPath::Visibility})
			{
				settings.Path = path;
				const auto actual = render();
				for (size_t i = 0; i < actual.size(); ++i)
					CHECK(std::abs(actual[i].x - reference[i].x) < 0.005f);
			}
		}
		// Alpha-one blending still takes the transparent path and must not inherit opaque screen AO.
		foreground.GetTransform().Scale.x = 1;
		foreground.GetTransform().Translation.x = -1;
		material->Alpha = AlphaMode::Blend;
		material->BaseColorTexture = material->HeightTexture = AssetHandle(0);
		material->BaseColorFactor.a = 1;
		++material->Version;
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			settings.Path = path;
			const auto transparent = render();
			for (uint32_t y = 10; y < 55; ++y)
				for (uint32_t x = 25; x < 72; ++x)
					CHECK(transparent[y * 97 + x] == glm::vec3(1));
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("Bloom: black and constant HDR fields keep threshold and level normalization on odd and tiny targets")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		Bloom bloom(graphics, budget);
		auto* device = graphics.GetDevice();
		{
			Testing::ScopedAssertionCapture capture;
			CHECK_THROWS_AS(bloom.Render(nullptr, 0, 0), Testing::AssertionFailedError);
		}
		CHECK_FALSE(bloom.Prepare(true, nullptr, nullptr, 0));
		CHECK(budget.GetUsedBytes() == 0);
		for (uint32_t size : {1u, 17u, 32u})
		{
			nvrhi::TextureDesc desc;
			desc.width = size;
			desc.height = size == 17 ? 13 : size;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			auto hdr = device->createTexture(desc);
			REQUIRE(hdr);
			for (uint32_t levels : {1u, 3u, 6u})
			{
				REQUIRE(bloom.Prepare(true, hdr, hdr, levels));
				if (size == 1 && levels == 1)
				{
					Testing::ScopedAssertionCapture capture;
					auto invalidCommands = device->createCommandList();
					CHECK_THROWS_AS(bloom.Render(nullptr, 0, 0), Testing::AssertionFailedError);
					for (float invalid : {-1.0f, 65.0f, std::numeric_limits<float>::quiet_NaN()})
						CHECK_THROWS_AS(bloom.Render(invalidCommands, invalid, 0), Testing::AssertionFailedError);
					for (float invalid : {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()})
						CHECK_THROWS_AS(bloom.Render(invalidCommands, 0, invalid), Testing::AssertionFailedError);
				}
				const auto attempts = bloom.GetAllocationAttempts();
				REQUIRE(bloom.Prepare(true, hdr, hdr, levels));
				CHECK(bloom.GetAllocationAttempts() == attempts);
				for (uint32_t scenario = 0; scenario < 4; ++scenario)
				{
					CAPTURE(size);
					CAPTURE(levels);
					CAPTURE(scenario);
					auto commands = device->createCommandList();
					commands->open();
					PostEffectTextures::TrackUsage(commands, hdr, nvrhi::ResourceStates::CopyDest);
					commands->clearTextureFloat(hdr, nvrhi::AllSubresources, scenario == 0 ? nvrhi::Color(0) : nvrhi::Color(4, 2, 1, 1));
					const float threshold = scenario == 2 ? 64.0f : scenario == 3 ? 4.0f
																				  : 2.0f;
					bloom.Render(commands, threshold, scenario == 3 ? 1.0f : 0.0f);
					commands->close();
					device->executeCommandList(commands);
					const float luminance = glm::dot(glm::vec3(4, 2, 1), glm::vec3(0.2126f, 0.7152f, 0.0722f));
					const glm::vec3 expected = scenario == 1   ? glm::vec3(4, 2, 1) * ((luminance - 2) / luminance)
											   : scenario == 3 ? glm::vec3(4, 2, 1) * (luminance / 16)
															   : glm::vec3(0);
					for (const auto& pixel : ReadLinearHDR(device, bloom.GetTexture()))
						for (int channel = 0; channel < 3; ++channel)
						{
							CHECK(std::isfinite(pixel[channel]));
							CHECK(std::abs(pixel[channel] - expected[channel]) < 0.003f);
						}
				}
			}
			bloom.Release();
			CHECK(budget.GetUsedBytes() == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("Bloom: additional normalized levels spread a bright impulse without modifying the HDR source")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 32;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		auto hdr = device->createTexture(desc);
		REQUIRE(hdr);
		std::vector<uint16_t> pixels(32 * 32 * 4, 0);
		for (uint32_t channel = 0; channel < 3; ++channel)
			pixels[(16 * 32 + 16) * 4 + channel] = glm::packHalf1x16(32);
		auto commands = device->createCommandList();
		commands->open();
		commands->writeTexture(hdr, 0, 0, pixels.data(), 32 * 8);
		commands->close();
		device->executeCommandList(commands);
		const auto original = ReadLinearHDR(device, hdr);
		PostEffectBudget budget;
		Bloom bloom(graphics, budget);
		std::array<double, 2> spread{};
		for (size_t trial = 0; trial < 2; ++trial)
		{
			REQUIRE(bloom.Prepare(true, hdr, hdr, trial == 0 ? 1 : 5));
			commands->open();
			bloom.Render(commands, 0, 0);
			commands->close();
			device->executeCommandList(commands);
			const auto filtered = ReadLinearHDR(device, bloom.GetTexture());
			double weight = 0;
			for (uint32_t y = 0; y < 16; ++y)
				for (uint32_t x = 0; x < 16; ++x)
				{
					const double value = static_cast<double>(filtered[y * 16 + x].x);
					REQUIRE(std::isfinite(value));
					CHECK(value >= 0);
					const double dx = static_cast<double>(x) - 8, dy = static_cast<double>(y) - 8;
					spread[trial] += value * (dx * dx + dy * dy);
					weight += value;
				}
			REQUIRE(weight > 0);
			spread[trial] /= weight;
			CHECK(ReadLinearHDR(device, hdr) == original);
		}
		CHECK(spread[1] > spread[0] + 1);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: bloom bypass is exact and enabled bloom preserves HDR while glowing before tonemapping")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(53001);
		material->BaseColorFactor = {0, 0, 0, 1};
		material->EmissiveFactor = {8, 2, 1};
		assets.AddMemoryAsset(material);
		Scene scene("BloomEmitter");
		auto emitter = scene.CreateEntity("Emitter");
		emitter.GetTransform().Scale = glm::vec3(0.25f);
		auto& mesh = emitter.AddComponent<MeshComponent>();
		mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
		mesh.Materials = {material->Handle};
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.Tonemapper = TonemapOperator::None;
		auto output = CreateOutput(graphics.GetDevice(), 65, 65);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 1);
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			settings.Path = path;
			settings.BloomEnabled = false;
			settings.DebugView = RenderDebugView::None;
			renderer.Render(scene, camera, output.Framebuffer);
			const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
			const auto hdr = ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget());
			settings.BloomEnabled = true;
			settings.BloomIntensity = 0;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			settings.BloomIntensity = 0.5f;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().BloomActive);
			CHECK(renderer.GetStats().BloomPasses == 9);
			CHECK(ReadLinearHDR(graphics.GetDevice(), renderer.GetHDRTarget()) == hdr);
			const auto bloom = ReadbackTexture(graphics.GetDevice(), output.Texture);
			uint32_t halo = 0;
			for (uint32_t y = 0; y < 65; ++y)
				for (uint32_t x = 0; x < 65; ++x)
					halo += baseline.GetPixel(x, y).r < 2 && bloom.GetPixel(x, y).r > 5;
			CHECK(halo > 10);
			const auto attempts = renderer.GetStats().EffectAllocationAttempts;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
			CHECK(Image::MaxChannelDifference(bloom, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
			settings.DebugView = RenderDebugView::Emissive;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK_FALSE(renderer.GetStats().BloomActive);
			CHECK(renderer.GetStats().EffectOwnedBytes == 0);
			settings.DebugView = RenderDebugView::None;
			settings.BloomEnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("Bloom: partial resource failures release backing and retry every missing object")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class FailureFactory : public PostEffectResourceFactory
		{
		public:
			uint32_t Calls = 0, FailAt = 0;
			nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateTexture(device, desc);
			}
			nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateFramebuffer(device, desc);
			}
			nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateBindingLayout(device, desc);
			}
			nvrhi::SamplerHandle CreateSampler(nvrhi::IDevice* device, const nvrhi::SamplerDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateSampler(device, desc);
			}
			nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
			}
			nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreatePipeline(device, desc, info);
			}
		} factory;
		nvrhi::TextureDesc desc;
		desc.width = 17;
		desc.height = 13;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		auto hdr = graphics.GetDevice()->createTexture(desc);
		REQUIRE(hdr);
		PostEffectBudget budget;
		for (uint32_t failAt : {1u, 2u, 11u, 12u, 13u, 14u, 23u, 24u})
		{
			CAPTURE(failAt);
			Bloom bloom(graphics, budget, &factory);
			factory.Calls = 0;
			factory.FailAt = failAt;
			CHECK_FALSE(bloom.Prepare(true, hdr, hdr, 3));
			CHECK_FALSE(bloom.IsReady());
			CHECK(budget.GetUsedBytes() == 0);
			CHECK_FALSE(std::string(bloom.GetFallbackReason()).empty());
			factory.FailAt = 0;
			REQUIRE(bloom.Prepare(true, hdr, hdr, 3));
			CHECK(budget.GetUsedBytes() > 0);
			CHECK(bloom.GetPassCount() == 5);
			bloom.Release();
			CHECK(budget.GetUsedBytes() == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("Bloom: invalid and negative channels are isolated before filtering and original HDR bits are preserved")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 16;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		auto hdr = device->createTexture(desc);
		REQUIRE(hdr);
		std::vector<uint16_t> pixels(16 * 16 * 4, 0);
		pixels[(8 * 16 + 8) * 4] = 0x7c00;     // +Inf
		pixels[(8 * 16 + 8) * 4 + 1] = 0x7e00; // NaN
		pixels[(8 * 16 + 8) * 4 + 2] = 0xfc00; // -Inf
		for (uint32_t channel = 0; channel < 3; ++channel)
		{
			pixels[(8 * 16 + 9) * 4 + channel] = glm::packHalf1x16(8);
			pixels[(9 * 16 + 8) * 4 + channel] = glm::packHalf1x16(-4);
		}
		auto commands = device->createCommandList();
		commands->open();
		commands->writeTexture(hdr, 0, 0, pixels.data(), 16 * 8);
		commands->close();
		device->executeCommandList(commands);
		const auto original = ReadLinearHDR(device, hdr);
		PostEffectBudget budget;
		Bloom bloom(graphics, budget);
		REQUIRE(bloom.Prepare(true, hdr, hdr, 1));
		commands->open();
		bloom.Render(commands, 0, 0);
		commands->close();
		device->executeCommandList(commands);
		const auto filtered = ReadLinearHDR(device, bloom.GetTexture());
		CHECK(filtered[4 * 8 + 4] == glm::vec3(2));
		for (const auto& pixel : filtered)
			for (int channel = 0; channel < 3; ++channel)
			{
				CHECK(std::isfinite(pixel[channel]));
				CHECK(pixel[channel] >= 0);
			}
		const auto after = ReadLinearHDR(device, hdr);
		for (size_t pixel = 0; pixel < after.size(); ++pixel)
			for (int channel = 0; channel < 3; ++channel)
				CHECK(std::bit_cast<uint32_t>(after[pixel][channel]) == std::bit_cast<uint32_t>(original[pixel][channel]));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("Bloom: equal scene linear luminance receives equal threshold gain across different hues")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 8;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		auto hdr = device->createTexture(desc);
		REQUIRE(hdr);
		PostEffectBudget budget;
		Bloom bloom(graphics, budget);
		REQUIRE(bloom.Prepare(true, hdr, hdr, 3));
		const glm::vec3 weights(0.2126f, 0.7152f, 0.0722f);
		for (int channel = 0; channel < 3; ++channel)
		{
			glm::vec3 color(0);
			color[channel] = 2 / weights[channel];
			auto commands = device->createCommandList();
			commands->open();
			PostEffectTextures::TrackUsage(commands, hdr, nvrhi::ResourceStates::CopyDest);
			commands->clearTextureFloat(hdr, nvrhi::AllSubresources, nvrhi::Color(color.x, color.y, color.z, 1));
			bloom.Render(commands, 1, 0);
			commands->close();
			device->executeCommandList(commands);
			for (const auto& pixel : ReadLinearHDR(device, bloom.GetTexture()))
				CHECK(std::abs(glm::dot(pixel, weights) - 1) < 0.002f);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("Bloom: area downsampling and integrated upsampling preserve odd center and edge impulse means")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		PostEffectBudget budget;
		Bloom bloom(graphics, budget);
		for (auto size : {glm::uvec2(3, 1), glm::uvec2(1, 3), glm::uvec2(3, 3), glm::uvec2(9, 1), glm::uvec2(17, 13)})
		{
			nvrhi::TextureDesc desc;
			desc.width = size.x;
			desc.height = size.y;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			auto hdr = device->createTexture(desc);
			REQUIRE(hdr);

			for (uint32_t levels : {1u, 2u, 3u, 6u})
			{
				REQUIRE(bloom.Prepare(true, hdr, hdr, levels));
				CAPTURE(levels);
				for (auto position : {glm::uvec2(0), size / 2u, size - 1u})
				{
					CAPTURE(size.x);
					CAPTURE(size.y);
					CAPTURE(position.x);
					CAPTURE(position.y);
					std::vector<uint16_t> pixels(size.x * size.y * 4, 0);
					for (uint32_t channel = 0; channel < 3; ++channel)
						pixels[(position.y * size.x + position.x) * 4 + channel] = glm::packHalf1x16(1);
					auto commands = device->createCommandList();
					commands->open();
					commands->writeTexture(hdr, 0, 0, pixels.data(), size.x * 8);
					bloom.Render(commands, 0, 0);
					commands->close();
					device->executeCommandList(commands);
					const auto result = ReadLinearHDR(device, bloom.GetTexture());
					double sum = 0;
					for (const auto& pixel : result)
						sum += static_cast<double>(pixel.r);
					const double expected = 1.0 / (size.x * size.y);
					CHECK(std::abs(sum / static_cast<double>(result.size()) - expected) < expected * 0.001);
				}
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: render scale changes only scene raster dimensions and restores the native baseline on every path")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ScaledScene");
		BuildTestScene(scene, assets);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.Shadows = false;
		settings.GIEnabled = false;
		auto output = CreateOutput(graphics.GetDevice(), 65, 49);
		auto camera = CameraView::LookAt({0, 2, 4}, {0, 0.5f, 0}, glm::radians(60.0f), 65.0f / 49);
		const auto projection = camera.Projection;
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			settings.Path = path;
			settings.InternalRenderScale = 1;
			renderer.Render(scene, camera, output.Framebuffer);
			const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
			for (float factor : {0.75f, 0.5f, 1.0f})
			{
				settings.InternalRenderScale = factor;
				renderer.Render(scene, camera, output.Framebuffer);
				const auto expected = RenderScale::CalculateDimensions(65, 49, factor);
				CHECK(renderer.GetHDRTarget()->getDesc().width == expected.x);
				CHECK(renderer.GetHDRTarget()->getDesc().height == expected.y);
				const auto& stats = renderer.GetStats();
				CHECK(stats.InternalWidth == expected.x);
				CHECK(stats.InternalHeight == expected.y);
				CHECK(stats.OutputWidth == 65);
				CHECK(stats.OutputHeight == 49);
				CHECK(stats.EffectiveRenderScale == factor);
				CHECK(stats.NativeDepthPasses == 0);
				CHECK(stats.EffectOwnedBytes == 0);
				CHECK(camera.Projection == projection);
			}
			CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: scaled scene keeps screen text native and caches world text depth allocation failure until request changes")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		class FailureFactory : public PostEffectResourceFactory
		{
		public:
			bool Fail = false;
			nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) override
			{
				return Fail ? nullptr : PostEffectResourceFactory::CreateTexture(device, desc);
			}
		} factory;
		AssetManager assets;
		Scene scene("ScaledText");
		auto entity = scene.CreateEntity("Text");
		entity.GetTransform().Translation = {0.05f, 0.05f, 0};
		auto& text = entity.AddComponent<TextComponent>();
		text.Text = "Native UI";
		text.Size = 180;
		text.ScreenSpace = true;
		SceneRenderer renderer(graphics, assets, nullptr, nullptr, &factory);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = false;
		settings.Shadows = false;
		settings.DefaultEnvironmentIntensity = 0;
		auto output = CreateOutput(graphics.GetDevice(), 129, 65);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 129.0f / 65);
		renderer.Render(scene, camera, output.Framebuffer);
		const auto baseline = ReadbackTexture(graphics.GetDevice(), output.Texture);
		CHECK(renderer.GetStats().TextGlyphs == 8);
		uint32_t whitePixels = 0;
		for (uint32_t y = 0; y < 65; ++y)
			for (uint32_t x = 0; x < 129; ++x)
			{
				const auto pixel = baseline.GetPixel(x, y);
				whitePixels += pixel.r > 200 && pixel.g > 200 && pixel.b > 200;
			}
		CHECK(whitePixels > 10);
		settings.InternalRenderScale = 0.5f;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(Image::MaxChannelDifference(baseline, ReadbackTexture(graphics.GetDevice(), output.Texture)) == 0);
		CHECK(renderer.GetStats().NativeDepthPasses == 0);
		text.ScreenSpace = false;
		text.Size = 0.2f;
		entity.GetTransform().Translation = {0, 0, 0};
		factory.Fail = true;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectiveRenderScale == 1);
		CHECK_FALSE(renderer.GetStats().ScaleFallbackReason.empty());
		const auto attempts = renderer.GetStats().EffectAllocationAttempts;
		factory.Fail = false;
		for (int frame = 0; frame < 3; ++frame)
		{
			renderer.Render(scene, camera, output.Framebuffer);
			CHECK(renderer.GetStats().EffectiveRenderScale == 1);
			CHECK(renderer.GetStats().EffectAllocationAttempts == attempts);
			CHECK(renderer.GetStats().StaticLightingGenerations == 0);
		}
		settings.InternalRenderScale = 0.75f;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectiveRenderScale == 0.75f);
		CHECK(renderer.GetStats().NativeDepthPasses == 1);
		CHECK(renderer.GetStats().EffectOwnedBytes > 0);
		settings.InternalRenderScale = 1;
		renderer.Render(scene, camera, output.Framebuffer);
		CHECK(renderer.GetStats().EffectOwnedBytes == 0);
		CHECK(renderer.GetStats().NativeDepthPasses == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("SceneRenderer: scale transition retires incompatible bloom before deciding the native depth budget")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		Scene scene("ScaleBudget");
		auto entity = scene.CreateEntity("WorldText");
		auto& text = entity.AddComponent<TextComponent>();
		text.Text = "Budget";
		text.ScreenSpace = false;
		auto output = CreateOutput(graphics.GetDevice(), 129, 97);
		auto camera = CameraView::LookAt({0, 0, 3}, {0, 0, 0}, glm::radians(60.0f), 129.0f / 97);
		auto configure = [](SceneRenderer& renderer) {
			auto& settings = renderer.GetSettings();
			settings.GIEnabled = false;
			settings.Shadows = false;
			settings.BloomEnabled = true;
			settings.BloomIntensity = 0.2f;
			settings.BloomLevels = 6;
		};
		uint64_t oldBloom = 0, nativeDepth = 0, finalUsage = 0;
		{
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			configure(renderer);
			renderer.Render(scene, camera, output.Framebuffer);
			oldBloom = renderer.GetStats().EffectOwnedBytes;
			renderer.GetSettings().InternalRenderScale = 0.5f;
			renderer.Render(scene, camera, output.Framebuffer);
			finalUsage = renderer.GetStats().EffectOwnedBytes;
			renderer.GetSettings().BloomEnabled = false;
			renderer.Render(scene, camera, output.Framebuffer);
			nativeDepth = renderer.GetStats().EffectOwnedBytes;
		}
		const uint64_t capacity = std::max(oldBloom, finalUsage);
		REQUIRE(oldBloom + nativeDepth > capacity);
		SceneRenderer bounded(graphics, assets, nullptr, nullptr, nullptr, capacity);
		REQUIRE(bounded.Initialize());
		configure(bounded);
		bounded.Render(scene, camera, output.Framebuffer);
		REQUIRE(bounded.GetStats().BloomActive);
		bounded.GetSettings().InternalRenderScale = 0.5f;
		bounded.Render(scene, camera, output.Framebuffer);
		CHECK(bounded.GetStats().EffectiveRenderScale == 0.5f);
		CHECK(bounded.GetStats().ScaleFallbackReason.empty());
		CHECK(bounded.GetStats().BloomActive);
		CHECK(bounded.GetStats().NativeDepthPasses == 1);
		CHECK(bounded.GetStats().EffectOwnedBytes == finalUsage);
		CHECK(bounded.GetStats().EffectPeakBytes <= capacity);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("RenderScale: nonconstant HDR interpolation and bloom precede nonlinear tonemapping")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = 2;
		desc.height = 1;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		auto hdr = device->createTexture(desc);
		desc.width = 1;
		auto bloom = device->createTexture(desc);
		REQUIRE(hdr);
		REQUIRE(bloom);
		auto output = CreateOutput(device, 5, 1);
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Pixel;
		layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(GPU::TonemapConstants)), nvrhi::BindingLayoutItem::Texture_SRV(0),
							   nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Sampler(1)};
		auto layout = device->createBindingLayout(layoutDesc);
		auto point = device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(false));
		auto linear = device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(true));
		nvrhi::BindingSetDesc bindingDesc;
		bindingDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::TonemapConstants)), nvrhi::BindingSetItem::Texture_SRV(0, hdr),
								nvrhi::BindingSetItem::Texture_SRV(1, bloom), nvrhi::BindingSetItem::Sampler(0, point), nvrhi::BindingSetItem::Sampler(1, linear)};
		auto bindings = device->createBindingSet(bindingDesc, layout);
		nvrhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.VS = ShaderLibrary::CreateShader(device, "Tonemap", "TonemapVS", nvrhi::ShaderType::Vertex);
		pipelineDesc.PS = ShaderLibrary::CreateShader(device, "Tonemap", "TonemapPS", nvrhi::ShaderType::Pixel);
		pipelineDesc.bindingLayouts = {layout};
		pipelineDesc.renderState.rasterState.setCullNone();
		pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
		auto pipeline = device->createGraphicsPipeline(pipelineDesc, output.Framebuffer->getFramebufferInfo());
		REQUIRE(pipeline);
		std::array<uint16_t, 8> values{0, 0, 0, 0, glm::packHalf1x16(4), glm::packHalf1x16(4), glm::packHalf1x16(4), 0};
		const std::array<uint16_t, 4> bloomValues{glm::packHalf1x16(0.25f), glm::packHalf1x16(0.25f), glm::packHalf1x16(0.25f), 0};
		auto commands = device->createCommandList();
		commands->open();
		commands->writeTexture(hdr, 0, 0, values.data(), 16);
		commands->writeTexture(bloom, 0, 0, bloomValues.data(), 8);
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = output.Framebuffer;
		state.bindings = {bindings};
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(5.0f, 1.0f));
		commands->setGraphicsState(state);
		const GPU::TonemapConstants constants{1, 1, 0.5f, 1};
		commands->setPushConstants(&constants, sizeof(constants));
		commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		commands->close();
		device->executeCommandList(commands);
		const auto image = ReadbackTexture(device, output.Texture);
		auto aces = [](float value) { return std::clamp((value * (value + 0.0245786f) - 0.000090537f) / (value * (0.983729f * value + 0.4329510f) + 0.238081f), 0.0f, 1.0f); };
		auto encode = [](float value) { return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f; };
		for (uint32_t x = 0; x < 5; ++x)
		{
			const float fraction = std::clamp((static_cast<float>(x) + 0.5f) * 2 / 5 - 0.5f, 0.0f, 1.0f);
			const float expected = encode(aces(4 * fraction + 0.125f)) * 255;
			// Neutral RGB remains neutral through the ACES matrices. Allow one dither step and quantization.
			CHECK(std::abs(static_cast<float>(image.GetPixel(x, 0).r) - expected) < 2);
		}
		CHECK(std::abs(static_cast<float>(image.GetPixel(1, 0).r) - encode(aces(0.125f)) * 255) > 30);
		CHECK(std::abs(static_cast<float>(image.GetPixel(1, 0).r) - encode(aces(0.125f) * 0.9f + aces(4.125f) * 0.1f) * 255) > 20);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("Bloom: pyramid storage rounds halfway normal and subnormal results to nearest even")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 2;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		auto hdr = device->createTexture(desc);
		REQUIRE(hdr);
		PostEffectBudget budget;
		Bloom bloom(graphics, budget);
		REQUIRE(bloom.Prepare(true, hdr, hdr, 1));
		for (const auto& [low, high, expected] : std::array<std::array<uint16_t, 3>, 5>{{{0x3c00, 0x3c01, 0x3c00}, {0x3c01, 0x3c02, 0x3c02}, {0, 1, 0}, {1, 2, 2}, {0x7bff, 0x7bff, 0x7bff}}})
		{
			std::array<uint16_t, 16> pixels{};
			for (uint32_t pixel = 0; pixel < 4; ++pixel)
				for (uint32_t channel = 0; channel < 3; ++channel)
					pixels[pixel * 4 + channel] = pixel < 2 ? low : high;
			auto commands = device->createCommandList();
			commands->open();
			commands->writeTexture(hdr, 0, 0, pixels.data(), 16);
			bloom.Render(commands, 0, 0);
			commands->close();
			device->executeCommandList(commands);
			CAPTURE(low);
			CAPTURE(high);
			CAPTURE(expected);
			const auto actual = ReadLinearHDR(device, bloom.GetTexture())[0];
			for (int channel = 0; channel < 3; ++channel)
				CHECK(actual[channel] == glm::unpackHalf1x16(expected));
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
