#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/TextureAsset.h>
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
	using Pixels = std::vector<glm::dvec3>;
	using Masks = std::vector<uint8_t>;

	double Decode(double value)
	{
		return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
	}
	double Encode(double value)
	{
		return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - 0.055;
	}

	Pixels Reduce(const Image& image, uint32_t scale)
	{
		REQUIRE(image.GetWidth() == Width * scale);
		REQUIRE(image.GetHeight() == Height * scale);
		Pixels result(Width * Height, glm::dvec3(0));
		for (uint32_t y = 0; y < Height; ++y)
			for (uint32_t x = 0; x < Width; ++x)
				for (uint32_t dy = 0; dy < scale; ++dy)
					for (uint32_t dx = 0; dx < scale; ++dx)
					{
						const auto pixel = image.GetPixel(x * scale + dx, y * scale + dy);
						for (int channel = 0; channel < 3; ++channel)
							result[y * Width + x][channel] += Decode(pixel[channel] / 255.0) / (scale * scale);
					}
		return result;
	}

	nlohmann::json Metrics(const Pixels& actual, const Pixels& reference, const Masks& mask)
	{
		glm::dvec3 linear(0), encoded(0), mean(0), square(0), referenceMean(0), referenceSquare(0);
		uint32_t count = 0;
		for (size_t i = 0; i < actual.size(); ++i)
			if (mask[i])
			{
				++count;
				mean += actual[i];
				square += actual[i] * actual[i];
				referenceMean += reference[i];
				referenceSquare += reference[i] * reference[i];
				for (int c = 0; c < 3; ++c)
				{
					linear[c] += std::pow(actual[i][c] - reference[i][c], 2);
					encoded[c] += std::pow(Encode(actual[i][c]) - Encode(reference[i][c]), 2);
				}
			}
		REQUIRE(count > 0);
		std::array<double, 3> linearMSE{}, encodedMSE{}, variation{}, referenceVariation{}, referenceEnergy{};
		for (int c = 0; c < 3; ++c)
		{
			const auto index = static_cast<size_t>(c);
			linearMSE[index] = linear[c] / count;
			encodedMSE[index] = encoded[c] / count;
			variation[index] = std::sqrt(std::max(0.0, square[c] / count - std::pow(mean[c] / count, 2)));
			referenceEnergy[index] = referenceSquare[c] / count;
			referenceVariation[index] = std::sqrt(std::max(0.0, referenceSquare[c] / count - std::pow(referenceMean[c] / count, 2)));
		}
		return {{"pixels", count}, {"linearMSE", linearMSE}, {"encodedMSE", encodedMSE}, {"linearVariation", variation}, {"referenceLinearVariation", referenceVariation}, {"referenceEnergy", referenceEnergy}};
	}

	struct Target
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;
		Target(nvrhi::IDevice* device, uint32_t scale)
		{
			nvrhi::TextureDesc desc;
			desc.width = Width * scale;
			desc.height = Height * scale;
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

	CameraView Camera(uint32_t pose)
	{
		const float x = pose == 0 ? 0.0f : pose == 1 ? 0.02f
													 : 0.04f;
		return CameraView::LookAt({x, 0, 3}, {x, 0, 0}, glm::radians(60.0f), float(Width) / Height, 0.1f, 10.0f);
	}

	void Configure(SceneRenderer& renderer, AntiAliasingMode mode)
	{
		auto& s = renderer.GetSettings();
		s.Path = RenderPath::Forward;
		s.AntiAliasing = mode;
		s.GIEnabled = s.Shadows = s.SSREnabled = s.SSAOEnabled = s.BloomEnabled = false;
		s.DefaultEnvironmentIntensity = s.AmbientFillIntensity = 0;
		s.Exposure = s.InternalRenderScale = 1;
		s.Tonemapper = TonemapOperator::None;
	}

	std::pair<Masks, Masks> GeometryMasks(const CameraView& camera, const glm::mat4& world)
	{
		const glm::dmat4 inverseWorld = glm::inverse(glm::dmat4(world));
		const glm::dmat4 inverseVP = glm::inverse(glm::dmat4(camera.Projection * camera.View));
		const glm::dvec3 eye = glm::dvec3(glm::inverse(glm::dmat4(camera.View))[3]);
		const glm::dvec3 origin = glm::dvec3(inverseWorld * glm::dvec4(eye, 1));
		auto hit = [&](double x, double y, bool frontOnly) {
			glm::dvec4 point = inverseVP * glm::dvec4(2 * x / Width - 1, 1 - 2 * y / Height, 0.5, 1);
			point /= point.w;
			const auto ray = glm::dvec3(inverseWorld * glm::dvec4(glm::dvec3(point) - eye, 0));
			if (frontOnly)
			{
				if (ray.z >= 0)
					return false;
				const double t = (0.5 - origin.z) / ray.z;
				const auto p = origin + t * ray;
				return t > 0 && std::abs(p.x) < 0.5 && std::abs(p.y) < 0.5;
			}
			double near = 0, far = 1e9;
			for (int axis = 0; axis < 3; ++axis)
			{
				if (std::abs(ray[axis]) < 1e-12)
				{
					if (std::abs(origin[axis]) > 0.5)
						return false;
					continue;
				}
				const double a = (-0.5 - origin[axis]) / ray[axis], b = (0.5 - origin[axis]) / ray[axis];
				near = std::max(near, std::min(a, b));
				far = std::min(far, std::max(a, b));
			}
			return near <= far;
		};
		Masks interior(Width * Height, 0), silhouette(Width * Height, 0);
		for (uint32_t y = 0; y < Height; ++y)
			for (uint32_t x = 0; x < Width; ++x)
			{
				bool allFront = true, anyBox = false, allBox = true;
				// All four corners of a footprint expanded by2native pixels must hit the front face.
				for (double dy : {-2.0, 3.0})
					for (double dx : {-2.0, 3.0})
					{
						allFront &= hit(x + dx, y + dy, true);
						const bool box = hit(x + dx, y + dy, false);
						anyBox |= box;
						allBox &= box;
					}
				interior[y * Width + x] = allFront ? 1 : 0;
				silhouette[y * Width + x] = anyBox && !allBox ? 1 : 0;
			}
		return {interior, silhouette};
	}

	void POMMotion(AntiAliasingMode mode, const char* name)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto errors = graphics.GetValidationErrorCount();
		{
			AssetManager assets;
			Scene scene("PersistentPOM");
			std::array<uint8_t, 16 * 16 * 4> heights{}, colors{};
			for (uint32_t y = 0; y < 16; ++y)
				for (uint32_t x = 0; x < 16; ++x)
				{
					const auto i = (y * 16 + x) * 4;
					const auto height = static_cast<uint8_t>(std::min(x, 15 - x) * 255 / 7);
					heights[i] = heights[i + 1] = heights[i + 2] = height;
					heights[i + 3] = 255;
					const bool checker = ((x / 2 + y / 2) % 2) == 0;
					colors[i] = checker ? 230 : 30;
					colors[i + 1] = checker ? 60 : 180;
					colors[i + 2] = checker ? 30 : 230;
					colors[i + 3] = 255;
				}
			auto height = TextureAsset::CreateRGBA8(16, 16, heights.data(), false);
			height->Handle = AssetHandle(59201);
			assets.AddMemoryAsset(height);
			auto color = TextureAsset::CreateRGBA8(16, 16, colors.data(), true);
			color->Handle = AssetHandle(59202);
			assets.AddMemoryAsset(color);
			auto material = CreateRef<MaterialAsset>();
			material->Handle = AssetHandle(59203);
			material->BaseColorFactor = {0, 0, 0, 1};
			material->EmissiveFactor = {0.8f, 0.8f, 0.8f};
			material->EmissiveTexture = color->Handle;
			material->HeightTexture = height->Handle;
			material->HeightScale = 0.08f;
			assets.AddMemoryAsset(material);
			auto entity = scene.CreateEntity("POMstrip");
			entity.GetTransform().Scale = {1.2f, 1.2f, 0.1f};
			entity.GetTransform().Rotation = glm::angleAxis(glm::radians(50.0f), glm::vec3(0, 1, 0));
			auto& mesh = entity.AddComponent<MeshComponent>();
			mesh.Mesh = AssetHandle(AssetManager::BuiltinCube);
			mesh.Materials = {material->Handle};
			const auto folder = std::filesystem::path("TestOutput") / "FXAAPOMMotion" / name;
			nlohmann::json report = {{"mode", name}, {"reference", "4x/8x supersampled displayed appearance, decoded before exact box reduction; includes quantization/dither and different POM texture footprints, not intersection truth"}, {"textures", {{"size", 16}, {"height", "R=min(x,15-x)*255/7; linear; 1=top"}, {"emissive", "sRGB checker floor(x/2)+floor(y/2) even=(230,60,30), odd=(30,180,230); factor0.8"}, {"sampling", "repeat, generated mip chain"}}}, {"references", nlohmann::json::array()}, {"frames", nlohmann::json::array()}};
			auto save = [&]() { std::ofstream file(folder/"Report.json"); REQUIRE(file); file<<report.dump(2)<<'\n'; CHECK(file.good()); };
			Target native(graphics.GetDevice(), 1), four(graphics.GetDevice(), 4), eight(graphics.GetDevice(), 8);
			std::array<Pixels, 3> references;
			std::array<Masks, 3> interiors, silhouettes;
			Masks full(Width * Height, 1);
			{
				SceneRenderer reference(graphics, assets);
				REQUIRE(reference.Initialize());
				Configure(reference, AntiAliasingMode::Off);
				reference.Render(scene, Camera(0), native.Framebuffer);
				const auto enabled = ReadbackTexture(graphics.GetDevice(), native.Texture);
				REQUIRE(enabled.SavePNG(folder / "Control-POM.png"));
				material->HeightTexture = 0;
				++material->Version;
				reference.Render(scene, Camera(0), native.Framebuffer);
				const auto disabled = ReadbackTexture(graphics.GetDevice(), native.Texture);
				REQUIRE(disabled.SavePNG(folder / "Control-NoPOM.png"));
				material->HeightTexture = height->Handle;
				++material->Version;
				const auto controlMask = GeometryMasks(Camera(0), entity.GetTransform().GetLocalMatrix()).first;
				uint32_t changed = 0, count = 0;
				double magnitude = 0;
				for (uint32_t y = 0; y < Height; ++y)
					for (uint32_t x = 0; x < Width; ++x)
						if (controlMask[y * Width + x])
						{
							++count;
							int maximum = 0;
							const auto a = enabled.GetPixel(x, y), b = disabled.GetPixel(x, y);
							for (int c = 0; c < 3; ++c)
							{
								const int delta = std::abs(int(a[c]) - int(b[c]));
								maximum = std::max(maximum, delta);
								magnitude += delta / 255.0;
							}
							changed += maximum > 8 ? 1u : 0u;
						}
				report["control"] = {{"interiorPixels", count}, {"changedPixelsOver8", changed}, {"meanEncodedAbsoluteDifference", count ? magnitude / (3 * count) : 0}};
				save();
				REQUIRE(count > 0);
				CHECK(changed >= 8);
				CHECK(magnitude / (3 * count) > 0.01);
				for (uint32_t pose = 0; pose < 3; ++pose)
				{
					const auto camera = Camera(pose);
					auto masks = GeometryMasks(camera, entity.GetTransform().GetLocalMatrix());
					interiors[pose] = std::move(masks.first);
					silhouettes[pose] = std::move(masks.second);
					reference.Render(scene, camera, four.Framebuffer);
					const auto low = ReadbackTexture(graphics.GetDevice(), four.Texture);
					reference.Render(scene, camera, eight.Framebuffer);
					const auto high = ReadbackTexture(graphics.GetDevice(), eight.Texture);
					REQUIRE(low.SavePNG(folder / ("Reference4-" + std::to_string(pose) + ".png")));
					REQUIRE(high.SavePNG(folder / ("Reference8-" + std::to_string(pose) + ".png")));
					references[pose] = Reduce(high, 8);
					const auto lowPixels = Reduce(low, 4);
					report["references"].push_back({{"pose", pose}, {"interior", Metrics(lowPixels, references[pose], interiors[pose])}, {"silhouette", Metrics(lowPixels, references[pose], silhouettes[pose])}, {"full", Metrics(lowPixels, references[pose], full)}});
					save();
				}
			}
			SceneRenderer subject(graphics, assets);
			REQUIRE(subject.Initialize());
			Configure(subject, mode);
			const auto materialVersion = material->Version, heightVersion = height->Version, colorVersion = color->Version;
			Image stationary;
			Pixels previous;
			for (uint32_t frame = 0; frame < 8; ++frame)
			{
				const uint32_t pose = frame < 2 ? 0 : frame == 2 ? 1
																 : 2;
				if (frame == 6)
					subject.ResetTemporalHistories("POM fixture explicit reset");
				subject.Render(scene, Camera(pose), native.Framebuffer);
				const auto& stats = subject.GetStats();
				const auto image = ReadbackTexture(graphics.GetDevice(), native.Texture);
				REQUIRE(image.SavePNG(folder / (std::to_string(frame) + ".png")));
				const auto pixels = Reduce(image, 1);
				double residualChange = 0;
				if (frame == 4 || frame == 5)
					for (size_t i = 0; i < pixels.size(); ++i)
						if (interiors[pose][i])
							for (int c = 0; c < 3; ++c)
								residualChange += std::abs((pixels[i][c] - references[pose][i][c]) - (previous[i][c] - references[pose][i][c]));
				const auto interior = Metrics(pixels, references[pose], interiors[pose]);
				report["frames"].push_back({{"frame", frame}, {"pose", pose}, {"effectiveAA", static_cast<int>(stats.EffectiveAA)}, {"sampleIndex", stats.AASampleIndex}, {"completedFrames", stats.AACompletedFrames}, {"resetReason", stats.AAResetReason}, {"interior", interior}, {"silhouette", Metrics(pixels, references[pose], silhouettes[pose])}, {"full", Metrics(pixels, references[pose], full)}, {"fixedPoseResidualChange", (frame == 4 || frame == 5) ? nlohmann::json(residualChange / (3 * interior["pixels"].get<uint32_t>())) : nlohmann::json(nullptr)}});
				save();
				// Conservative nondegeneracy guards declared before the first run: reject blank
				// or constant output without requiring either AA mode to beat the other.
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const double variation = interior["linearVariation"][channel].get<double>();
					const double referenceVariation = interior["referenceLinearVariation"][channel].get<double>();
					CHECK(referenceVariation > 0.005);
					CHECK(variation > std::max(0.005, 0.2 * referenceVariation));
					CHECK(interior["linearMSE"][channel].get<double>() < 0.9 * interior["referenceEnergy"][channel].get<double>());
				}
				CHECK(stats.EffectiveAA == mode);
				CHECK(stats.AAHistoryValid == (mode == AntiAliasingMode::Temporal));
				CHECK(stats.AACompletedFrames == (mode == AntiAliasingMode::Temporal ? frame + 1 : 0));
				CHECK(stats.AASampleIndex == (mode == AntiAliasingMode::Temporal ? (frame < 6 ? frame : frame - 6) : 0));
				if (frame == 6 && mode == AntiAliasingMode::Temporal)
					CHECK(stats.AAResetReason == "POM fixture explicit reset");
				if (frame == 3)
					stationary = image;
				if ((frame == 4 || frame == 5) && mode == AntiAliasingMode::FXAA)
					CHECK(Image::MaxChannelDifference(stationary, image) == 0);
				CHECK(material->Version == materialVersion);
				CHECK(height->Version == heightVersion);
				CHECK(color->Version == colorVersion);
				previous = pixels;
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}
}

TEST_CASE("FXAA POM motion: FXAA retains stable textured appearance along a persistent camera trajectory")
{
	POMMotion(AntiAliasingMode::FXAA, "FXAA");
}
TEST_CASE("FXAA POM motion: Temporal records evolving textured history and an explicit reset")
{
	POMMotion(AntiAliasingMode::Temporal, "Temporal");
}
