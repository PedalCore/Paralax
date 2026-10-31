#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

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
		ground.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinPlane);

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
