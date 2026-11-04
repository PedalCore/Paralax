#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Renderer/DepthHierarchy.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Paralax;

TEST_CASE("DepthHierarchy: ceil levels include every edge texel without integer overflow")
{
	CHECK(DepthHierarchy::LevelCount({1, 1}) == 1);
	CHECK(DepthHierarchy::LevelCount({3, 5}) == 4);
	CHECK(DepthHierarchy::LevelSize({3, 5}, 1) == glm::uvec2(2, 3));
	CHECK(DepthHierarchy::LevelSize({3, 5}, 2) == glm::uvec2(1, 2));
	CHECK(DepthHierarchy::LevelSize({3, 5}, 3) == glm::uvec2(1));
	CHECK(DepthHierarchy::LevelSize({3, 5}, 4) == glm::uvec2(0));
	CHECK(DepthHierarchy::LevelCount({32768, 1}) == DepthHierarchy::MaxLevels);
	for (const auto size : {glm::uvec2(0), glm::uvec2(1, 0), glm::uvec2(32769, 1), glm::uvec2(std::numeric_limits<uint32_t>::max())})
	{
		CHECK(DepthHierarchy::LevelCount(size) == 0);
		CHECK(DepthHierarchy::LevelSize(size, 0) == glm::uvec2(0));
	}
	CHECK(DepthHierarchy::LevelSize({1, 1}, std::numeric_limits<uint32_t>::max()) == glm::uvec2(0));
}

namespace {

	nvrhi::TextureHandle HierarchyInput(nvrhi::IDevice* device, glm::uvec2 size, nvrhi::Format format)
	{
		nvrhi::TextureDesc desc;
		desc.width = size.x;
		desc.height = size.y;
		desc.format = format;
		desc.isRenderTarget = format == nvrhi::Format::D32;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		auto texture = device->createTexture(desc);
		REQUIRE(texture);
		return texture;
	}

	class HierarchyFailureFactory : public PostEffectResourceFactory
	{
	public:
		enum class Failure
		{
			None,
			Format,
			Layout,
			Framebuffer,
			Binding,
			Pipeline
		};
		Failure Mode = Failure::None;
		uint32_t Framebuffers = 0, Bindings = 0;
		nvrhi::FormatSupport QueryFormatSupport(nvrhi::IDevice* device, nvrhi::Format format) override
		{
			return Mode == Failure::Format && format == nvrhi::Format::RG32_FLOAT ? nvrhi::FormatSupport::None : PostEffectResourceFactory::QueryFormatSupport(device, format);
		}
		nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc) override
		{
			return Mode == Failure::Layout ? nullptr : PostEffectResourceFactory::CreateBindingLayout(device, desc);
		}
		nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) override
		{
			return Mode == Failure::Framebuffer && ++Framebuffers == 2 ? nullptr : PostEffectResourceFactory::CreateFramebuffer(device, desc);
		}
		nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) override
		{
			return Mode == Failure::Binding && ++Bindings == 2 ? nullptr : PostEffectResourceFactory::CreateBindingSet(device, desc, layout);
		}
		nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) override
		{
			return Mode == Failure::Pipeline ? nullptr : PostEffectResourceFactory::CreatePipeline(device, desc, info);
		}
	};

}

TEST_CASE("DepthHierarchy: GPU intervals match independent covered base rectangles for odd and degenerate sizes")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	auto* device = graphics.GetDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (auto size : {glm::uvec2(1), glm::uvec2(1, 7), glm::uvec2(7, 1), glm::uvec2(3, 5), glm::uvec2(5, 7), glm::uvec2(16, 9)})
	{
		CAPTURE(size.x);
		CAPTURE(size.y);
		PostEffectBudget budget;
		DepthHierarchy hierarchy(graphics, budget);
		auto depth = HierarchyInput(device, size, nvrhi::Format::R32_FLOAT);
		auto coverage = HierarchyInput(device, size, nvrhi::Format::RGBA32_FLOAT);
		REQUIRE(hierarchy.Prepare(depth, coverage));
		const auto attempts = hierarchy.GetAllocationAttempts();
		REQUIRE(hierarchy.Prepare(depth, coverage));
		CHECK(hierarchy.GetAllocationAttempts() == attempts);
		std::vector<float> depths(size.x * size.y);
		std::vector<glm::vec4> guides(depths.size());
		for (uint32_t mode = 0; mode < 3; ++mode)
		{
			CAPTURE(mode);
			for (uint32_t y = 0; y < size.y; ++y)
				for (uint32_t x = 0; x < size.x; ++x)
				{
					const auto i = y * size.x + x;
					depths[i] = float((x + 7 * y) % 17) / 16;
					guides[i] = glm::vec4(0, 0, mode == 0 && (x + 3 * y) % 5 != 0 ? 0.5f : 0, 0);
				}
			if (mode != 1)
			{
				depths.back() = 0;
				guides.back().z = 0.5f;
			}
			if (depths.size() > 5 && mode == 0)
			{
				depths[0] = std::numeric_limits<float>::quiet_NaN();
				depths[1] = std::numeric_limits<float>::infinity();
				depths[2] = -0.1f;
				depths[3] = 1.1f;
				guides[4].z = std::numeric_limits<float>::quiet_NaN();
				for (size_t i = 0; i < 4; ++i)
					guides[i].z = 1;
			}
			auto commands = device->createCommandList();
			commands->open();
			commands->writeTexture(depth, 0, 0, depths.data(), size.x * sizeof(float));
			commands->writeTexture(coverage, 0, 0, guides.data(), size.x * sizeof(glm::vec4));
			hierarchy.Render(commands);
			std::vector<nvrhi::StagingTextureHandle> staging;
			for (uint32_t level = 0; level < hierarchy.GetLevelCount(); ++level)
			{
				auto desc = hierarchy.GetLevel(level)->getDesc();
				desc.isRenderTarget = false;
				staging.push_back(device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read));
				REQUIRE(staging.back());
				commands->copyTexture(staging.back(), {}, hierarchy.GetLevel(level), {});
			}
			commands->close();
			device->executeCommandList(commands);
			graphics.WaitForIdle();
			for (uint32_t level = 0; level < hierarchy.GetLevelCount(); ++level)
			{
				CAPTURE(level);
				size_t pitch = 0;
				const auto* data = static_cast<const uint8_t*>(device->mapStagingTexture(staging[level], {}, nvrhi::CpuAccessMode::Read, &pitch));
				REQUIRE(data);
				const auto levelSize = DepthHierarchy::LevelSize(size, level);
				const uint32_t span = 1u << level;
				for (uint32_t y = 0; y < levelSize.y; ++y)
					for (uint32_t x = 0; x < levelSize.x; ++x)
					{
						glm::vec2 expected(-1);
						// Compute each footprint directly from original pixels, independently of GPU reductions.
						for (uint32_t sy = y * span; sy < std::min((y + 1) * span, size.y); ++sy)
							for (uint32_t sx = x * span; sx < std::min((x + 1) * span, size.x); ++sx)
							{
								const auto i = sy * size.x + sx;
								const float d = depths[i];
								if (!std::isfinite(d) || d < 0 || d > 1 || !std::isfinite(guides[i].z) || guides[i].z <= 0)
									continue;
								expected = expected.x < 0 ? glm::vec2(d) : glm::vec2(std::max(expected.x, d), std::min(expected.y, d));
							}
						glm::vec2 actual;
						std::memcpy(&actual, data + y * pitch + x * sizeof(actual), sizeof(actual));
						CHECK(actual == expected);
					}
				device->unmapStagingTexture(staging[level]);
			}
		}
		CHECK(hierarchy.GetLevel(hierarchy.GetLevelCount()) == nullptr);
		hierarchy.Release();
		graphics.CollectGarbage();
		budget.CollectRetired();
		CHECK(budget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("DepthHierarchy: invalid inputs and partial allocation failures recover without leaking budget")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	auto* device = graphics.GetDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		HierarchyFailureFactory factory;
		DepthHierarchy hierarchy(graphics, budget, &factory);
		auto depth = HierarchyInput(device, {5, 7}, nvrhi::Format::R32_FLOAT);
		auto coverage = HierarchyInput(device, {5, 7}, nvrhi::Format::RGBA32_FLOAT);
		auto wrongSize = HierarchyInput(device, {3, 5}, nvrhi::Format::RGBA32_FLOAT);
		CHECK_FALSE(hierarchy.Prepare(nullptr, coverage));
		CHECK_FALSE(hierarchy.Prepare(depth, nullptr));
		CHECK_FALSE(hierarchy.Prepare(depth, coverage, 4));
		CHECK_FALSE(hierarchy.Prepare(depth, wrongSize));
		auto integer = HierarchyInput(device, {5, 7}, nvrhi::Format::R32_UINT);
		CHECK_FALSE(hierarchy.Prepare(integer, coverage));
		CHECK_FALSE(hierarchy.Prepare(depth, integer, 0));
		CHECK_FALSE(hierarchy.Prepare(depth, depth, 1));
		auto invalidDepthDesc = depth->getDesc();
		invalidDepthDesc.format = nvrhi::Format::D32;
		auto invalidDepth = device->createTexture(invalidDepthDesc);
		REQUIRE(invalidDepth);
		CHECK_FALSE(hierarchy.Prepare(invalidDepth, coverage));
		CHECK(budget.GetUsedBytes() == 0);
		for (auto mode : {HierarchyFailureFactory::Failure::Format, HierarchyFailureFactory::Failure::Layout,
						  HierarchyFailureFactory::Failure::Framebuffer, HierarchyFailureFactory::Failure::Binding, HierarchyFailureFactory::Failure::Pipeline})
		{
			factory.Mode = mode;
			factory.Framebuffers = factory.Bindings = 0;
			CHECK_FALSE(hierarchy.Prepare(depth, coverage));
			CHECK_FALSE(hierarchy.IsReady());
			CHECK(std::string(hierarchy.GetFallbackReason()).size() > 0);
			CHECK(budget.GetUsedBytes() == 0);
			factory.Mode = HierarchyFailureFactory::Failure::None;
			REQUIRE(hierarchy.Prepare(depth, coverage));
			CHECK(hierarchy.GetOwnedBytes() > 0);
			hierarchy.Release();
			CHECK(budget.GetUsedBytes() == 0);
		}
		PostEffectBudget tiny(1);
		DepthHierarchy exhausted(graphics, tiny);
		CHECK_FALSE(exhausted.Prepare(depth, coverage));
		CHECK(tiny.GetUsedBytes() == 0);
		Testing::ScopedAssertionCapture assertions;
		CHECK_THROWS_AS(exhausted.Render(nullptr), Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("DepthHierarchy: D32 coverage and resized outputs preserve far zero and retained backing charges")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	auto* device = graphics.GetDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		PostEffectBudget budget;
		DepthHierarchy hierarchy(graphics, budget);
		nvrhi::TextureHandle retained;
		uint64_t retainedBytes = 0;
		for (auto size : {glm::uvec2(3, 5), glm::uvec2(5, 7)})
		{
			auto depth = HierarchyInput(device, size, nvrhi::Format::D32);
			auto coverage = HierarchyInput(device, size, nvrhi::Format::R32_FLOAT);
			std::vector<float> depths(size.x * size.y, 1), covered(depths.size(), 0);
			depths.back() = 0;
			covered.back() = 1;
			const bool resized = bool(retained);
			if (resized)
			{
				depths.front() = 0.75f;
				covered.front() = 1;
				depths.back() = 0.25f;
			}
			REQUIRE(hierarchy.Prepare(depth, coverage, 0));
			CHECK(hierarchy.GetLevel(0)->getDesc().width == size.x);
			CHECK(hierarchy.GetLevel(0)->getDesc().height == size.y);
			CHECK(budget.GetUsedBytes() == hierarchy.GetOwnedBytes() + retainedBytes);
			auto commands = device->createCommandList();
			commands->open();
			commands->writeTexture(depth, 0, 0, depths.data(), size.x * sizeof(float));
			commands->writeTexture(coverage, 0, 0, covered.data(), size.x * sizeof(float));
			hierarchy.Render(commands);
			auto* last = hierarchy.GetLevel(hierarchy.GetLevelCount() - 1);
			auto desc = last->getDesc();
			desc.isRenderTarget = false;
			auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			REQUIRE(staging);
			commands->copyTexture(staging, {}, last, {});
			auto baseDesc = hierarchy.GetLevel(0)->getDesc();
			baseDesc.isRenderTarget = false;
			auto baseStaging = device->createStagingTexture(baseDesc, nvrhi::CpuAccessMode::Read);
			REQUIRE(baseStaging);
			commands->copyTexture(baseStaging, {}, hierarchy.GetLevel(0), {});
			commands->close();
			device->executeCommandList(commands);
			graphics.WaitForIdle();
			size_t pitch = 0;
			const auto* data = device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch);
			REQUIRE(data);
			glm::vec2 actual(-2);
			std::memcpy(&actual, data, sizeof(actual));
			device->unmapStagingTexture(staging);
			CHECK(actual == (resized ? glm::vec2(0.75f, 0.25f) : glm::vec2(0)));
			const auto* baseData = static_cast<const uint8_t*>(device->mapStagingTexture(baseStaging, {}, nvrhi::CpuAccessMode::Read, &pitch));
			REQUIRE(baseData);
			std::memcpy(&actual, baseData, sizeof(actual));
			CHECK(actual == glm::vec2(resized ? 0.75f : -1));
			std::memcpy(&actual, baseData + (size.y - 1) * pitch + (size.x - 1) * sizeof(actual), sizeof(actual));
			CHECK(actual == glm::vec2(resized ? 0.25f : 0));
			device->unmapStagingTexture(baseStaging);
			commands = nullptr;
			if (!retained)
				retained = hierarchy.GetLevel(0);
			hierarchy.Release();
			graphics.CollectGarbage();
			budget.CollectRetired();
			CHECK(budget.GetUsedBytes() > 0);
			if (!retainedBytes)
				retainedBytes = budget.GetUsedBytes();
			CHECK(budget.GetUsedBytes() == retainedBytes);
		}
		retained = nullptr;
		graphics.CollectGarbage();
		budget.CollectRetired();
		CHECK(budget.GetUsedBytes() == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
