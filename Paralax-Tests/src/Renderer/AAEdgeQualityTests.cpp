#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Renderer/ShaderLibrary.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace Paralax;

namespace {

	// Float fixtures isolate the shader's acceptance rules from geometry rasterization and FP16 storage.
	template<uint32_t Extent>
	struct BasicEdgeFixture
	{
		static constexpr uint32_t Size = Extent;
		struct Constants
		{
			glm::mat4 Inverse{1};
			glm::vec4 JitterHistory{0, 0, 0.9f, 1};
			glm::uvec4 Dimensions{Size, Size, 0, 0};
		} Push;
		nvrhi::IDevice* Device;
		std::array<nvrhi::TextureHandle, 11> Inputs;
		std::array<nvrhi::TextureHandle, 4> Outputs;
		nvrhi::BindingLayoutHandle Layout;
		nvrhi::BindingSetHandle Bindings;
		nvrhi::FramebufferHandle Framebuffer;
		nvrhi::GraphicsPipelineHandle Pipeline;
		std::vector<glm::vec4> Color, History, Metadata;
		std::vector<glm::vec2> Normal, PreviousUV;
		std::vector<float> Depth, PreviousDepth, Reactive, HistoryDepth;
		std::vector<uint32_t> ID, HistoryID;

		BasicEdgeFixture(nvrhi::IDevice* device, bool temporal)
			: Device(device)
		{
			const std::array formats{nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::R32_FLOAT, nvrhi::Format::RG32_FLOAT,
									 nvrhi::Format::R32_UINT, nvrhi::Format::RG32_FLOAT, nvrhi::Format::R32_FLOAT, nvrhi::Format::R32_FLOAT,
									 nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::R32_UINT, nvrhi::Format::R32_FLOAT};
			for (size_t i = 0; i < formats.size(); ++i)
				Inputs[i] = Texture(formats[i]);
			Outputs = {Texture(nvrhi::Format::RGBA32_FLOAT), Texture(nvrhi::Format::RGBA32_FLOAT), Texture(nvrhi::Format::R32_UINT), Texture(nvrhi::Format::R32_FLOAT)};
			nvrhi::FramebufferDesc framebuffer;
			for (uint32_t i = 0; i < (temporal ? 4u : 1u); ++i)
				framebuffer.addColorAttachment(Outputs[i]);
			Framebuffer = device->createFramebuffer(framebuffer);
			REQUIRE(Framebuffer);
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, temporal ? sizeof(Constants) : sizeof(glm::vec4))};
			for (uint32_t slot = 0; slot < (temporal ? Inputs.size() : 1u); ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			if (!temporal)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(0));
			Layout = device->createBindingLayout(layout);
			REQUIRE(Layout);
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0, temporal ? sizeof(Constants) : sizeof(glm::vec4))};
			for (uint32_t slot = 0; slot < (temporal ? Inputs.size() : 1u); ++slot)
				bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(slot, Inputs[slot]));
			auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
			if (!temporal)
				bindings.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, sampler));
			Bindings = device->createBindingSet(bindings, Layout);
			REQUIRE(Bindings);
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = ShaderLibrary::CreateShader(device, temporal ? "TemporalResolve" : "SpatialAA", temporal ? "ResolveVS" : "SpatialVS", nvrhi::ShaderType::Vertex);
			pipeline.PS = ShaderLibrary::CreateShader(device, temporal ? "TemporalResolve" : "SpatialAA", temporal ? "ResolvePS" : "SpatialPS", nvrhi::ShaderType::Pixel);
			pipeline.bindingLayouts = {Layout};
			pipeline.renderState.rasterState.setCullNone();
			pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			Pipeline = device->createGraphicsPipeline(pipeline, Framebuffer->getFramebufferInfo());
			REQUIRE(Pipeline);
			Reset();
		}

		nvrhi::TextureHandle Texture(nvrhi::Format format)
		{
			nvrhi::TextureDesc desc;
			desc.width = desc.height = Size;
			desc.format = format;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			auto texture = Device->createTexture(desc);
			REQUIRE(texture);
			return texture;
		}

		void Reset()
		{
			Push.JitterHistory = {0, 0, 0.9f, 0};
			Color.assign(Size * Size, glm::vec4(0));
			History.assign(Size * Size, glm::vec4(100));
			Metadata.assign(Size * Size, glm::vec4(0));
			Normal.assign(Size * Size, glm::vec2(0));
			PreviousUV.assign(Size * Size, glm::vec2(0.5f));
			Depth.assign(Size * Size, 0.5f);
			PreviousDepth.assign(Size * Size, -0.5f);
			Reactive.assign(Size * Size, 0);
			HistoryDepth.assign(Size * Size, -0.5f);
			ID.assign(Size * Size, 1);
			HistoryID.assign(Size * Size, 99);
		}

		std::vector<glm::vec4> Run(bool temporal, uint32_t output = 0)
		{
			auto commands = Device->createCommandList();
			commands->open();
			auto upload = [&](uint32_t slot, const auto& pixels) {
				commands->writeTexture(Inputs[slot], 0, 0, pixels.data(), Size * sizeof(pixels[0]));
			};
			upload(0, Color);
			upload(1, Depth);
			upload(2, Normal);
			upload(3, ID);
			upload(4, PreviousUV);
			upload(5, PreviousDepth);
			upload(6, Reactive);
			upload(7, History);
			upload(8, Metadata);
			upload(9, HistoryID);
			upload(10, HistoryDepth);
			nvrhi::GraphicsState state;
			state.pipeline = Pipeline;
			state.framebuffer = Framebuffer;
			state.bindings = {Bindings};
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(Size), float(Size)));
			commands->setGraphicsState(state);
			if (temporal)
				commands->setPushConstants(&Push, sizeof(Push));
			else
			{
				const glm::vec4 size(Size, Size, 0, 0);
				commands->setPushConstants(&size, sizeof(size));
			}
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
			auto desc = Outputs[output]->getDesc();
			desc.isRenderTarget = false;
			auto staging = Device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			REQUIRE(staging);
			commands->copyTexture(staging, {}, Outputs[output], {});
			commands->close();
			Device->executeCommandList(commands);
			Device->waitForIdle();
			size_t pitch = 0;
			const auto* data = static_cast<const uint8_t*>(Device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
			REQUIRE(data);
			std::vector<glm::vec4> result(Size * Size);
			for (uint32_t y = 0; y < Size; ++y)
				std::memcpy(result.data() + y * Size, data + y * pitch, Size * sizeof(result[0]));
			Device->unmapStagingTexture(staging);
			return result;
		}
	};

	using EdgeFixture = BasicEdgeFixture<640>;

	struct Shape
	{
		int Kind = 0;
		double Slope = 0, Phase = 0;
		bool Inverted = false;
		bool Inside(double x, double y) const
		{
			double distance = (y - Slope * x - Phase) / std::sqrt(1 + Slope * Slope);
			bool inside = distance >= 0;
			if (Kind == 1)
				inside = std::abs(distance) < 1.25;
			if (Kind == 2)
				inside = (x - Phase) * (x - Phase) + y * y < 10.3 * 10.3;
			if (Kind == 3)
				inside = std::fmod(std::abs(distance + 100), 6.0) < 1.25;
			return inside != Inverted;
		}
	};

	std::vector<Shape> Shapes()
	{
		std::vector<Shape> result;
		for (double slope : {0.015625, 0.03125, 0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0})
			for (double sign : {-1.0, 1.0})
				for (double phase : {0.125, 0.5, 0.875})
					for (bool inverted : {false, true})
						result.push_back({0, slope * sign, phase, inverted});
		for (int kind : {1, 2, 3})
			for (double slope : {0.125, 0.5})
				for (bool inverted : {false, true})
					result.push_back({kind, slope, 0.375, inverted});
		return result;
	}

	float Bilinear(const std::vector<glm::vec4>& color, double x, double y, int size = 640)
	{
		const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
		const float fx = static_cast<float>(x - ix), fy = static_cast<float>(y - iy);
		auto load = [&](int px, int py) { return color[static_cast<size_t>(std::clamp(py, 0, size - 1)) * size + std::clamp(px, 0, size - 1)].x; };
		return std::lerp(std::lerp(load(ix, iy), load(ix + 1, iy), fx), std::lerp(load(ix, iy + 1), load(ix + 1, iy + 1), fx), fy);
	}

	// Frozen R12 cardinal filter is a comparison baseline, not the quality oracle.
	float OldSpatial(const std::vector<glm::vec4>& color, int x, int y)
	{
		auto sample = [&](double dx, double dy) { return Bilinear(color, x + dx, y + dy); };
		const float center = sample(0, 0), m = std::sqrt(center);
		const float n = std::sqrt(sample(0, -1)), s = std::sqrt(sample(0, 1));
		const float e = std::sqrt(sample(1, 0)), w = std::sqrt(sample(-1, 0));
		const float high = std::max({m, n, s, e, w}), low = std::min({m, n, s, e, w});
		const float contrast = high - low;
		if (contrast < std::max(0.05f, high * 0.125f))
			return center;
		const bool vertical = std::abs(n + s - 2 * m) >= std::abs(e + w - 2 * m);
		const float filtered = (sample(vertical ? 0 : -0.5, vertical ? -0.5 : 0) + sample(vertical ? 0 : 0.5, vertical ? 0.5 : 0)) * 0.5f;
		return std::lerp(center, filtered, std::min(std::abs((n + s + e + w) * 0.25f - m) / contrast * 2, 0.75f));
	}
}

TEST_CASE("AAEdgeQuality: directional reconstruction improves slopes strips curves and repeating tile coverage")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		const auto shapes = Shapes();
		std::vector<double> reference(shapes.size() * 32 * 32);
		// Independent 32x32 area integration; no filter weights or endpoint logic enter the oracle.
		for (size_t i = 0; i < shapes.size(); ++i)
			for (int y = 80; y < 112; ++y)
				for (int x = 80; x < 112; ++x)
				{
					double sum = 0;
					for (int sy = 0; sy < 32; ++sy)
						for (int sx = 0; sx < 32; ++sx)
							sum += shapes[i].Inside(x - 96 + (sx + 0.5) / 32, y - 96 + (sy + 0.5) / 32) ? 1.0 : 0.0;
					reference[i * 1024 + (y - 80) * 32 + x - 80] = sum / 1024;
				}
		for (bool temporal : {false, true})
		{
			EdgeFixture fixture(graphics.GetDevice(), temporal);
			const std::array jitters{glm::vec2(0), glm::vec2(-0.25f, 0.25f), glm::vec2(0.25f, -0.375f)};
			for (size_t frame = 0; frame < (temporal ? jitters.size() + 2 : 1); ++frame)
			{
				const auto jitter = jitters[frame % jitters.size()];
				const float scale = frame == 3 ? 1.0f / 256 : frame == 4 ? 64.0f
																		 : 1.0f;
				fixture.Push.JitterHistory.x = jitter.x;
				fixture.Push.JitterHistory.y = jitter.y;
				// Mismatched bright history must remain rejected, including every reconstructed silhouette.
				fixture.Push.JitterHistory.w = 1;
				std::array<double, 4> off{}, old{}, current{};
				std::vector<double> caseOff(shapes.size()), caseOld(shapes.size()), caseCurrent(shapes.size());
				// Nine isolated 192px cells keep the 64-step search inside each case.
				// The 32px measurement window has 80px of guard on either side.
				for (size_t batch = 0; batch < shapes.size(); batch += 9)
				{
					std::fill(fixture.Color.begin(), fixture.Color.end(), glm::vec4(0));
					std::fill(fixture.ID.begin(), fixture.ID.end(), 2);
					for (size_t i = batch; i < std::min(batch + 9, shapes.size()); ++i)
						for (int y = 0; y < 192; ++y)
							for (int x = 0; x < 192; ++x)
							{
								const size_t index = ((i - batch) / 3 * 192 + y) * 640 + (i - batch) % 3 * 192 + x;
								const bool inside = shapes[i].Inside(x - 95.5 - double(jitter.x), y - 95.5 - double(jitter.y));
								fixture.Color[index] = glm::vec4(inside ? scale : 0);
								fixture.ID[index] = inside ? 1 : 2;
							}
					const auto result = fixture.Run(temporal);
					for (size_t i = batch; i < std::min(batch + 9, shapes.size()); ++i)
						for (int y = 80; y < 112; ++y)
							for (int x = 80; x < 112; ++x)
							{
								const int px = static_cast<int>((i - batch) % 3 * 192) + x, py = static_cast<int>((i - batch) / 3 * 192) + y;
								const size_t index = static_cast<size_t>(py) * 640 + px;
								const double target = reference[i * 1024 + (y - 80) * 32 + x - 80];
								const double point = shapes[i].Inside(x - 95.5, y - 95.5) ? 1 : 0;
								const double baseline = double(temporal ? Bilinear(fixture.Color, px + double(jitter.x), py + double(jitter.y)) : OldSpatial(fixture.Color, px, py)) / double(scale);
								const int kind = shapes[i].Kind;
								off[kind] += (point - target) * (point - target);
								old[kind] += (baseline - target) * (baseline - target);
								current[kind] += (double(result[index].x / scale) - target) * (double(result[index].x / scale) - target);
								caseOff[i] += (point - target) * (point - target);
								caseOld[i] += (baseline - target) * (baseline - target);
								caseCurrent[i] += (double(result[index].x / scale) - target) * (double(result[index].x / scale) - target);
							}
				}
				// Aggregate gains must not conceal a poor slope/phase. Report individual
				// regressions versus R12. Temporal uses a different jittered sampling lattice,
				// so individual phases need not beat a fortuitously aligned unjittered point.
				for (size_t i = 0; i < shapes.size(); ++i)
				{
					INFO("temporal=", temporal, " frame=", frame, " kind=", shapes[i].Kind, " slope=", shapes[i].Slope,
						 " phase=", shapes[i].Phase, " inverse=", shapes[i].Inverted, " Off=", caseOff[i], " R12=", caseOld[i], " new=", caseCurrent[i]);
					if (caseCurrent[i] > caseOld[i])
						std::cout << "AA case regression temporal=" << temporal << " frame=" << frame << " kind=" << shapes[i].Kind << " slope=" << shapes[i].Slope
								  << " phase=" << shapes[i].Phase << " inverse=" << shapes[i].Inverted << " Off=" << caseOff[i] << " R12=" << caseOld[i] << " new=" << caseCurrent[i] << " new/R12=" << caseCurrent[i] / caseOld[i] << '\n';
				}
				// These are material quality targets (20% vs point, 15% vs prior filter),
				// not floating-point tolerances. The 32x32 area oracle and output dither
				// are much finer than the stair-step corrections being assessed.
				for (size_t kind = 0; kind < 4; ++kind)
				{
					INFO("temporal=", temporal, " frame=", frame, " kind=", kind, " Off SSE=", off[kind], " R12=", old[kind], " new=", current[kind]);
					std::cout << "AA quality temporal=" << temporal << " frame=" << frame << " kind=" << kind << " Off=" << off[kind] << " R12=" << old[kind] << " new=" << current[kind] << '\n';
					CHECK(current[kind] < off[kind] * 0.8);
					CHECK(current[kind] < old[kind] * 0.85);
				}
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AAEdgeQuality: flat low contrast and resolved detail survive while isolated HDR samples stay bounded")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		for (bool temporal : {false, true})
		{
			EdgeFixture fixture(graphics.GetDevice(), temporal);
			fixture.Push.JitterHistory.w = 0;
			for (uint32_t y = 0; y < EdgeFixture::Size; ++y)
				for (uint32_t x = 0; x < EdgeFixture::Size; ++x)
				{
					const size_t index = static_cast<size_t>(y) * EdgeFixture::Size + x;
					const uint32_t region = x / 128;
					float value = 0.4f;
					if (region == 1)
						value = 0.5f + (((x + y) & 1u) ? 0.025f : -0.025f);
					if (region == 2)
						value = ((x / 4) & 1u) ? 0.8f : 0.2f;
					if (region == 3)
						value = ((x + y) & 1u) ? 0.8f : 0.2f;
					if (region == 4)
						value = 0;
					fixture.Color[index] = glm::vec4(value);
					fixture.ID[index] = value > 0.5f ? 1 : 2;
				}
			const size_t bright = 320 * EdgeFixture::Size + 576;
			fixture.Color[bright] = glm::vec4(temporal ? 64 : 1);
			fixture.Color[bright + 8] = glm::vec4(std::numeric_limits<float>::quiet_NaN());
			fixture.Reactive[bright] = 1;
			const auto result = fixture.Run(temporal);
			for (uint32_t region = 0; region < 4; ++region)
			{
				double squaredError = 0, inputContrast = 0, outputContrast = 0;
				for (uint32_t y = 32; y < 608; ++y)
					for (uint32_t x = region * 128 + 16; x < region * 128 + 112; ++x)
					{
						const size_t index = static_cast<size_t>(y) * EdgeFixture::Size + x;
						const double input = double(fixture.Color[index].x), output = double(result[index].x);
						squaredError += (output - input) * (output - input);
						inputContrast += std::abs(input - 0.5);
						outputContrast += std::abs(output - 0.5);
					}
				INFO("temporal=", temporal, " detail region=", region, " contrast retained=", outputContrast / inputContrast);
				// Flat and low-contrast detail should change only by the native Spatial output dither.
				if (region <= 1)
					CHECK(squaredError / (576 * 96) < 0.000004);
				else
					CHECK(outputContrast > inputContrast * 0.9);
			}
			for (uint32_t y = 300; y < 340; ++y)
				for (uint32_t x = 550; x < 600; ++x)
				{
					const auto value = result[static_cast<size_t>(y) * EdgeFixture::Size + x];
					REQUIRE(std::isfinite(value.x));
					CHECK(value.x >= -0.00001f);
					CHECK(value.x <= (temporal ? 64.0f : 1.01f));
					if (std::abs(int(x) - 576) > 2 || std::abs(int(y) - 320) > 2)
						CHECK(std::abs(value.x) < 0.00001f);
				}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AAEdgeQuality: aligned fractional edges corners and border footprints preserve analytic area on odd targets")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		for (bool temporal : {false, true})
		{
			BasicEdgeFixture<17> fixture(graphics.GetDevice(), temporal);
			for (int shape = 0; shape < 3; ++shape)
				for (bool inverted : {false, true})
					for (float phase : {-0.375f, 0.0f, 0.375f})
					{
						fixture.Push.JitterHistory = {temporal ? phase : 0, temporal ? -phase : 0, 0.9f, 1};
						for (int y = 0; y < 17; ++y)
							for (int x = 0; x < 17; ++x)
							{
								const bool inside = (shape == 1 || x <= 8) && (shape == 0 || y <= 8);
								const size_t index = static_cast<size_t>(y) * 17 + x;
								fixture.Color[index] = glm::vec4(inside != inverted ? 1 : 0);
								fixture.ID[index] = inside ? 1 : 2;
							}
						const auto result = fixture.Run(temporal);
						for (int y = 0; y < 17; ++y)
							for (int x = 0; x < 17; ++x)
							{
								const float cx = shape == 1 ? 1 : std::clamp(9.0f - float(x) - fixture.Push.JitterHistory.x, 0.0f, 1.0f);
								const float cy = shape == 0 ? 1 : std::clamp(9.0f - float(y) - fixture.Push.JitterHistory.y, 0.0f, 1.0f);
								const float reference = inverted ? 1 - cx * cy : cx * cy;
								INFO("mode=", temporal, " shape=", shape, " inverse=", inverted, " phase=", phase, " x=", x, " y=", y);
								CHECK(std::abs(result[static_cast<size_t>(y) * 17 + x].x - reference) < (temporal ? 0.00001f : 0.005f));
							}
					}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AAEdgeQuality: fine checker coverage is phase symmetric and does not reverse contrast")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		BasicEdgeFixture<17> fixture(graphics.GetDevice(), true);
		fixture.Push.JitterHistory.w = 1;
		for (int y = 0; y < 17; ++y)
			for (int x = 0; x < 17; ++x)
			{
				const size_t index = static_cast<size_t>(y) * 17 + x;
				const bool white = ((x + y) & 1) != 0;
				fixture.Color[index] = glm::vec4(white ? 1 : 0);
				fixture.ID[index] = white ? 1 : 2;
			}
		for (float phase : {-0.25f, 0.25f})
		{
			fixture.Push.JitterHistory.x = phase;
			fixture.Push.JitterHistory.y = phase;
			const auto result = fixture.Run(true);
			for (int y = 0; y < 17; ++y)
				for (int x = 0; x < 17; ++x)
				{
					// Integrating four aligned checker squares over the shifted pixel is bilinear.
					const float reference = Bilinear(fixture.Color, x + double(phase), y + double(phase), 17);
					CHECK(result[static_cast<size_t>(y) * 17 + x].x == doctest::Approx(double(reference)).epsilon(1e-5));
				}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AAEdgeQuality: isolated bright and dark texels and short blocks retain shifted area and energy")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		for (bool temporal : {false, true})
		{
			BasicEdgeFixture<17> fixture(graphics.GetDevice(), temporal);
			for (glm::ivec2 extent : {glm::ivec2(1), glm::ivec2(2), glm::ivec2(2, 3)})
				for (bool inverted : {false, true})
					for (float phase : {-0.25f, 0.0f, 0.25f})
					{
						fixture.Color.assign(17 * 17, glm::vec4(inverted ? 1 : 0));
						for (int y = 8; y < 8 + extent.y; ++y)
							for (int x = 8; x < 8 + extent.x; ++x)
								fixture.Color[y * 17 + x] = glm::vec4(inverted ? 0 : 1);
						fixture.ID.assign(17 * 17, 1);
						for (int y = 8; y < 8 + extent.y; ++y)
							for (int x = 8; x < 8 + extent.x; ++x)
								fixture.ID[y * 17 + x] = 2;
						fixture.Push.JitterHistory = {temporal ? phase : 0, temporal ? phase : 0, 0.9f, 1};
						const auto result = fixture.Run(temporal);
						double energy = 0;
						for (int y = 0; y < 17; ++y)
							for (int x = 0; x < 17; ++x)
							{
								const float phaseX = fixture.Push.JitterHistory.x, phaseY = fixture.Push.JitterHistory.y;
								const float left = float(x) + phaseX - 0.5f, top = float(y) + phaseY - 0.5f;
								const float area = std::max(0.0f, std::min(left + 1, 7.5f + float(extent.x)) - std::max(left, 7.5f)) *
												   std::max(0.0f, std::min(top + 1, 7.5f + float(extent.y)) - std::max(top, 7.5f));
								const float expected = inverted ? 1 - area : area;
								const float actual = result[static_cast<size_t>(y) * 17 + x].x;
								CHECK(std::abs(actual - expected) < (temporal ? 0.00001f : 0.005f));
								if (x >= 7 && x <= 8 + extent.x && y >= 7 && y <= 8 + extent.y)
									energy += double(inverted ? 1 - actual : actual);
							}
						CHECK(std::abs(energy - extent.x * extent.y) < (temporal ? 0.00001 : 0.08));
					}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AAEdgeQuality: reconstruction beyond a homogeneous footprint rejects compatible history and stores reactivity")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		BasicEdgeFixture<17> fixture(graphics.GetDevice(), true);
		for (int y = 0; y < 17; ++y)
			for (int x = 0; x < 17; ++x)
			{
				const size_t index = static_cast<size_t>(y) * 17 + x;
				const bool inside = double(y) >= 0.25 * x + 6.25;
				fixture.Color[index] = fixture.History[index] = glm::vec4(inside ? 1 : 0);
				fixture.ID[index] = fixture.HistoryID[index] = inside ? 1 : 2;
				fixture.PreviousUV[index] = {(float(x) + 0.5f) / 17, (float(y) + 0.5f) / 17};
			}
		fixture.Push.JitterHistory.w = 0;
		const auto current = fixture.Run(true);
		fixture.Push.JitterHistory.w = 1;
		const auto withHistory = fixture.Run(true);
		const auto metadata = fixture.Run(true, 1);
		uint32_t checked = 0;
		for (int y = 2; y < 15; ++y)
			for (int x = 2; x < 15; ++x)
			{
				const size_t index = static_cast<size_t>(y) * 17 + x;
				if (std::abs(current[index].x - fixture.Color[index].x) <= 0.01f)
					continue;
				++checked;
				CHECK(withHistory[index].x == current[index].x);
				CHECK(metadata[index].z == 1);
			}
		CHECK(checked >= 8);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("AAEdgeQuality: fractional chromatic edges preserve tangential color instead of inventing a two color model")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		BasicEdgeFixture<17> fixture(graphics.GetDevice(), true);
		// Geometry and tangential shading are independent. Exercise equal-luma
		// chroma, unequal HDR magnitudes, and collinear same-hue variation.
		const float green = 0.2126f / 0.7152f;
		for (glm::vec3 second : {glm::vec3(0, green, 0), glm::vec3(0, 2, 0), glm::vec3(0.5f, 0, 0)})
			for (float scale : {1.0f / 256, 1.0f, 64.0f})
				for (bool transpose : {false, true})
					for (float sign : {-1.0f, 1.0f})
						for (bool reverse : {false, true})
							for (float normalPhase : {-0.25f, 0.0f, 0.25f})
							{
								const glm::vec3 firstColor = reverse ? second : glm::vec3(1, 0, 0);
								const glm::vec3 secondColor = reverse ? glm::vec3(1, 0, 0) : second;
								const glm::vec2 phase = transpose ? glm::vec2(normalPhase, sign * 0.25f) : glm::vec2(sign * 0.25f, normalPhase);
								fixture.Push.JitterHistory = {phase.x, phase.y, 0.9f, 1};
								for (int y = 0; y < 17; ++y)
									for (int x = 0; x < 17; ++x)
									{
										const float u = sign * float((transpose ? y : x) - 8), v = float((transpose ? x : y) - 8);
										const bool covered = v >= u / 4 - 0.375f;
										const size_t index = static_cast<size_t>(y) * 17 + x;
										fixture.Color[index] = glm::vec4(covered ? (u <= 0 ? firstColor : secondColor) * scale : glm::vec3(0), 1);
										fixture.ID[index] = covered ? 1 : 2;
									}
								const auto result = fixture.Run(true);
								const int x = static_cast<int>(std::floor(8 + phase.x)), y = static_cast<int>(std::floor(8 + phase.y));
								const float fx = 8 + phase.x - float(x), fy = 8 + phase.y - float(y);
								const auto expected = glm::mix(glm::mix(fixture.Color[y * 17 + x], fixture.Color[y * 17 + x + 1], fx),
															   glm::mix(fixture.Color[(y + 1) * 17 + x], fixture.Color[(y + 1) * 17 + x + 1], fx), fy);
								const glm::vec3 actual = glm::vec3(result[8 * 17 + 8]) / scale;
								INFO("transpose=", transpose, " sign=", sign, " reverse=", reverse, " normalPhase=", normalPhase, " scale=", scale);
								for (int channel = 0; channel < 3; ++channel)
									CHECK(std::abs(actual[channel] - expected[channel] / scale) < 1e-5f);
								if (normalPhase == 0 && second.r == 0)
								{
									// Exact independent area integral over u=[-.25,.75], v=[-.5,.5]:
									// foreground v>=u/4-3/8, color split at u=.5. Coverage is
									// 81/128 first color plus 23/128 second, totaling 13/16.
									const glm::vec3 reference = firstColor * (81.0f / 128) + secondColor * (23.0f / 128);
									const glm::vec3 droppedTangent = firstColor * (13.0f / 16);
									CHECK(glm::length(actual - reference) < glm::length(droppedTangent - reference));
								}
							}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
