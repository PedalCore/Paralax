#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Paralax;

namespace {

	struct ShadowOutput
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;

		explicit ShadowOutput(nvrhi::IDevice* device, uint32_t size = 128)
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

	Ref<MeshAsset> ReceiverMesh(AssetHandle handle, const glm::vec3& normal)
	{
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = handle;
		for (const glm::vec3 position : {glm::vec3(-1, -1, 0), glm::vec3(1, -1, 0), glm::vec3(1, 1, 0), glm::vec3(-1, 1, 0)})
		{
			MeshVertex vertex;
			vertex.Position = position;
			vertex.Normal = normal;
			vertex.Tangent = glm::vec4(glm::normalize(glm::cross(glm::vec3(0, 1, 0), normal)), 1);
			vertex.UV = glm::vec2(position) * 0.5f + 0.5f;
			mesh->Vertices.push_back(vertex);
		}
		mesh->Indices = {0, 1, 2, 0, 2, 3};
		Submesh submesh;
		submesh.IndexCount = 6;
		submesh.VertexCount = 4;
		mesh->Submeshes.push_back(submesh);
		mesh->Materials = {AssetHandle(50003)};
		mesh->ComputeBounds();
		return mesh;
	}

	int MaskMaximum(const Image& image)
	{
		int maximum = 0;
		for (uint32_t y = 54; y < 74; ++y)
			for (uint32_t x = 54; x < 74; ++x)
				for (int c = 0; c < 3; ++c)
					maximum = std::max(maximum, int(image.GetPixel(x, y)[c]));
		return maximum;
	}

	int MaskMinimum(const Image& image)
	{
		int minimum = 255;
		for (uint32_t y = 54; y < 74; ++y)
			for (uint32_t x = 54; x < 74; ++x)
				for (int c = 0; c < 3; ++c)
					minimum = std::min(minimum, int(image.GetPixel(x, y)[c]));
		return minimum;
	}

	float DecodeSRGB(float value)
	{
		return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
	}

}

TEST_CASE("DirectionalShadow: grazing receivers retain distant blockers without self shadowing")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50003);
		material->BaseColorFactor = glm::vec4(1);
		material->RoughnessFactor = 1;
		material->DoubleSided = true;
		assets.AddMemoryAsset(material);
		assets.AddMemoryAsset(ReceiverMesh(AssetHandle(50001), glm::vec3(0, 0, 1)));
		assets.AddMemoryAsset(ReceiverMesh(AssetHandle(50002), glm::vec3(0.5f, 0, std::sqrt(0.75f))));
		Scene scene("GrazingDirectionalShadow");
		auto receiver = scene.CreateEntity("Receiver");
		auto& receiverMesh = receiver.AddComponent<MeshComponent>();
		receiverMesh.Materials = {material->Handle};
		auto blocker = scene.CreateEntity("Blocker");
		auto& blockerMesh = blocker.AddComponent<MeshComponent>();
		blockerMesh.Mesh = AssetHandle(50001);
		blockerMesh.Materials = {material->Handle};
		blocker.GetTransform().Scale = glm::vec3(3);
		auto sun = scene.CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>().Intensity = 3;
		const auto camera = CameraView::LookAt({0, 0, 4}, {0, 0, 0}, glm::radians(60.0f), 1.0f);
		ShadowOutput output(graphics.GetDevice());
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.DebugView = RenderDebugView::DirectDiffuse;
		settings.ShadowDistance = 30;
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
			for (auto quality : {ShadowQuality::Low, ShadowQuality::Medium})
				for (float cosine : {0.008f, 0.018f})
				{
					CAPTURE(int(path));
					CAPTURE(int(quality));
					CAPTURE(cosine);
					settings.Path = path;
					settings.ShadowResolution = quality;
					const glm::vec3 toLight(std::sqrt(1 - cosine * cosine), 0, cosine);
					sun.GetTransform().Rotation = glm::rotation(glm::vec3(0, 0, 1), toLight);
					blocker.GetTransform().Rotation = sun.GetTransform().Rotation;
					blocker.GetTransform().Translation = 3.0f * toLight;
					// The central mask lies within +/-0.5m. Even the widest 0.5m receiver disk is blocked;
					// the blocker's x>=2.94m keeps it out of the camera's central receiver rays.
					for (uint64_t mesh : {50001u, 50002u})
					{
						CAPTURE(mesh);
						receiverMesh.Mesh = AssetHandle(mesh);
						settings.Shadows = false;
						renderer.Render(scene, camera, output.Framebuffer);
						const Image unshadowed = ReadbackTexture(graphics.GetDevice(), output.Texture);
						REQUIRE(MaskMinimum(unshadowed) > 10);
						REQUIRE(MaskMaximum(unshadowed) < 250);
						settings.Shadows = true;
						for (float softness : {0.0f, 0.06f, 0.5f})
						{
							CAPTURE(softness);
							settings.ShadowSoftness = softness;
							blockerMesh.CastShadows = false;
							renderer.Render(scene, camera, output.Framebuffer);
							REQUIRE(renderer.GetStats().EffectivePath == path);
							const Image clear = ReadbackTexture(graphics.GetDevice(), output.Texture);
							int difference = 0;
							for (uint32_t y = 54; y < 74; ++y)
								for (uint32_t x = 54; x < 74; ++x)
									for (int c = 0; c < 3; ++c)
										difference = std::max(difference, std::abs(int(clear.GetPixel(x, y)[c]) - int(unshadowed.GetPixel(x, y)[c])));
							CHECK(difference <= 3);
							blockerMesh.CastShadows = true;
							renderer.Render(scene, camera, output.Framebuffer);
							const Image blocked = ReadbackTexture(graphics.GetDevice(), output.Texture);
							CHECK(MaskMaximum(blocked) <= 1);
							if (path == RenderPath::Forward && quality == ShadowQuality::Low && cosine == 0.008f && mesh == 50002 && softness == 0.06f)
							{
								REQUIRE(unshadowed.SavePNG(Testing::GetTestOutputDirectory() / "R17-Grazing-Unshadowed.png"));
								REQUIRE(clear.SavePNG(Testing::GetTestOutputDirectory() / "R17-Grazing-Clear.png"));
								REQUIRE(blocked.SavePNG(Testing::GetTestOutputDirectory() / "R17-Grazing-Blocked.png"));
							}
						}
					}
				}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("DirectionalShadow: a finite filter preserves lit and occluded sides of a shadow edge")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50003);
		material->BaseColorFactor = glm::vec4(0.5f, 0.5f, 0.5f, 1);
		material->DoubleSided = true;
		material->RoughnessFactor = 1;
		assets.AddMemoryAsset(material);
		assets.AddMemoryAsset(ReceiverMesh(AssetHandle(50001), glm::vec3(0, 0, 1)));
		Scene scene("DirectionalShadowEdge");
		auto receiver = scene.CreateEntity("Receiver");
		receiver.AddComponent<MeshComponent>().Mesh = AssetHandle(50001);
		auto blocker = scene.CreateEntity("Blocker");
		blocker.AddComponent<MeshComponent>().Mesh = AssetHandle(50001);
		blocker.GetTransform().Scale = {0.5f, 3, 1};
		blocker.GetTransform().Translation = {-0.5f, 0, 0.5f};
		auto sun = scene.CreateEntity("Sun");
		sun.GetTransform().Rotation = glm::rotation(glm::vec3(0, 0, 1), glm::normalize(glm::vec3(0.4f, 0.3f, 1)));
		sun.AddComponent<DirectionalLightComponent>().Intensity = 3;
		// View from the right so the elevated blocker does not cover its shadow edge.
		const auto camera = CameraView::LookAt({1.5f, 0, 2}, {0, 0, 0}, glm::radians(60.0f), 1.0f);
		ShadowOutput output(graphics.GetDevice(), 512);
		SceneRenderer renderer(graphics, assets);
		REQUIRE(renderer.Initialize());
		auto& settings = renderer.GetSettings();
		settings.GIEnabled = false;
		settings.DefaultEnvironmentIntensity = 0;
		settings.DebugView = RenderDebugView::DirectDiffuse;
		settings.ShadowDistance = 30;
		settings.ShadowResolution = ShadowQuality::Low;
		settings.ShadowSoftness = 0.06f;
		for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
		{
			CAPTURE(int(path));
			settings.Path = path;
			renderer.Render(scene, camera, output.Framebuffer);
			REQUIRE(renderer.GetStats().EffectivePath == path);
			const Image shadowed = ReadbackTexture(graphics.GetDevice(), output.Texture);
			REQUIRE(shadowed.SavePNG(Testing::GetTestOutputDirectory() / ("R17-ShadowEdge-" + std::to_string(int(path)) + ".png")));
			for (float x : {-0.4f, 0.4f})
			{
				const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(x, 0, 0, 1);
				const glm::vec2 uv = glm::vec2(clip) / clip.w * glm::vec2(0.5f, -0.5f) + 0.5f;
				const auto pixel = shadowed.GetPixel(static_cast<uint32_t>(uv.x * 512), static_cast<uint32_t>(uv.y * 512));
				if (x < 0)
					CHECK(pixel.r <= 1);
				else
					CHECK(pixel.r > 100);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("LocalShadow: grazing receiver support retains a nearby blocker for point and spot lights")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50003);
		material->BaseColorFactor = glm::vec4(1);
		material->RoughnessFactor = 1;
		material->DoubleSided = true;
		assets.AddMemoryAsset(material);
		assets.AddMemoryAsset(ReceiverMesh(AssetHandle(50001), glm::vec3(0, 0, 1)));
		assets.AddMemoryAsset(ReceiverMesh(AssetHandle(50002), glm::vec3(0.5f, 0, std::sqrt(0.75f))));
		const auto camera = CameraView::LookAt({0, 0, 1}, {0, 0, 0}, glm::radians(60.0f), 1.0f);
		ShadowOutput output(graphics.GetDevice());
		for (bool spot : {false, true})
		{
			CAPTURE(spot);
			Scene scene("LocalGrazingReceiver");
			auto receiver = scene.CreateEntity("Receiver");
			auto& receiverMesh = receiver.AddComponent<MeshComponent>();
			auto blocker = scene.CreateEntity("Blocker");
			auto& blockerMesh = blocker.AddComponent<MeshComponent>();
			blockerMesh.Mesh = AssetHandle(50001);
			blocker.GetTransform().Scale = glm::vec3(3);
			auto lamp = scene.CreateEntity("Lamp");
			if (spot)
			{
				auto& light = lamp.AddComponent<SpotLightComponent>();
				light.Intensity = 100;
				light.Range = 20;
				light.CastShadows = true;
			}
			else
			{
				auto& light = lamp.AddComponent<PointLightComponent>();
				light.Intensity = 100;
				light.Range = 20;
				light.CastShadows = true;
			}
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.GIEnabled = false;
			settings.DefaultEnvironmentIntensity = 0;
			settings.MaxShadowedLocalLights = 1;
			for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
				for (auto quality : {LocalShadowQuality::Low, LocalShadowQuality::Medium})
					for (float cosine : {0.0046f, 0.0126f, -0.00175f})
					{
						CAPTURE(int(path));
						CAPTURE(int(quality));
						CAPTURE(cosine);
						settings.Path = path;
						settings.LocalShadowResolution = quality;
						const glm::vec3 toLight(std::sqrt(1 - cosine * cosine), 0, cosine);
						lamp.GetTransform().Translation = 10.0f * toLight;
						lamp.GetTransform().Rotation = glm::rotation(glm::vec3(0, 0, 1), toLight);
						blocker.GetTransform().Rotation = lamp.GetTransform().Rotation;
						blocker.GetTransform().Translation = 0.5f * toLight;
						// The mask is within +/-0.1m. At <=10.102m distance the Low512 point map's
						// guarded eight-texel cap is <=0.317m, plus <0.040m offset: no tap crosses the 0.5m blocker.
						// Medium and the 30-degree spot have smaller support. The camera sees past the blocker.
						for (uint64_t mesh : {50001u, 50002u})
						{
							CAPTURE(mesh);
							receiverMesh.Mesh = AssetHandle(mesh);
							settings.Shadows = false;
							if (mesh == 50002)
							{
								settings.DebugView = RenderDebugView::DirectDiffuse;
								renderer.Render(scene, camera, output.Framebuffer);
								const auto positive = ReadbackTexture(graphics.GetDevice(), output.Texture);
								REQUIRE(MaskMinimum(positive) > 10);
								REQUIRE(MaskMaximum(positive) < 250);
							}
							settings.DebugView = RenderDebugView::LocalShadow;
							renderer.Render(scene, camera, output.Framebuffer);
							REQUIRE(MaskMinimum(ReadbackTexture(graphics.GetDevice(), output.Texture)) == 255);
							settings.Shadows = true;
							for (float softness : {0.0f, 0.06f, 0.5f})
							{
								CAPTURE(softness);
								settings.LocalShadowSoftness = softness;
								blockerMesh.CastShadows = false;
								renderer.Render(scene, camera, output.Framebuffer);
								REQUIRE(renderer.GetStats().EffectivePath == path);
								const auto clear = ReadbackTexture(graphics.GetDevice(), output.Texture);
								// Negative geometric cosine can legitimately self-occlude despite its smooth normal.
								if (cosine > 0)
									CHECK(MaskMinimum(clear) >= 252);
								blockerMesh.CastShadows = true;
								renderer.Render(scene, camera, output.Framebuffer);
								CHECK(renderer.GetStats().LocalShadowLayers == (spot ? 1u : 6u));
								const auto blocked = ReadbackTexture(graphics.GetDevice(), output.Texture);
								CHECK(MaskMaximum(blocked) <= 1);
								if (path == RenderPath::Forward && quality == LocalShadowQuality::Low && cosine == 0.0046f && mesh == 50002 && softness == 0.06f)
								{
									const std::string name = spot ? "R17-Spot" : "R17-Point";
									REQUIRE(clear.SavePNG(Testing::GetTestOutputDirectory() / (name + "-Grazing-Clear.png")));
									REQUIRE(blocked.SavePNG(Testing::GetTestOutputDirectory() / (name + "-Grazing-Blocked.png")));
								}
							}
						}
					}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("LocalShadow: partial shadow edges remain bounded through cube face ties and corners")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		AssetManager assets;
		auto material = CreateRef<MaterialAsset>();
		material->Handle = AssetHandle(50003);
		material->BaseColorFactor = glm::vec4(1);
		material->DoubleSided = true;
		assets.AddMemoryAsset(material);
		assets.AddMemoryAsset(ReceiverMesh(AssetHandle(50001), glm::vec3(0, 0, 1)));
		const auto camera = CameraView::LookAt({2, 0, 1}, {0, 0, 0}, glm::radians(60.0f), 1.0f);
		struct ReceiverPixel
		{
			uint32_t X, Y;
			glm::vec3 Position;
		};
		std::vector<ReceiverPixel> region;
		const auto inverseVP = glm::inverse(camera.Projection * camera.View);
		for (uint32_t y = 0; y < 512; ++y)
			for (uint32_t x = 0; x < 512; ++x)
			{
				const glm::vec2 ndc = (glm::vec2(x, y) + 0.5f) / 512.0f * glm::vec2(2, -2) + glm::vec2(-1, 1);
				const glm::vec4 nearPoint = inverseVP * glm::vec4(ndc, 1, 1);
				const glm::vec3 ray = glm::vec3(nearPoint) / nearPoint.w - camera.Position;
				if (ray.z >= 0)
					continue;
				const glm::vec3 position = camera.Position - ray * (camera.Position.z / ray.z);
				// The declared plane region encloses the ~0.02-0.03m biased face tie and the 0.06m disk.
				if (position.x >= -0.06f && position.x <= 0.10f && position.y >= -0.06f && position.y <= 0.10f)
					region.push_back({x, y, position});
			}
		REQUIRE(region.size() > 128);
		ShadowOutput output(graphics.GetDevice(), 512);
		for (bool spot : {false, true})
		{
			CAPTURE(spot);
			Scene scene("LocalShadowEdge");
			scene.CreateEntity("Receiver").AddComponent<MeshComponent>().Mesh = AssetHandle(50001);
			auto blocker = scene.CreateEntity("Blocker");
			blocker.AddComponent<MeshComponent>().Mesh = AssetHandle(50001);
			blocker.GetTransform().Scale = {0.5f, 3, 1};
			blocker.GetTransform().Translation = {0, 0, 0.5f};
			auto lamp = scene.CreateEntity("Lamp");
			if (spot)
			{
				auto& light = lamp.AddComponent<SpotLightComponent>();
				light.Range = 20;
				light.CastShadows = true;
			}
			else
			{
				auto& light = lamp.AddComponent<PointLightComponent>();
				light.Range = 20;
				light.CastShadows = true;
			}
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.GIEnabled = false;
			settings.DebugView = RenderDebugView::LocalShadow;
			settings.LocalShadowResolution = LocalShadowQuality::Low;
			settings.LocalShadowSoftness = 0.06f;
			settings.MaxShadowedLocalLights = 1;
			for (auto path : {RenderPath::Forward, RenderPath::Deferred, RenderPath::Visibility})
				for (bool corner : {false, true})
				{
					std::vector<float> previousVisibility;
					for (int step : {-1, 0, 1})
					{
						if (spot && (corner || step != 0))
							continue;
						CAPTURE(int(path));
						CAPTURE(corner);
						CAPTURE(step);
						settings.Path = path;
						lamp.GetTransform().Translation = {4 + static_cast<float>(step) * 0.01f, corner ? 4.0f : 0.0f, 4};
						lamp.GetTransform().Rotation = glm::rotation(glm::vec3(0, 0, 1), glm::normalize(lamp.GetTransform().Translation));
						renderer.Render(scene, camera, output.Framebuffer);
						REQUIRE(renderer.GetStats().EffectivePath == path);
						CHECK(renderer.GetStats().LocalShadowLayers == (spot ? 1u : 6u));
						const auto image = ReadbackTexture(graphics.GetDevice(), output.Texture);
						std::vector<float> visibility;
						uint32_t faces = 0;
						size_t partial = 0;
						float totalChange = 0;
						for (const auto& pixel : region)
						{
							const float value = DecodeSRGB(static_cast<float>(image.GetPixel(pixel.X, pixel.Y).r) / 255.0f);
							partial += value > 0.01f && value < 0.99f ? 1u : 0u;
							if (!previousVisibility.empty())
								totalChange += std::abs(value - previousVisibility[visibility.size()]);
							visibility.push_back(value);
							const float cubeTangent = 1.0f + 2.0f / 512.0f;
							const float texelWorld = glm::distance(pixel.Position, lamp.GetTransform().Translation) * 2.0f * cubeTangent / 512.0f;
							const glm::vec3 biasedDirection = glm::abs(pixel.Position + glm::vec3(0, 0, texelWorld) - lamp.GetTransform().Translation);
							if (biasedDirection.x >= biasedDirection.y && biasedDirection.x >= biasedDirection.z)
								faces |= 1;
							else if (biasedDirection.y >= biasedDirection.z)
								faces |= 2;
							else
								faces |= 4;
						}
						CHECK(partial * 2 >= region.size());
						if (!spot)
							CHECK(faces == (corner ? 7u : 5u));
						if (!previousVisibility.empty())
						{
							// Budget one of sixteen equal PCF samples in the mean, plus the worst two-frame
							// sRGB8 rounding envelope. Individual pixel steps can be larger than this mean.
							const float quantization = 2.0f * (1.0f - DecodeSRGB(254.5f / 255.0f));
							CHECK(totalChange / static_cast<float>(region.size()) <= 1.0f / 16.0f + quantization);
						}
						previousVisibility = std::move(visibility);
						for (float x : {-0.3f, 0.3f})
						{
							const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(x, 0, 0, 1);
							const glm::vec2 uv = glm::vec2(clip) / clip.w * glm::vec2(0.5f, -0.5f) + 0.5f;
							const auto pixel = image.GetPixel(static_cast<uint32_t>(uv.x * 512), static_cast<uint32_t>(uv.y * 512));
							if (x < 0)
								CHECK(pixel.r <= 1);
							else
								CHECK(pixel.r >= 254);
						}
						if (path == RenderPath::Forward)
						{
							const std::string name = spot ? "R17-Spot-Edge" : std::string("R17-Point-") + (corner ? "Corner-" : "Tie-") + std::to_string(step);
							REQUIRE(image.SavePNG(Testing::GetTestOutputDirectory() / (name + ".png")));
						}
					}
				}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
