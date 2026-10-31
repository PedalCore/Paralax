#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <array>
#include <limits>

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
		const Image expected = Image::LoadFromFile(std::filesystem::path(__FILE__).parent_path() / "Golden" / "R1-Reference.png");
		REQUIRE(expected.GetWidth() == actual.GetWidth());
		REQUIRE(expected.GetHeight() == actual.GetHeight());
		// Permit sparse raster-edge differences across GPU drivers, but not a broad lighting or color shift.
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
