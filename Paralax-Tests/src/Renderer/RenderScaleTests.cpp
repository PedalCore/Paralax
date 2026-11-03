#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Renderer/RenderScale.h>

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
	CHECK(RenderScale::CalculateDimensions(17, 13, 10) == glm::uvec2(17, 13));
	CHECK(RenderScale::CalculateDimensions(17, 13, std::numeric_limits<float>::quiet_NaN()) == glm::uvec2(17, 13));
	CHECK(RenderScale::CalculateDimensions(17, 13, std::numeric_limits<float>::infinity()) == glm::uvec2(17, 13));
	CHECK(RenderScale::CalculateDimensions(~0u, ~0u, 1) == glm::uvec2(~0u));
	CHECK(RenderScale::CalculateDimensions(~0u, ~0u, 0.5f) == glm::uvec2(2147483648u));
}

TEST_CASE("RenderScale: native depth preserves nearest raster samples at odd and minimal dimensions")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		PostEffectBudget budget;
		RenderScale scale(graphics, budget);
		for (auto output : {glm::uvec2(1), glm::uvec2(17, 13), glm::uvec2(32, 16)})
		{
			const auto input = RenderScale::CalculateDimensions(output.x, output.y, 0.5f);
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
				values[i] = static_cast<float>(i) / static_cast<float>(values.size());
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
					CHECK(actual == values[sy * input.x + sx]);
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
