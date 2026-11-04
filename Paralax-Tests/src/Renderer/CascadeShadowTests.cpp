#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Core/EmbeddedFile.h>
#include <Paralax/Renderer/SceneRendererTypes.h>
#include <Paralax/Renderer/ShaderLibrary.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Paralax::EmbeddedTestShaders {
	extern const EmbeddedFile* const g_Entries;
	extern const size_t g_EntryCount;
}

using namespace Paralax;

TEST_CASE("SunShadow: fixed depth layers isolate continuous internal cascade blending")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		const auto code = FindEmbeddedFile(EmbeddedTestShaders::g_Entries, EmbeddedTestShaders::g_EntryCount, "CascadeShadowTest");
		REQUIRE_FALSE(code.empty());
		CHECK(ShaderLibrary::GetModule("CascadeShadowTest").empty());
		nvrhi::ShaderDesc shaderDesc;
		shaderDesc.shaderType = nvrhi::ShaderType::Compute;
		shaderDesc.entryName = "SampleCascadeBoundary";
		auto shader = device->createShader(shaderDesc, code.data(), code.size());
		REQUIRE(shader);
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
							   nvrhi::BindingLayoutItem::Sampler(2), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)};
		auto layout = device->createBindingLayout(layoutDesc);
		REQUIRE(layout);
		auto pipeline = device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
		REQUIRE(pipeline);
		nvrhi::TextureDesc depthDesc;
		depthDesc.width = depthDesc.height = 16;
		depthDesc.arraySize = GPU::MaxCascades;
		depthDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
		depthDesc.format = nvrhi::Format::D32;
		depthDesc.isRenderTarget = true;
		depthDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		depthDesc.keepInitialState = true;
		auto depth = device->createTexture(depthDesc);
		REQUIRE(depth);
		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Border).setBorderColor(nvrhi::Color(1.0f));
		samplerDesc.setReductionType(nvrhi::SamplerReductionType::Comparison);
		auto sampler = device->createSampler(samplerDesc);
		REQUIRE(sampler);
		auto constants = device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(GPU::ViewConstants)).setIsConstantBuffer(true).setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
		REQUIRE(constants);
		constexpr uint32_t SamplesPerBoundary = 65;
		constexpr uint32_t SampleCount = (GPU::MaxCascades - 1) * SamplesPerBoundary;
		using Samples = std::array<float, SampleCount>;
		auto result = device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(Samples)).setStructStride(sizeof(float)).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true));
		auto staging = device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(Samples)).setCpuAccess(nvrhi::CpuAccessMode::Read).setInitialState(nvrhi::ResourceStates::CopyDest).setKeepInitialState(true));
		REQUIRE(result);
		REQUIRE(staging);
		nvrhi::BindingSetDesc bindingsDesc;
		bindingsDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constants), nvrhi::BindingSetItem::Texture_SRV(1, depth),
								 nvrhi::BindingSetItem::Sampler(2, sampler), nvrhi::BindingSetItem::StructuredBuffer_UAV(0, result)};
		auto bindings = device->createBindingSet(bindingsDesc, layout);
		REQUIRE(bindings);
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		for (uint32_t layer = 0; layer < GPU::MaxCascades; ++layer)
			commands->clearDepthStencilTexture(depth, nvrhi::TextureSubresourceSet(0, 1, layer, 1), true, static_cast<float>(layer % 2), false, 0);
		commands->close();
		device->executeCommandList(commands);
		GPU::ViewConstants view{};
		view.SunColor.w = 1;
		view.CascadeNear = 0.1f;
		view.CascadeSplits = {10, 20, 30, 40};
		view.CascadeTexelWorldSize = glm::vec4(1.0f / 16.0f);
		view.ShadowParams = {1.0f / 16.0f, 0, 0, 0.06f};
		for (auto& matrix : view.CascadeViewProjection)
			matrix = glm::mat4(1);
		std::array<Samples, 2> samples{};
		for (size_t mode = 0; mode < samples.size(); ++mode)
		{
			view.CascadeBlend = mode == 0 ? 0.0f : 0.3f;
			commands->open();
			commands->writeBuffer(constants, &view, sizeof(view));
			commands->setBufferState(result, nvrhi::ResourceStates::UnorderedAccess);
			nvrhi::ComputeState state;
			state.pipeline = pipeline;
			state.bindings = {bindings};
			commands->setComputeState(state);
			commands->dispatch(SampleCount, 1, 1);
			commands->copyBuffer(staging, 0, result, 0, sizeof(Samples));
			commands->close();
			device->executeCommandList(commands);
			device->waitForIdle();
			const void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
			REQUIRE(mapped);
			std::memcpy(samples[mode].data(), mapped, sizeof(Samples));
			device->unmapBuffer(staging);
		}
		for (uint32_t boundary = 0; boundary + 1 < GPU::MaxCascades; ++boundary)
		{
			CAPTURE(boundary);
			std::array<float, 2> jumps{};
			const uint32_t first = boundary * SamplesPerBoundary;
			const float start = static_cast<float>(boundary % 2);
			for (size_t mode = 0; mode < samples.size(); ++mode)
			{
				CAPTURE(mode);
				CHECK(samples[mode][first] == doctest::Approx(static_cast<double>(start)).epsilon(1e-5));
				CHECK(samples[mode][first + SamplesPerBoundary - 1] == doctest::Approx(static_cast<double>(1 - start)).epsilon(1e-5));
				CHECK(samples[mode][first + 32] == doctest::Approx(static_cast<double>(mode == 0 ? start : 0.5f)).epsilon(1e-5));
				for (uint32_t i = 0; i < SamplesPerBoundary; ++i)
				{
					const float value = samples[mode][first + i];
					REQUIRE(std::isfinite(value));
					CHECK(value >= 0);
					CHECK(value <= 1);
					if (i > 0)
						jumps[mode] = std::max(jumps[mode], std::abs(value - samples[mode][first + i - 1]));
				}
			}
			// Alternating fixed layers provide a real hard discontinuity without moving geometry, texel snapping
			// or varying cascade fits. Disabling blending or the neighbor lookup must fail this positive control.
			CHECK(jumps[0] * 255.0f > 3.0f);
			CHECK(jumps[1] < jumps[0] * 0.6f);
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
