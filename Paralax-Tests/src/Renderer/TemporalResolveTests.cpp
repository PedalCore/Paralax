#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Renderer/ShaderLibrary.h>

#include <glm/glm.hpp>

#include <array>
#include <cstring>
#include <limits>

using namespace Paralax;

namespace {

	// Float fixtures isolate the shader's acceptance rules from geometry rasterization and FP16 storage.
	struct ResolveFixture
	{
		static constexpr uint32_t Size = 5;
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
		std::array<glm::vec4, Size * Size> Color, History, Metadata;
		std::array<glm::vec2, Size * Size> Normal, PreviousUV;
		std::array<float, Size * Size> Depth, PreviousDepth, Reactive, HistoryDepth;
		std::array<uint32_t, Size * Size> ID, HistoryID;

		explicit ResolveFixture(nvrhi::IDevice* device)
			: Device(device)
		{
			const std::array formats{nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::R32_FLOAT, nvrhi::Format::RG32_FLOAT,
									 nvrhi::Format::R32_UINT, nvrhi::Format::RG32_FLOAT, nvrhi::Format::R32_FLOAT, nvrhi::Format::R32_FLOAT,
									 nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::R32_UINT, nvrhi::Format::R32_FLOAT};
			for (size_t i = 0; i < formats.size(); ++i)
				Inputs[i] = Texture(formats[i]);
			Outputs = {Texture(nvrhi::Format::RGBA32_FLOAT), Texture(nvrhi::Format::RGBA32_FLOAT), Texture(nvrhi::Format::R32_UINT), Texture(nvrhi::Format::R32_FLOAT)};
			nvrhi::FramebufferDesc framebuffer;
			for (const auto& texture : Outputs)
				framebuffer.addColorAttachment(texture);
			Framebuffer = device->createFramebuffer(framebuffer);
			REQUIRE(Framebuffer);
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants))};
			for (uint32_t slot = 0; slot < Inputs.size(); ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			Layout = device->createBindingLayout(layout);
			REQUIRE(Layout);
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants))};
			for (uint32_t slot = 0; slot < Inputs.size(); ++slot)
				bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(slot, Inputs[slot]));
			Bindings = device->createBindingSet(bindings, Layout);
			REQUIRE(Bindings);
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = ShaderLibrary::CreateShader(device, "TemporalResolve", "ResolveVS", nvrhi::ShaderType::Vertex);
			pipeline.PS = ShaderLibrary::CreateShader(device, "TemporalResolve", "ResolvePS", nvrhi::ShaderType::Pixel);
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
			Push = {};
			Color.fill(glm::vec4(0.5f));
			// Keep an unclipped [0,1] same-surface neighborhood around the measured center.
			Color[6] = glm::vec4(0);
			Color[8] = glm::vec4(1);
			History.fill(glm::vec4(0.25f));
			Metadata.fill(glm::vec4(0));
			Normal.fill(glm::vec2(0));        // Octahedral +Z.
			PreviousUV.fill(glm::vec2(0.6f)); // Equal four-tap footprint at (2,2)..(3,3).
			Depth.fill(0.5f);
			PreviousDepth.fill(-0.5f);
			Reactive.fill(0);
			HistoryDepth.fill(-0.5f);
			ID.fill(7);
			HistoryID.fill(7);
		}

		glm::vec4 Run(uint32_t output = 0)
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
			commands->setPushConstants(&Push, sizeof(Push));
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
			glm::vec4 result;
			std::memcpy(&result, data + 2 * pitch + 2 * sizeof(result), sizeof(result));
			Device->unmapStagingTexture(staging);
			return result;
		}
	};

}

TEST_CASE("TemporalResolve: each history tap rejects incompatible identity depth normal reactivity and nonfinite data before filtering")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		ResolveFixture fixture(graphics.GetDevice());
		CHECK(fixture.Run().x == doctest::Approx(0.44375).epsilon(1e-5));
		for (int rejection = 0; rejection < 7; ++rejection)
		{
			CAPTURE(rejection);
			fixture.Reset();
			// Three invalid, brightly colored taps must not contaminate the one valid tap.
			for (size_t pixel : {13u, 17u, 18u})
			{
				fixture.History[pixel] = glm::vec4(1);
				switch (rejection)
				{
					case 0: fixture.HistoryID[pixel] = 8; break;
					case 1: fixture.HistoryDepth[pixel] = -2; break;
					case 2: fixture.Metadata[pixel].x = 1; break;
					case 3: fixture.Metadata[pixel].z = 1; break;
					case 4: fixture.History[pixel].x = std::numeric_limits<float>::quiet_NaN(); break;
					case 5: fixture.HistoryDepth[pixel] = std::numeric_limits<float>::infinity(); break;
					case 6: fixture.Metadata[pixel].x = std::numeric_limits<float>::quiet_NaN(); break;
				}
			}
			CHECK(fixture.Run().x == doctest::Approx(0.44375).epsilon(1e-5));
			fixture.HistoryID[12] = 8;
			CHECK(fixture.Run().x == doctest::Approx(0.5));
		}
		fixture.Reset();
		fixture.PreviousUV.fill(glm::vec2(-1));
		CHECK(fixture.Run().x == doctest::Approx(0.5));
		fixture.Reset();
		fixture.Reactive[12] = 1;
		CHECK(fixture.Run().x == doctest::Approx(0.5));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalResolve: mixed silhouettes reconstruct current color without history and store rejection for the next frame")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		ResolveFixture fixture(graphics.GetDevice());
		for (uint32_t adjacent : {0u, 8u})
		{
			fixture.Reset();
			fixture.Push.JitterHistory.x = 0.25f;
			fixture.Push.JitterHistory.y = 0.25f;
			fixture.Color.fill(glm::vec4(0));
			fixture.ID.fill(adjacent);
			for (uint32_t y = 0; y <= 2; ++y)
				for (uint32_t x = 0; x <= 2; ++x)
				{
					fixture.Color[y * ResolveFixture::Size + x] = glm::vec4(1);
					fixture.ID[y * ResolveFixture::Size + x] = 7;
				}
			// The aligned quadrant covers 3/4 of each dimension of this shifted pixel.
			// Its analytic area is 9/16, independent of any reconstruction implementation.
			CHECK(fixture.Run().x == doctest::Approx(0.5625));
			fixture.History.fill(glm::vec4(100));
			CHECK(fixture.Run().x == doctest::Approx(0.5625));
			CHECK(fixture.Run(1).z == 1);
		}
		fixture.Reset();
		fixture.ID.fill(0);
		CHECK(fixture.Run().x == doctest::Approx(0.5));
		fixture.Reset();
		fixture.Color.fill(glm::vec4(0.6f));
		fixture.History.fill(glm::vec4(10));
		CHECK(fixture.Run().x == doctest::Approx(0.6)); // Same-surface neighborhood clamp.
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("TemporalResolve: chroma bounds reject stale yellow and luminance weighting suppresses HDR flashes")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		ResolveFixture fixture(graphics.GetDevice());
		fixture.Color.fill({1, 0, 0, 1});
		fixture.Color[6] = {0, 1, 0, 1};
		fixture.History.fill({1, 1, 0, 1});
		const auto chroma = fixture.Run();
		CHECK(chroma.r == doctest::Approx(1));
		CHECK(chroma.g == doctest::Approx(0.05).epsilon(1e-5));
		CHECK(std::abs(chroma.b) < 1e-6f);
		fixture.Color[6] = {0, 0, 1, 1};
		fixture.History.fill({0, 0, 1, 1});
		const auto equalLuminance = fixture.Run();
		CHECK(equalLuminance.r == doctest::Approx(0.1).epsilon(1e-5));
		CHECK(equalLuminance.b == doctest::Approx(0.9).epsilon(1e-5));
		fixture.Reset();
		fixture.Color.fill(glm::vec4(1));
		fixture.Color[6] = glm::vec4(100);
		fixture.History.fill(glm::vec4(100));
		const auto flash = fixture.Run();
		CHECK(flash.r > 1);
		CHECK(flash.r < 1.02f);
		fixture.History.fill(glm::vec4(1));
		CHECK(fixture.Run().r == doctest::Approx(1));
		fixture.History.fill(glm::vec4(100));
		fixture.Push.JitterHistory.z = 0;
		CHECK(fixture.Run().r == doctest::Approx(1));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
