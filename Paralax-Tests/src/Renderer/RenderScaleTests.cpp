#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Renderer/RenderScale.h>
#include <Paralax/Renderer/SceneRendererTypes.h>
#include <Paralax/Renderer/ShaderLibrary.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace Paralax;

TEST_CASE("RenderScale: dimensions round safely and sanitize invalid scale without changing the output aspect contract")
{
	CHECK(RenderScale::CalculateDimensions(1920, 1080, 0.75f) == glm::uvec2(1440, 810));
	CHECK(RenderScale::CalculateDimensions(17, 13, 0.5f) == glm::uvec2(9, 7));
	CHECK(RenderScale::CalculateDimensions(1, 1, 0.5f) == glm::uvec2(1));
	CHECK(RenderScale::CalculateDimensions(0, 1, 0.5f) == glm::uvec2(0, 1));
	CHECK(RenderScale::CalculateDimensions(17, 13, -10) == glm::uvec2(9, 7));
	CHECK(RenderScale::CalculateDimensions(17, 13, 10) == glm::uvec2(34, 26));
	CHECK(RenderScale::CalculateDimensions(17, 13, std::numeric_limits<float>::quiet_NaN()) == glm::uvec2(17, 13));
	CHECK(RenderScale::CalculateDimensions(17, 13, std::numeric_limits<float>::infinity()) == glm::uvec2(17, 13));
	CHECK(RenderScale::CalculateDimensions(17, 13, 1.5f) == glm::uvec2(26, 20));
	CHECK(RenderScale::CalculateDimensions(~0u, ~0u, 2) == glm::uvec2(~0u));
	CHECK(RenderScale::CalculateDimensions(~0u, ~0u, 1) == glm::uvec2(~0u));
	CHECK(RenderScale::CalculateDimensions(~0u, ~0u, 0.5f) == glm::uvec2(2147483648u));
}

TEST_CASE("RenderScale: native depth preserves nearest upscale and closest overlapping supersamples at odd dimensions")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		PostEffectBudget budget;
		RenderScale scale(graphics, budget);
		for (float factor : {0.5f, 1.5f, 2.0f})
			for (auto output : {glm::uvec2(1), glm::uvec2(17, 13), glm::uvec2(32, 16)})
			{
				const auto input = RenderScale::CalculateDimensions(output.x, output.y, factor);
				nvrhi::TextureDesc desc;
				desc.width = input.x;
				desc.height = input.y;
				desc.format = nvrhi::Format::D32;
				desc.isRenderTarget = true;
				desc.initialState = nvrhi::ResourceStates::DepthWrite;
				desc.keepInitialState = true;
				auto source = device->createTexture(desc);
				REQUIRE(source);
				REQUIRE(scale.Prepare(true, source, output.x, output.y));
				const auto attempts = scale.GetAllocationAttempts();
				CHECK(scale.Prepare(true, source, output.x, output.y));
				CHECK(scale.GetAllocationAttempts() == attempts);
				std::vector<float> values(input.x * input.y);
				for (uint32_t i = 0; i < values.size(); ++i)
					values[i] = static_cast<float>((i * 37) % 101) / 100.0f;
				auto stagingDesc = scale.GetDepth()->getDesc();
				stagingDesc.isRenderTarget = false;
				stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
				auto staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
				REQUIRE(staging);
				auto commands = device->createCommandList();
				commands->open();
				commands->writeTexture(source, 0, 0, values.data(), input.x * sizeof(float));
				scale.RenderDepth(commands);
				commands->copyTexture(staging, {}, scale.GetDepth(), {});
				commands->close();
				device->executeCommandList(commands);
				device->waitForIdle();
				size_t pitch = 0;
				const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
				REQUIRE(mapped);
				for (uint32_t y = 0; y < output.y; ++y)
					for (uint32_t x = 0; x < output.x; ++x)
					{
						const uint32_t sx = static_cast<uint32_t>((static_cast<double>(x) + 0.5) * input.x / output.x);
						const uint32_t sy = static_cast<uint32_t>((static_cast<double>(y) + 0.5) * input.y / output.y);
						float actual;
						std::memcpy(&actual, mapped + y * pitch + x * sizeof(float), sizeof(float));
						float expected = values[sy * input.x + sx];
						if (factor > 1)
						{
							expected = 0;
							for (uint32_t iy = 0; iy < input.y; ++iy)
								for (uint32_t ix = 0; ix < input.x; ++ix)
									if (uint64_t(ix) * output.x < uint64_t(x + 1) * input.x && uint64_t(ix + 1) * output.x > uint64_t(x) * input.x &&
										uint64_t(iy) * output.y < uint64_t(y + 1) * input.y && uint64_t(iy + 1) * output.y > uint64_t(y) * input.y)
										expected = std::max(expected, values[iy * input.x + ix]);
						}
						CHECK(actual == expected);
					}
				device->unmapStagingTexture(staging);
				commands = nullptr;
				scale.Release();
				CHECK(budget.GetUsedBytes() == 0);
			}
		Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(scale.RenderDepth(nullptr), Testing::AssertionFailedError);
		CHECK_FALSE(scale.Prepare(true, nullptr, 17, 13));
		CHECK(budget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("RenderScale: every partial allocation and missing layout retries without retaining budget")
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
			nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateBindingLayout(device, desc);
			}
			nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
			}
			nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreateFramebuffer(device, desc);
			}
			nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
			{
				return ++Calls == FailAt ? nullptr : PostEffectResourceFactory::CreatePipeline(device, desc, info);
			}
		} factory;
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 8;
		desc.format = nvrhi::Format::D32;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::DepthWrite;
		desc.keepInitialState = true;
		auto source = graphics.GetDevice()->createTexture(desc);
		REQUIRE(source);
		PostEffectBudget budget;
		for (uint32_t failure = 1; failure <= 6; ++failure)
		{
			CAPTURE(failure);
			RenderScale scale(graphics, budget, &factory);
			factory.Calls = 0;
			factory.FailAt = failure;
			CHECK_FALSE(scale.Prepare(true, source, 17, 13));
			CHECK(budget.GetUsedBytes() == 0);
			factory.FailAt = 0;
			CHECK(scale.Prepare(true, source, 17, 13));
			CHECK(budget.GetUsedBytes() > 0);
			scale.Release();
			CHECK(budget.GetUsedBytes() == 0);
		}
		PostEffectBudget tinyBudget(1);
		RenderScale overBudget(graphics, tinyBudget);
		CHECK_FALSE(overBudget.Prepare(true, source, 17, 13));
		CHECK(overBudget.GetBackingAttempts() == 0);
		CHECK(tinyBudget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("RenderScale: area reduction conserves linear HDR energy and bright thin features on odd grids")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		for (const auto output : {glm::uvec2(1), glm::uvec2(2, 3), glm::uvec2(17, 13)})
			for (float factor : {1.5f, 2.0f})
			{
				const auto input = RenderScale::CalculateDimensions(output.x, output.y, factor);
				CAPTURE(output.x);
				CAPTURE(output.y);
				CAPTURE(factor);
				nvrhi::TextureDesc desc;
				desc.width = input.x;
				desc.height = input.y;
				desc.format = nvrhi::Format::RGBA32_FLOAT;
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				auto source = device->createTexture(desc);
				desc.width = output.x;
				desc.height = output.y;
				desc.isRenderTarget = true;
				auto target = device->createTexture(desc);
				REQUIRE(source);
				REQUIRE(target);
				auto framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
				nvrhi::BindingLayoutDesc layoutDesc;
				layoutDesc.visibility = nvrhi::ShaderType::Pixel;
				layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(GPU::TonemapConstants)), nvrhi::BindingLayoutItem::Texture_SRV(0),
									   nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Sampler(1)};
				auto layout = device->createBindingLayout(layoutDesc);
				auto point = device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(false));
				auto linear = device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(true));
				nvrhi::BindingSetDesc set;
				set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::TonemapConstants)), nvrhi::BindingSetItem::Texture_SRV(0, source),
								nvrhi::BindingSetItem::Texture_SRV(1, source), nvrhi::BindingSetItem::Sampler(0, point), nvrhi::BindingSetItem::Sampler(1, linear)};
				auto bindings = device->createBindingSet(set, layout);
				nvrhi::GraphicsPipelineDesc pipelineDesc;
				pipelineDesc.VS = ShaderLibrary::CreateShader(device, "Tonemap", "TonemapVS", nvrhi::ShaderType::Vertex);
				pipelineDesc.PS = ShaderLibrary::CreateShader(device, "Tonemap", "TonemapPS", nvrhi::ShaderType::Pixel);
				pipelineDesc.bindingLayouts = {layout};
				pipelineDesc.renderState.rasterState.setCullNone();
				pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
				auto pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
				REQUIRE(pipeline);
				auto stagingDesc = desc;
				stagingDesc.isRenderTarget = false;
				stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
				auto staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
				REQUIRE(staging);
				for (uint32_t pattern = 0; pattern < 4; ++pattern)
				{
					CAPTURE(pattern);
					std::vector<glm::vec4> values(input.x * input.y);
					double inputEnergy = 0;
					for (uint32_t y = 0; y < input.y; ++y)
						for (uint32_t x = 0; x < input.x; ++x)
						{
							const float value = pattern == 0 ? 3.25f : pattern == 1 ? (x == input.x / 2 ? 64.0f : 0.0f)
																   : pattern == 2   ? (y == input.y - 1 ? 16.0f : 0.0f)
																					: ((x + y) % 2 ? 8.0f : 0.125f);
							values[y * input.x + x] = {value, value, value, 1};
							inputEnergy += static_cast<double>(value);
						}
					auto commands = device->createCommandList();
					commands->open();
					commands->writeTexture(source, 0, 0, values.data(), input.x * sizeof(glm::vec4));
					nvrhi::GraphicsState state;
					state.pipeline = pipeline;
					state.framebuffer = framebuffer;
					state.bindings = {bindings};
					state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(output.x), float(output.y)));
					commands->setGraphicsState(state);
					GPU::TonemapConstants constants{1, 3, 0, 2};
					constants.Dimensions = {input.x, input.y, output.x, output.y};
					commands->setPushConstants(&constants, sizeof(constants));
					commands->draw(nvrhi::DrawArguments().setVertexCount(3));
					commands->copyTexture(staging, {}, target, {});
					commands->close();
					device->executeCommandList(commands);
					device->waitForIdle();
					size_t pitch = 0;
					const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
					REQUIRE(mapped);
					double outputEnergy = 0;
					for (uint32_t y = 0; y < output.y; ++y)
						for (uint32_t x = 0; x < output.x; ++x)
						{
							// Independent continuous box integral over every input cell, in double precision.
							const double x0 = double(x) * input.x / output.x, x1 = double(x + 1) * input.x / output.x;
							const double y0 = double(y) * input.y / output.y, y1 = double(y + 1) * input.y / output.y;
							double expected = 0;
							for (uint32_t iy = 0; iy < input.y; ++iy)
								for (uint32_t ix = 0; ix < input.x; ++ix)
								{
									const double dx = std::max(0.0, std::min(x1, double(ix + 1)) - std::max(x0, double(ix)));
									const double dy = std::max(0.0, std::min(y1, double(iy + 1)) - std::max(y0, double(iy)));
									expected += static_cast<double>(values[iy * input.x + ix].r) * dx * dy / ((x1 - x0) * (y1 - y0));
								}
							glm::vec4 actual;
							std::memcpy(&actual, mapped + y * pitch + x * sizeof(actual), sizeof(actual));
							for (int channel = 0; channel < 3; ++channel)
							{
								CHECK(std::isfinite(actual[channel]));
								CHECK(std::abs(static_cast<double>(actual[channel]) - expected) < 2e-5);
							}
							outputEnergy += static_cast<double>(actual.r);
						}
					device->unmapStagingTexture(staging);
					CHECK(std::abs(outputEnergy / (output.x * output.y) - inputEnergy / (input.x * input.y)) < 2e-5);
				}
			}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
