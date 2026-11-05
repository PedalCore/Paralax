#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Core/EmbeddedFile.h>
#include <Paralax/Renderer/FXAA.h>
#include <Paralax/Renderer/ShaderLibrary.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace Paralax::EmbeddedTestShaders {
	extern const EmbeddedFile* const g_Entries;
	extern const size_t g_EntryCount;
}

using namespace Paralax;

namespace {

	double Decode(double encoded)
	{
		return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
	}

	double Encode(double linear)
	{
		return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
	}

	struct Fixture
	{
		GraphicsDevice& Graphics;
		nvrhi::IDevice* Device;
		uint32_t Width, Height;
		PostEffectBudget Budget;
		FXAA AA;
		nvrhi::TextureHandle Output;
		nvrhi::FramebufferHandle Framebuffer;
		std::vector<glm::vec4> Pixels;

		Fixture(GraphicsDevice& graphics, uint32_t width, uint32_t height)
			: Graphics(graphics)
			, Device(graphics.GetDevice())
			, Width(width)
			, Height(height)
			, AA(graphics, Budget)
			, Pixels(width * height, glm::vec4(0, 0, 0, 1))
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA32_FLOAT;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Output = Device->createTexture(desc);
			REQUIRE(Output);
			Framebuffer = Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Output));
			REQUIRE(Framebuffer);
			REQUIRE(AA.Prepare(true, Framebuffer));
		}

		std::vector<glm::vec4> Run(const char* diagnostic = nullptr, glm::vec2 origin = glm::vec2(0))
		{
			auto* input = AA.GetFramebuffer()->getDesc().colorAttachments[0].texture;
			std::vector<uint64_t> packed;
			for (auto pixel : Pixels)
				packed.push_back(glm::packHalf4x16(pixel));
			auto commands = Device->createCommandList();
			commands->open();
			commands->writeTexture(input, 0, 0, packed.data(), Width * sizeof(uint64_t));
			if (!diagnostic)
				AA.Render(commands, Framebuffer);
			else
			{
				const auto code = FindEmbeddedFile(EmbeddedTestShaders::g_Entries, EmbeddedTestShaders::g_EntryCount, "FXAATest");
				REQUIRE_FALSE(code.empty());
				auto shader = [&](const char* entry, nvrhi::ShaderType type) {
					nvrhi::ShaderDesc desc;
					desc.entryName = entry;
					desc.shaderType = type;
					return Device->createShader(desc, code.data(), code.size());
				};
				nvrhi::BindingLayoutDesc layoutDesc;
				layoutDesc.visibility = nvrhi::ShaderType::Pixel;
				layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::vec4)), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0)};
				auto layout = Device->createBindingLayout(layoutDesc);
				auto sampler = Device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
				nvrhi::BindingSetDesc bindingsDesc;
				bindingsDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec4)), nvrhi::BindingSetItem::Texture_SRV(0, input), nvrhi::BindingSetItem::Sampler(0, sampler)};
				auto bindings = Device->createBindingSet(bindingsDesc, layout);
				nvrhi::GraphicsPipelineDesc pipelineDesc;
				pipelineDesc.VS = shader("TestVS", nvrhi::ShaderType::Vertex);
				pipelineDesc.PS = shader(diagnostic, nvrhi::ShaderType::Pixel);
				pipelineDesc.bindingLayouts = {layout};
				pipelineDesc.renderState.rasterState.setCullNone();
				pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
				auto pipeline = Device->createGraphicsPipeline(pipelineDesc, Framebuffer->getFramebufferInfo());
				REQUIRE(pipeline);
				nvrhi::GraphicsState state;
				state.pipeline = pipeline;
				state.framebuffer = Framebuffer;
				state.bindings = {bindings};
				state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(Width), float(Height)));
				commands->setGraphicsState(state);
				const glm::vec4 parameters(float(Width), float(Height), origin.x, origin.y);
				commands->setPushConstants(&parameters, sizeof(parameters));
				commands->draw(nvrhi::DrawArguments().setVertexCount(3));
			}
			auto desc = Output->getDesc();
			desc.isRenderTarget = false;
			auto staging = Device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			REQUIRE(staging);
			commands->copyTexture(staging, {}, Output, {});
			commands->close();
			Device->executeCommandList(commands);
			Device->waitForIdle();
			size_t pitch = 0;
			const auto* data = static_cast<const uint8_t*>(Device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
			REQUIRE(data);
			std::vector<glm::vec4> result(Width * Height);
			for (uint32_t y = 0; y < Height; ++y)
				std::memcpy(result.data() + y * Width, data + y * pitch, Width * sizeof(result[0]));
			Device->unmapStagingTexture(staging);
			return result;
		}
	};

	void NearDithered(const glm::vec4& actual, double encoded)
	{
		const double linear = Decode(encoded);
		// The shader's sinusoidal dither is implementation-dependent in the last bits; bound its
		// exact existing amplitude instead of duplicating GPU transcendental evaluation on the CPU.
		const double tolerance = 0.5 / 255.0 * 2.2 * std::pow(std::max(linear, 1e-6), 1.2 / 2.2) + 0.0003;
		for (int channel = 0; channel < 3; ++channel)
			CHECK(std::abs(static_cast<double>(actual[channel]) - linear) <= tolerance);
		CHECK(actual.a == 1);
	}
}

TEST_CASE("FXAA: exact display transfer breakpoints and encoded bilinear midpoint")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, 9, 1);
		const float values[] = {0, 0.003f, 0.0031308f, 0.004f, 0.04f, 0.04045f, 0.041f, 0.5f, 1};
		for (size_t i = 0; i < fixture.Pixels.size(); ++i)
			fixture.Pixels[i] = glm::vec4(values[i]);
		const auto output = fixture.Run("TransferPS");
		for (size_t i = 0; i < output.size(); ++i)
		{
			const double input = static_cast<double>(glm::unpackHalf4x16(glm::packHalf4x16(fixture.Pixels[i])).r);
			CHECK(output[i].r == doctest::Approx(Decode(input)).epsilon(1e-5));
			CHECK(output[i].g == doctest::Approx(Encode(input)).epsilon(1e-5));
			CHECK(output[i].b == doctest::Approx(input).epsilon(1e-5));
		}
		fixture.Pixels[0] = glm::vec4(0);
		fixture.Pixels[1] = glm::vec4(1);
		CHECK(fixture.Run("HalfTexelPS")[0].r == doctest::Approx(0.21404114048223255).epsilon(1e-5));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: independent impulse and infinite stripe arithmetic includes both orientations")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, 65, 65);
		for (int shape = 0; shape < 4; ++shape)
		{
			CAPTURE(shape);
			std::fill(fixture.Pixels.begin(), fixture.Pixels.end(), glm::vec4(shape == 1 ? 1.0f : 0.0f));
			fixture.Pixels[32 * 65 + 32] = glm::vec4(shape == 1 ? 0.0f : 1.0f);
			if (shape >= 2)
				for (int i = 0; i < 65; ++i)
					fixture.Pixels[shape == 2 ? 32 * 65 + i : i * 65 + 32] = glm::vec4(1);
			const auto result = fixture.Run();
			NearDithered(result[32 * 65 + 32], shape == 0 ? 1.0 / 3 : shape == 1 ? 2.0 / 3
																				 : 7.0 / 9);
			for (const auto& pixel : result)
			{
				CHECK(std::isfinite(pixel.r));
				CHECK(pixel.r >= -1e-5);
				CHECK(pixel.r <= 1.005f);
				CHECK(pixel.a == 1);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: searched endpoints use last tested spans strict signs and deterministic ties")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, 41, 1);
		auto reset = [&]() { std::fill(fixture.Pixels.begin(), fixture.Pixels.end(), glm::vec4(0.5f)); };
		reset();
		fixture.Pixels[18] = glm::vec4(0);
		auto result = fixture.Run("SearchPS", {20.5f, 0.5f})[0];
		CHECK(result.r == doctest::Approx(7.0 / 18));
		CHECK(result.g == 2);
		CHECK(result.b == 16);
		CHECK(result.a == 1);
		fixture.Pixels[18] = glm::vec4(1);
		CHECK(fixture.Run("SearchPS", {20.5f, 0.5f})[0].r == 0);
		fixture.Pixels[18] = glm::vec4(0);
		fixture.Pixels[22] = glm::vec4(1);
		result = fixture.Run("SearchPS", {20.5f, 0.5f})[0];
		CHECK(result.r == 0);
		CHECK(result.a == 3);
		// Equal distances yield zero offset; unequal found distances verify the nearer sign wins.
		fixture.Pixels[22] = glm::vec4(0.5f);
		fixture.Pixels[24] = glm::vec4(1);
		CHECK(fixture.Run("SearchPS", {20.5f, 0.5f})[0].r == doctest::Approx(1.0 / 6));
		reset();
		fixture.Pixels[28] = glm::vec4(0);
		result = fixture.Run("SearchPS", {36.5f, 0.5f})[0];
		CHECK(result.r == 0);
		CHECK(result.g == 8);
		CHECK(result.b == 4);
		CHECK(result.a == 1);
		reset();
		result = fixture.Run("SearchPS", {20.5f, 0.5f})[0];
		CHECK(result.r == 0);
		CHECK(result.g == 16);
		CHECK(result.b == 16);
		CHECK(result.a == 0);
		fixture.Pixels[1] = glm::vec4(0);
		result = fixture.Run("SearchPS", {0.5f, 0.5f})[0];
		CHECK(result.r == 0);
		CHECK(result.g == 0);
		CHECK(result.b == 1);
		CHECK(result.a == 2);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: constant dark colors survive odd narrow and single pixel targets")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		for (auto size : {glm::uvec2(1, 1), glm::uvec2(1, 17), glm::uvec2(19, 1), glm::uvec2(17, 13)})
		{
			Fixture fixture(graphics, size.x, size.y);
			for (float value : {0.0f, 0.015625f, 0.25f, 1.0f})
			{
				std::fill(fixture.Pixels.begin(), fixture.Pixels.end(), glm::vec4(value));
				for (const auto& pixel : fixture.Run())
					NearDithered(pixel, static_cast<double>(value));
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: asymmetric complete filter applies the analytic edge offset in every pair direction")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, 41, 41);
		for (bool transpose : {false, true})
			for (int sign : {-1, 1})
				for (bool inverted : {false, true})
				{
					CAPTURE(transpose);
					CAPTURE(sign);
					CAPTURE(inverted);
					std::fill(fixture.Pixels.begin(), fixture.Pixels.end(), glm::vec4(inverted ? 0.0f : 1.0f));
					auto write = [&](int x, int y) { fixture.Pixels[transpose ? x * 41 + y : y * 41 + x] = glm::vec4(inverted ? 1.0f : 0.0f); };
					for (int x = 0; x < 41; ++x)
						write(x, 20 + sign);
					write(18, 20);
					// Three cardinal neighbors match the center: subpixel weight is exactly zero.
					// The selected pair's midpoint finds the opposite sign at distance2, capped16
					// on the other side: shift7/18, bilinear center value11/18 (or7/18 inverted).
					NearDithered(fixture.Run()[20 * 41 + 20], inverted ? 7.0 / 18 : 11.0 / 18);
				}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: geometric coverage references quantify edge error separately from thin detail attenuation")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, 41, 41);
		double encodedError = 0, baselineEncodedError = 0, linearError = 0, baselineLinearError = 0;
		for (int shape = 0; shape < 5; ++shape)
		{
			auto inside = [&](double x, double y) {
				if (shape == 4)
					return (x - 20.3) * (x - 20.3) + (y - 19.7) * (y - 19.7) < 10.7 * 10.7;
				const double slopes[] = {0.125, 0.5, 1, 2};
				return y < 20.3 + slopes[shape] * (x - 20.5);
			};
			for (uint32_t y = 0; y < 41; ++y)
				for (uint32_t x = 0; x < 41; ++x)
					fixture.Pixels[y * 41 + x] = glm::vec4(inside(x + 0.5, y + 0.5) ? 1.0f : 0.0f);
			const auto result = fixture.Run();
			for (uint32_t y = 2; y < 39; ++y)
				for (uint32_t x = 2; x < 39; ++x)
				{
					// Independent geometric area quadrature, with doubled-rate convergence checked
					// per pixel; no filter thresholds, endpoint rules or texture sampling in the oracle.
					auto coverage = [&](int rate) {
						uint32_t covered = 0;
						for (int sy = 0; sy < rate; ++sy)
							for (int sx = 0; sx < rate; ++sx)
								covered += inside(x + (sx + 0.5) / rate, y + (sy + 0.5) / rate) ? 1u : 0u;
						return covered / static_cast<double>(rate * rate);
					};
					const double reference = coverage(64);
					CHECK(std::abs(reference - coverage(32)) <= 0.04);
					const double actual = std::clamp(static_cast<double>(result[y * 41 + x].r), 0.0, 1.0);
					const double baseline = static_cast<double>(fixture.Pixels[y * 41 + x].r);
					linearError += (actual - reference) * (actual - reference);
					baselineLinearError += (baseline - reference) * (baseline - reference);
					encodedError += std::pow(Encode(actual) - Encode(reference), 2);
					baselineEncodedError += std::pow(baseline - Encode(reference), 2);
				}
		}
		std::cout << "Coverage squared errors: encoded FXAA=" << encodedError << " Off=" << baselineEncodedError << "; linear FXAA=" << linearError << " Off=" << baselineLinearError << "\n";
		INFO("Coverage squared errors: encoded FXAA=" << encodedError << " Off=" << baselineEncodedError << "; linear FXAA=" << linearError << " Off=" << baselineLinearError);
		CHECK(encodedError < baselineEncodedError);
		// Detail is measured separately: a one-pixel stripe retains7/9 encoded contrast,
		// unlike a filter that improves edge error by indiscriminately blurring the image.
		for (uint32_t y = 0; y < 41; ++y)
			for (uint32_t x = 0; x < 41; ++x)
				fixture.Pixels[y * 41 + x] = glm::vec4(y == 20 ? 1.0f : 0.0f);
		const auto detail = fixture.Run();
		NearDithered(detail[20 * 41 + 20], 7.0 / 9);
		NearDithered(detail[16 * 41 + 20], 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: equal luminance chromatic edges preserve color and expose the documented detection limit")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, 17, 13);
		for (uint32_t y = 0; y < 13; ++y)
			for (uint32_t x = 0; x < 17; ++x)
				fixture.Pixels[y * 17 + x] = x < y ? glm::vec4(0.587f, 0, 0, 1) : glm::vec4(0, 0.299f, 0, 1);
		const auto result = fixture.Run();
		for (size_t i = 0; i < result.size(); ++i)
		{
			const auto input = glm::unpackHalf4x16(glm::packHalf4x16(fixture.Pixels[i]));
			for (int channel = 0; channel < 3; ++channel)
			{
				const double expected = Decode(static_cast<double>(input[channel]));
				CHECK(std::abs(static_cast<double>(result[i][channel]) - expected) < 0.003);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("FXAA: absolute and relative contrast thresholds bypass subthreshold edges and preserve colored filtering")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		Fixture fixture(graphics, 17, 13);
		// The bright impulse range3/32 exceeds minimum1/16 but remains below relative1/8.
		// Without the relative cutoff its center becomes15/16 instead of1.
		for (bool dark : {true, false})
		{
			for (uint32_t y = 0; y < 13; ++y)
				for (uint32_t x = 0; x < 17; ++x)
					fixture.Pixels[y * 17 + x] = glm::vec4(dark ? (x < y ? 0.03125f : 0) : (x == 8 && y == 6 ? 1.0f : 0.90625f));
			const auto result = fixture.Run();
			for (size_t i = 0; i < result.size(); ++i)
				NearDithered(result[i], static_cast<double>(fixture.Pixels[i].r));
		}
		std::fill(fixture.Pixels.begin(), fixture.Pixels.end(), glm::vec4(0));
		fixture.Pixels[6 * 17 + 8] = glm::vec4(1, 0, 0, 1);
		const auto center = fixture.Run()[6 * 17 + 8];
		CHECK(std::abs(static_cast<double>(center.r) - Decode(1.0 / 3)) < 0.0015);
		CHECK(std::abs(center.g) < 1e-5f);
		CHECK(std::abs(center.b) < 1e-5f);
		CHECK(center.a == 1);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
