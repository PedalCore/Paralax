#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

using namespace Paralax;

namespace {

	constexpr uint32_t Width = 65, Height = 49;
	constexpr std::array<double, 4> BarCenters{-0.32, -0.08, 0.16, 0.4};

	double Overlap(double a0, double a1, double b0, double b1)
	{
		return std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
	}

	// Exact world-space rectangle/pixel intersections: independent of triangle rasterization,
	// AA shaders, jitter, image history and filter parameters. Front-facing bars do not overlap.
	double Coverage(uint32_t x, uint32_t y, double cameraX, double occluderOffset)
	{
		const double x0 = -1 + 2.0 * x / Width + cameraX, x1 = x0 + 2.0 / Width;
		const double y1 = 1 - 2.0 * y / Height, y0 = y1 - 2.0 / Height;
		double area = 0;
		for (double center : BarCenters)
		{
			const double bx0 = center - 0.02, bx1 = center + 0.02;
			area += Overlap(x0, x1, bx0, bx1) * Overlap(y0, y1, -0.7, 0.7);
			area -= Overlap(x0, x1, std::max(bx0, -0.4 + occluderOffset), std::min(bx1, occluderOffset)) * Overlap(y0, y1, -0.55, 0.55);
		}
		return std::clamp(area * Width * Height / 4.0, 0.0, 1.0);
	}

	double Decode(double value)
	{
		return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
	}
	double Encode(double value)
	{
		return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - 0.055;
	}

	void PersistentMotion(AntiAliasingMode mode, const char* name)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			auto white = CreateRef<MaterialAsset>();
			white->Handle = AssetHandle(59101);
			white->BaseColorFactor = {0, 0, 0, 1};
			white->EmissiveFactor = {1, 1, 1};
			assets.AddMemoryAsset(white);
			auto black = CreateRef<MaterialAsset>();
			black->Handle = AssetHandle(59102);
			black->BaseColorFactor = {0, 0, 0, 1};
			assets.AddMemoryAsset(black);
			Scene scene("PersistentAABars");
			auto cube = [&](const char* label, glm::vec3 position, glm::vec3 size, AssetHandle material) {
				auto entity = scene.CreateEntity(label);
				entity.GetTransform().Translation = position;
				entity.GetTransform().Scale = size;
				auto& mesh = entity.AddComponent<MeshComponent>();
				mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
				mesh.Materials = {material};
				return entity;
			};
			for (double x : BarCenters)
				cube("Bar", {static_cast<float>(x), 0, 0}, {0.04f, 1.4f, 0.02f}, white->Handle);
			auto occluder = cube("Occluder", {-0.2f, 0, 0.3f}, {0.4f, 1.1f, 0.1f}, black->Handle);
			SceneRenderer renderer(graphics, assets);
			REQUIRE(renderer.Initialize());
			auto& settings = renderer.GetSettings();
			settings.AntiAliasing = mode;
			settings.Shadows = settings.GIEnabled = false;
			settings.DefaultEnvironmentIntensity = 0;
			settings.Tonemapper = TonemapOperator::None;
			nvrhi::TextureDesc desc;
			desc.width = Width;
			desc.height = Height;
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			auto output = graphics.GetDevice()->createTexture(desc);
			REQUIRE(output);
			auto framebuffer = graphics.GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output));
			REQUIRE(framebuffer);
			nlohmann::json report = {{"mode", name}, {"reference", "Exact unjittered world-space rectangle coverage; no texture or POM claim"}, {"frames", nlohmann::json::array()}};
			const std::filesystem::path folder = std::filesystem::path("TestOutput") / "FXAAMotion" / name;
			for (uint32_t frame = 0; frame < 8; ++frame)
			{
				const double cameraX = frame < 2 ? 0 : frame == 2 ? 0.02
																  : 0.04;
				const double occluderOffset = frame < 4 ? 0 : frame == 4 ? 0.1
																		 : 0.2;
				occluder.GetTransform().Translation.x = static_cast<float>(-0.2 + occluderOffset);
				auto camera = CameraView::LookAt({static_cast<float>(cameraX), 0, 3}, {static_cast<float>(cameraX), 0, 0}, glm::radians(60.0f), float(Width) / Height);
				camera.Orthographic = true;
				camera.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 10.0f, 0.1f);
				if (frame == 6)
					renderer.ResetTemporalHistories("motion fixture explicit reset");
				renderer.Render(scene, camera, framebuffer);
				const auto& stats = renderer.GetStats();
				CHECK(stats.EffectiveAA == mode);
				CHECK(stats.AAHistoryValid == (mode == AntiAliasingMode::Temporal));
				CHECK(stats.AACompletedFrames == (mode == AntiAliasingMode::Temporal ? frame + 1 : 0));
				CHECK(stats.AASampleIndex == (mode == AntiAliasingMode::Temporal ? (frame < 6 ? frame : frame - 6) : 0));
				CHECK(renderer.GetDefaultTemporalHistory().GetState().GetSampleIndex() == (mode == AntiAliasingMode::Temporal ? (frame < 6 ? frame + 1 : frame - 5) : 0));
				uint32_t reactive = 0;
				for (const auto& draw : renderer.GetDefaultTemporalHistory().GetState().GetDrawRecords())
					reactive += draw.Reactive ? 1u : 0u;
				if (mode == AntiAliasingMode::Temporal)
					CHECK(reactive == ((frame == 0 || frame == 6) ? 5 : 0));
				const auto image = ReadbackTexture(graphics.GetDevice(), output);
				REQUIRE(image.SavePNG(folder / (std::to_string(frame) + ".png")));
				double linearMSE = 0, encodedMSE = 0, revealedError = 0, linePeak = 0, gapSum = 0, edgeLinear = 0, edgeEncoded = 0;
				uint32_t revealed = 0, gapCount = 0, edgeCount = 0;
				for (uint32_t y = 0; y < Height; ++y)
					for (uint32_t x = 0; x < Width; ++x)
					{
						const double reference = Coverage(x, y, cameraX, occluderOffset);
						const double encoded = image.GetPixel(x, y).r / 255.0;
						const double linear = Decode(encoded);
						linearMSE += std::pow(linear - reference, 2);
						encodedMSE += std::pow(encoded - Encode(reference), 2);
						const double worldX = -1 + 2.0 * (x + 0.5) / Width + cameraX;
						if (std::abs(worldX - 0.4) < 0.04 && y > 15 && y < 34)
							linePeak = std::max(linePeak, linear);
						if (std::abs(worldX - 0.47) < 0.02 && y > 15 && y < 34)
						{
							gapSum += linear;
							++gapCount;
						}
						bool edge = reference > 1e-8 && reference < 1 - 1e-8;
						if (x > 0 && x + 1 < Width && y > 0 && y + 1 < Height)
							edge |= std::abs(Coverage(x - 1, y, cameraX, occluderOffset) - Coverage(x + 1, y, cameraX, occluderOffset)) > 1e-8 ||
									std::abs(Coverage(x, y - 1, cameraX, occluderOffset) - Coverage(x, y + 1, cameraX, occluderOffset)) > 1e-8;
						if (edge)
						{
							edgeLinear += std::pow(linear - reference, 2);
							edgeEncoded += std::pow(encoded - Encode(reference), 2);
							++edgeCount;
						}
						if (frame == 4 && y > 15 && y < 34 && reference > 0.5 && Coverage(x, y, cameraX, 0) == 0)
						{
							CHECK(reference == doctest::Approx(0.85).epsilon(1e-10));
							revealedError += std::abs(linear - reference);
							++revealed;
						}
					}
				linearMSE /= Width * Height;
				encodedMSE /= Width * Height;
				const double gap = gapCount ? gapSum / gapCount : 0;
				const double lineContrast = linePeak - gap;
				report["frames"].push_back({{"frame", frame}, {"cameraX", cameraX}, {"occluderOffset", occluderOffset}, {"effectiveMode", static_cast<int>(stats.EffectiveAA)}, {"completedFrames", stats.AACompletedFrames}, {"sampleIndex", stats.AASampleIndex}, {"resetReason", stats.AAResetReason}, {"reactiveDraws", reactive}, {"linearMSE", linearMSE}, {"encodedMSE", encodedMSE}, {"linePeak", linePeak}, {"gapMean", gap}, {"gapPixels", gapCount}, {"lineContrast", lineContrast}, {"edgePixels", edgeCount}, {"edgeLinearMSE", edgeCount ? edgeLinear / edgeCount : 0}, {"edgeEncodedMSE", edgeCount ? edgeEncoded / edgeCount : 0}, {"revealedPixels", revealed}, {"revealedMeanError", revealed ? revealedError / revealed : 0}});
				{
					std::ofstream file(folder / "Report.json");
					REQUIRE(file);
					file << report.dump(2) << '\n';
					CHECK(file.good());
				}
				CAPTURE(frame);
				CAPTURE(linearMSE);
				CAPTURE(encodedMSE);
				CAPTURE(lineContrast);
				CHECK(linearMSE < 0.025);
				CHECK(encodedMSE < 0.025);
				CHECK(lineContrast > 0.35);
				CHECK(edgeCount > 0);
				CHECK(gapCount > 0);
				if (frame == 4)
				{
					REQUIRE(revealed == 18);
					CHECK(revealedError / revealed < 0.55);
				}
				if (frame == 6 && mode == AntiAliasingMode::Temporal)
					CHECK(stats.AAResetReason == "motion fixture explicit reset");
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("FXAA motion: Off reference tracks a persistent camera occluder and explicit reset")
{
	PersistentMotion(AntiAliasingMode::Off, "Off");
}
TEST_CASE("FXAA motion: Spatial tracks a persistent camera occluder and explicit reset")
{
	PersistentMotion(AntiAliasingMode::Spatial, "Spatial");
}
TEST_CASE("FXAA motion: Temporal retains real history through motion and explicitly resets")
{
	PersistentMotion(AntiAliasingMode::Temporal, "Temporal");
}
TEST_CASE("FXAA motion: FXAA tracks a persistent camera occluder without history")
{
	PersistentMotion(AntiAliasingMode::FXAA, "FXAA");
}
