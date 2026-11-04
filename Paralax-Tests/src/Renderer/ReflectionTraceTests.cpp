#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Renderer/DepthHierarchy.h>
#include <Paralax/Renderer/SceneRendererTypes.h>
#include <Paralax/Renderer/ShaderLibrary.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace Paralax;

namespace {

	glm::vec2 EncodeTraceNormal(glm::vec3 normal)
	{
		normal /= std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
		return normal.z >= 0 ? glm::vec2(normal) : (glm::vec2(1) - glm::abs(glm::vec2(normal.y, normal.x))) * glm::vec2(normal.x >= 0 ? 1 : -1, normal.y >= 0 ? 1 : -1);
	}

	struct TraceFixture
	{
		uint32_t Size;
		glm::uvec2 Receiver, Target;
		glm::mat4 Projection{1};
		glm::vec3 Position{0}, Direction = glm::normalize(glm::vec3(1, 0, -1));
		glm::vec3 Normal = glm::normalize(glm::vec3(0, 0, 1) + Direction);
		float Distance = 10, TargetDepth = 0;
		uint32_t Steps = 256;
		std::vector<float> Depth;
		std::vector<glm::vec4> Normals, Guides, Source, Base;

		explicit TraceFixture(uint32_t size)
			: Size(size)
			, Depth(size * size)
			, Normals(size * size)
			, Guides(size * size)
			, Source(size * size)
			, Base(size * size, glm::vec4(1))
		{
		}
		float ProjectDepth(glm::vec3 world) const
		{
			const auto clip = Projection * glm::vec4(world, 1);
			return clip.z / clip.w;
		}
		void Surface(glm::uvec2 pixel, float depth, glm::vec3 normal, glm::vec3 color, float roughness)
		{
			const auto i = pixel.y * Size + pixel.x;
			Depth[i] = depth;
			const auto encoded = EncodeTraceNormal(normal);
			Normals[i] = glm::vec4(encoded, encoded);
			Guides[i] = glm::vec4(encoded, roughness, 1);
			Source[i] = glm::vec4(color, 1);
		}
		glm::vec4 Run(GraphicsDevice& graphics, glm::vec4* inspectionDetails = nullptr, uint32_t inspectionPart = 0)
		{
			auto* device = graphics.GetDevice();
			auto texture = [&](nvrhi::Format format, bool target = false) {
				nvrhi::TextureDesc desc;
				desc.width = desc.height = Size;
				desc.format = format;
				desc.isRenderTarget = target;
				desc.initialState = target ? nvrhi::ResourceStates::RenderTarget : nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				auto result = device->createTexture(desc);
				REQUIRE(result);
				return result;
			};
			std::array<nvrhi::TextureHandle, 8> inputs;
			for (uint32_t i = 0; i < inputs.size(); ++i)
				inputs[i] = texture(i == 0 ? nvrhi::Format::R32_FLOAT : i == 6 ? nvrhi::Format::RG32_FLOAT
																			   : nvrhi::Format::RGBA32_FLOAT);
			auto output = texture(nvrhi::Format::RGBA32_FLOAT, true), hit = texture(nvrhi::Format::RGBA32_FLOAT, true);
			PostEffectBudget budget;
			DepthHierarchy hierarchy(graphics, budget);
			REQUIRE(hierarchy.Prepare(inputs[0], inputs[2]));
			GPU::ViewConstants view{};
			view.View = glm::mat4(1);
			view.Projection = view.ViewProjection = Projection;
			view.InverseViewProjection = glm::inverse(Projection);
			view.Diagnostics.x = 1;
			nvrhi::BufferDesc buffer;
			buffer.byteSize = sizeof(view);
			buffer.isConstantBuffer = true;
			buffer.initialState = nvrhi::ResourceStates::ConstantBuffer;
			buffer.keepInitialState = true;
			auto constants = device->createBuffer(buffer);
			REQUIRE(constants);
			struct Parameters
			{
				glm::vec4 Settings;
				glm::uvec4 Dimensions;
				glm::uvec4 Probe{0};
			} parameters{{Distance, 0.1f, 0.8f, float(Steps)}, {Size, Size, hierarchy.GetLevelCount(), 0}, {Receiver.x, Receiver.y, inspectionPart, 0}};
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::AllGraphics;
			layoutDesc.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::PushConstants(15, sizeof(parameters)), nvrhi::BindingLayoutItem::Sampler(0)};
			for (uint32_t i = 0; i < 24; ++i)
				layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(i));
			auto layout = device->createBindingLayout(layoutDesc);
			REQUIRE(layout);
			auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
			REQUIRE(sampler);
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constants), nvrhi::BindingSetItem::PushConstants(15, sizeof(parameters)), nvrhi::BindingSetItem::Sampler(0, sampler)};
			for (uint32_t i = 0; i < 16; ++i)
				set.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(i, hierarchy.GetLevel(std::min(i, hierarchy.GetLevelCount() - 1))));
			for (uint32_t i = 0; i < inputs.size(); ++i)
				set.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(16 + i, inputs[i]));
			auto bindings = device->createBindingSet(set, layout);
			REQUIRE(bindings);
			auto framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output).addColorAttachment(hit));
			REQUIRE(framebuffer);
			nvrhi::GraphicsPipelineDesc pipelineDesc;
			pipelineDesc.VS = ShaderLibrary::CreateShader(device, "ScreenSpaceReflections", "ReflectionVS", nvrhi::ShaderType::Vertex);
			pipelineDesc.PS = ShaderLibrary::CreateShader(device, "ScreenSpaceReflections", inspectionDetails ? "InspectionPS" : "TracePS", nvrhi::ShaderType::Pixel);
			pipelineDesc.bindingLayouts = {layout};
			pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
			pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
			pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
			auto pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
			REQUIRE(pipeline);
			auto commands = device->createCommandList();
			commands->open();
			commands->writeBuffer(constants, &view, sizeof(view));
			commands->writeTexture(inputs[0], 0, 0, Depth.data(), Size * sizeof(float));
			commands->writeTexture(inputs[1], 0, 0, Normals.data(), Size * sizeof(glm::vec4));
			commands->writeTexture(inputs[2], 0, 0, Guides.data(), Size * sizeof(glm::vec4));
			commands->writeTexture(inputs[3], 0, 0, Source.data(), Size * sizeof(glm::vec4));
			commands->writeTexture(inputs[4], 0, 0, Base.data(), Size * sizeof(glm::vec4));
			commands->writeTexture(inputs[5], 0, 0, Source.data(), Size * sizeof(glm::vec4));
			std::vector<glm::vec2> brdf(Size * Size, {1, 0});
			commands->writeTexture(inputs[6], 0, 0, brdf.data(), Size * sizeof(glm::vec2));
			commands->writeTexture(inputs[7], 0, 0, Source.data(), Size * sizeof(glm::vec4));
			hierarchy.Render(commands);
			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.bindings = {bindings};
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(Size), float(Size)));
			commands->setGraphicsState(state);
			commands->setPushConstants(&parameters, sizeof(parameters));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
			auto desc = output->getDesc();
			desc.isRenderTarget = false;
			auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			REQUIRE(staging);
			commands->copyTexture(staging, {}, output, {});
			auto detailStaging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			REQUIRE(detailStaging);
			if (inspectionDetails)
				commands->copyTexture(detailStaging, {}, hit, {});
			commands->close();
			device->executeCommandList(commands);
			graphics.WaitForIdle();
			size_t pitch = 0;
			const auto* data = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
			REQUIRE(data);
			glm::vec4 result;
			std::memcpy(&result, data + Receiver.y * pitch + Receiver.x * sizeof(result), sizeof(result));
			device->unmapStagingTexture(staging);
			if (inspectionDetails)
			{
				data = static_cast<const uint8_t*>(device->mapStagingTexture(detailStaging, {}, nvrhi::CpuAccessMode::Read, &pitch));
				REQUIRE(data);
				std::memcpy(inspectionDetails, data + Receiver.y * pitch + Receiver.x * sizeof(result), sizeof(result));
				device->unmapStagingTexture(detailStaging);
			}
			return result;
		}
	};

}

TEST_CASE("ScreenSpaceReflections: a thickness hit at the outgoing cell boundary retains the validated source texel")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		TraceFixture fixture(8);
		fixture.Receiver = {1, 4};
		fixture.Target = {3, 4};
		fixture.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 100.0f, 0.1f);
		fixture.Position = {-0.625f, -0.125f, -1.5f};
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		const float exitDistance = -origin.x / fixture.Direction.x;
		const auto exit = origin + fixture.Direction * exitDistance;
		const auto target = exit - glm::vec3(0, 0, 0.05f);
		CHECK(exit.x == doctest::Approx(0).epsilon(0.00001));
		CHECK(target.z < exit.z);
		CHECK(exit.z - target.z < 0.1f);
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		fixture.Surface(fixture.Target, fixture.ProjectDepth(target), {0, 0, 1}, {1, 0, 0}, 0.5f);
		fixture.Surface({4, 4}, fixture.ProjectDepth({0.125f, -0.125f, -0.2f}), {0, 0, 1}, {0, 1, 0}, 0.5f);
		const auto result = fixture.Run(graphics);
		CAPTURE(result.x);
		CAPTURE(result.y);
		CAPTURE(result.w);
		CHECK(result.x > 0.99f);
		CHECK(result.y == 0);
		CHECK(result.w > 0);
		glm::vec4 details;
		const auto inspected = fixture.Run(graphics, &details);
		CHECK(inspected.x == float(fixture.Target.x));
		CHECK(inspected.y == float(fixture.Target.y));
		CHECK(inspected.z == doctest::Approx(double(exitDistance)).epsilon(0.001));
		CHECK(inspected.w == result.w);
		CHECK(details.x == doctest::Approx(-0.05).epsilon(0.001));
		CHECK(details.y == doctest::Approx(double(fixture.Direction.z)).epsilon(0.001));
		CHECK(details.z == 5);
		CHECK(details.w > 0);
		glm::vec4 termination;
		const auto plane = fixture.Run(graphics, &termination, 1);
		CHECK(plane.x == doctest::Approx(4.2).epsilon(0.0001));
		CHECK(plane.y == doctest::Approx(4.5).epsilon(0.0001));
		CHECK(plane.z == doctest::Approx(double(exitDistance + 0.05f / -fixture.Direction.z)).epsilon(0.0001));
		CHECK(static_cast<uint32_t>(plane.w) == 23u);
		CHECK(termination.x == 4); // The next cell is a different foreground plane.
		CHECK(termination.y == 1);
		CHECK(glm::vec2(termination.z, termination.w) == glm::vec2(4, 4));
		fixture.Guides[fixture.Target.y * fixture.Size + fixture.Target.x].z = 0;
		CHECK(fixture.Run(graphics) == glm::vec4(0));
		const auto missed = fixture.Run(graphics, &details);
		CHECK(missed.w == 0);
		CHECK(details.z == 1);
		CHECK(std::isnan(missed.x));
		CHECK(std::isnan(details.w));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: a tiny nonzero reversed Z delta still finds a distant planar hit")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		TraceFixture fixture(9);
		fixture.Receiver = {4, 4};
		fixture.Target = {5, 4};
		fixture.Projection = glm::perspectiveRH_ZO(2 * std::atan(0.25f), 1.0f, 100000.0f, 0.0001f);
		fixture.Position = {0, 0, -1000};
		fixture.Distance = 100;
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		const auto clip = fixture.Projection * glm::vec4(origin, 1);
		const auto slope = fixture.Projection * glm::vec4(fixture.Direction, 0);
		const float targetNDC = 5.5f / 9 * 2 - 1;
		const float distance = (targetNDC * clip.w - clip.x) / (slope.x - targetNDC * slope.w);
		REQUIRE(distance > 0);
		REQUIRE(distance < fixture.Distance);
		const auto target = origin + fixture.Direction * distance;
		const float delta = fixture.ProjectDepth(origin + fixture.Direction * fixture.Distance) - fixture.ProjectDepth(origin);
		REQUIRE(std::abs(delta) > 0);
		REQUIRE(std::abs(delta) < 1e-8f);
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		fixture.Surface(fixture.Target, fixture.ProjectDepth(target), {0, 0, 1}, {1, 0, 0}, 0.5f);
		const auto result = fixture.Run(graphics);
		CAPTURE(delta);
		CAPTURE(distance);
		CAPTURE(result.x);
		CAPTURE(result.w);
		CHECK(result.x > 0.99f);
		CHECK(result.y == 0);
		CHECK(result.w > 0);
		fixture.Distance = distance * 0.5f;
		CHECK(fixture.Run(graphics) == glm::vec4(0));
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: roughness filters covered source texels and rejects a different depth surface")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		TraceFixture fixture(8);
		fixture.Receiver = {1, 4};
		fixture.Target = {3, 4};
		fixture.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 100.0f, 0.1f);
		fixture.Position = {-0.625f, -0.125f, -1.5f};
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		// Keep the analytic intersection at the bright center texel: this isolates filtering from slab refinement.
		const auto target = origin + fixture.Direction * ((-0.125f - origin.x) / fixture.Direction.x);
		for (uint32_t y = 3; y <= 5; ++y)
			for (uint32_t x = 2; x <= 4; ++x)
				fixture.Surface({x, y}, fixture.ProjectDepth(target), {0, 0, 1}, {x == 3 && y == 4 ? 1.0f : 0.0f, 0, 0}, 0.5f);
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		CHECK(fixture.Run(graphics).x == doctest::Approx(1.0).epsilon(0.001));
		glm::vec4 details;
		const auto inspected = fixture.Run(graphics, &details);
		CHECK(glm::vec2(inspected) == glm::vec2(3, 4));
		CHECK(details.x == doctest::Approx(0.0).epsilon(0.0001));
		fixture.Guides[fixture.Receiver.y * fixture.Size + fixture.Receiver.x].z = 0.5f;
		CHECK(fixture.Run(graphics).x == doctest::Approx(0.25).epsilon(0.001));
		fixture.Surface({4, 4}, fixture.ProjectDepth({0, 0, -0.2f}), {0, 0, 1}, {0, 1, 0}, 0.5f);
		const auto filtered = fixture.Run(graphics);
		CHECK(filtered.x == doctest::Approx(4.0 / 14).epsilon(0.001));
		CHECK(filtered.y == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: approximate depth hits fade continuously while exact crossings retain energy")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (float sign : {-1.0f, 1.0f})
	{
		TraceFixture fixture(8);
		fixture.Receiver = {1, 4};
		fixture.Target = {3, 4};
		fixture.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 100.0f, 0.1f);
		fixture.Position = {-0.625f, -0.125f, -1.5f};
		fixture.Direction = glm::normalize(glm::vec3(1, 0, sign));
		fixture.Normal = glm::normalize(glm::vec3(0, 0, 1) + fixture.Direction);
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		const auto boundary = origin + fixture.Direction * (-origin.x / fixture.Direction.x);
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		const std::array<float, 5> offsets{0, 0.05f, 0.09f, 0.1f, 0.11f};
		const std::array<double, 5> confidence{1, 0.5, 0.028, 0, 0};
		for (size_t i = 0; i < offsets.size(); ++i)
		{
			CAPTURE(sign);
			CAPTURE(offsets[i]);
			// The target lies this known view-depth distance beyond the outgoing cell boundary.
			// Reversing the ray's depth direction exercises both residual signs without changing coverage.
			fixture.Surface(fixture.Target, fixture.ProjectDepth(boundary + glm::vec3(0, 0, sign * offsets[i])), {0, 0, -sign}, {1, 0, 0}, 0.5f);
			const auto result = fixture.Run(graphics);
			CAPTURE(result.x);
			CAPTURE(result.y);
			CAPTURE(result.z);
			CAPTURE(result.w);
			CHECK(std::isfinite(result.w));
			CHECK(result.w >= 0);
			CHECK(result.w <= 1);
			CHECK(result.w == doctest::Approx(confidence[i]).epsilon(0.001));
			if (i < 3)
			{
				CHECK(result.x == doctest::Approx(1.0).epsilon(0.001));
				CHECK(result.y == 0);
				glm::vec4 details;
				const auto inspected = fixture.Run(graphics, &details);
				CHECK(details.x == doctest::Approx(double(sign * offsets[i])).epsilon(0.0001));
				CHECK(details.z == 5);
				CHECK(inspected.w == result.w);
			}
			else if (i == 3)
			{
				glm::vec4 details;
				fixture.Run(graphics, &details);
				CAPTURE(details.x);
				CAPTURE(details.z);
				// The inverse projection can place the nominal boundary just inside the slab.
				// RGB is unpremultiplied; bound the displayed contribution to far below one 8-bit step.
				CHECK(result.w <= 1e-5f);
				for (uint32_t channel = 0; channel < 3; ++channel)
				{
					CHECK(std::isfinite(result[channel]));
					CHECK(std::abs(result[channel] * result.w) <= 1e-5f);
				}
			}
			else
				CHECK(result == glm::vec4(0));
		}
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: rejected overlapping cells advance within the fixed budget and preserve nearest ownership")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (bool gap : {false, true})
	{
		CAPTURE(gap);
		TraceFixture fixture(128);
		fixture.Receiver = {16, 64};
		fixture.Target = {100, 64};
		fixture.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 100.0f, 0.1f);
		fixture.Position = {(16.5f / 128.0f) * 2 - 1, -0.5f / 64, -2};
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		for (uint32_t x = 18; x <= 110; ++x)
		{
			if ((gap && x >= 60 && x < 80) || (x > 100 && x < 110))
				continue;
			const float worldX = (float(x) + 0.5f) / 64 - 1;
			const auto point = origin + fixture.Direction * ((worldX - origin.x) / fixture.Direction.x);
			fixture.Surface({x, 64}, fixture.ProjectDepth(point), x >= 100 ? glm::vec3(0, 0, 1) : glm::vec3(0, 0, -1),
							x == 110 ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), 0.5f);
		}
		glm::vec4 details;
		fixture.Steps = 16;
		CHECK(fixture.Run(graphics).w == 0);
		fixture.Run(graphics, &details);
		CHECK(details.z == 2);
		fixture.Steps = 96;
		const auto result = fixture.Run(graphics);
		CHECK(result.x > 0.99f);
		CHECK(result.y == 0);
		CHECK(result.w > 0.99f);
		const auto inspected = fixture.Run(graphics, &details);
		CHECK(details.z == 5);
		CHECK(inspected.x == float(fixture.Target.x));
		const float targetX = (100.5f / 64) - 1;
		CHECK(inspected.z == doctest::Approx(double((targetX - origin.x) / fixture.Direction.x)).epsilon(0.0001));
		// The second eligible source must be reachable but cannot replace the nearer red hit.
		fixture.Guides[64 * fixture.Size + 100].z = 0;
		fixture.Steps = 256;
		const auto farther = fixture.Run(graphics);
		CHECK(farther.x == 0);
		CHECK(farther.y > 0.99f);
		CHECK(farther.w > 0.99f);
		CHECK(fixture.Run(graphics, &details).x == 110);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: local refinement preserves foreground ownership across coverage plane and normal discontinuities")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (float sign : {-1.0f, 1.0f})
		for (uint32_t kind = 0; kind < 4; ++kind)
		{
			CAPTURE(kind);
			CAPTURE(sign);
			TraceFixture fixture(8);
			fixture.Receiver = {1, 4};
			fixture.Target = {3, 4};
			fixture.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 100.0f, 0.1f);
			fixture.Position = {-0.625f, -0.125f, -1.5f};
			fixture.Direction = glm::normalize(glm::vec3(1, 0, sign));
			fixture.Normal = glm::normalize(glm::vec3(0, 0, 1) + fixture.Direction);
			const auto origin = fixture.Position + fixture.Normal * 0.01f;
			const auto boundary = origin + fixture.Direction * (-origin.x / fixture.Direction.x);
			const auto nearPoint = boundary + glm::vec3(0, 0, sign * 0.05f);
			fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
			fixture.Surface(fixture.Target, fixture.ProjectDepth(nearPoint), {0, 0, -sign}, {1, 0, 0}, 0.5f);
			const glm::uvec2 farther{kind == 1 ? 5u : 4u, 4};
			const auto otherPoint = kind == 0 || kind == 3 ? nearPoint : boundary + fixture.Direction * (kind == 1 ? 0.375f : 0.125f) / fixture.Direction.x;
			fixture.Surface(farther, fixture.ProjectDepth(otherPoint), kind == 3 ? glm::normalize(glm::vec3(-1, 0, -sign)) : glm::vec3(0, 0, -sign),
							kind == 0 ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0), 0.5f);
			const auto result = fixture.Run(graphics);
			CHECK(result.x == doctest::Approx(1.0).epsilon(0.001));
			CHECK(result.y == 0);
			CHECK(result.w == doctest::Approx(kind == 0 ? 1.0 : 0.5).epsilon(0.001));
			glm::vec4 details;
			const auto inspection = fixture.Run(graphics, &details);
			CHECK(inspection.x == float(kind == 0 ? farther.x : fixture.Target.x));
			CHECK(details.z == 5);
			glm::vec4 termination;
			fixture.Run(graphics, &termination, 1);
			const std::array<float, 4> reasons{1, 2, 4, 3};
			CHECK(termination.x == reasons[kind]);
			CHECK(termination.y == 1);
			CHECK(glm::vec2(termination.z, termination.w) == glm::vec2(4, 4));
			glm::vec4 boundaryGeometry;
			const auto candidateGeometry = fixture.Run(graphics, &boundaryGeometry, 2);
			CHECK(candidateGeometry.x == 0);
			CHECK(candidateGeometry.y == 0);
			CHECK(candidateGeometry.z == -sign);
			if (kind == 1)
			{
				CHECK(std::isnan(candidateGeometry.w));
				CHECK(std::isnan(boundaryGeometry.x));
				CHECK(std::isnan(boundaryGeometry.w));
			}
			else
			{
				CHECK(candidateGeometry.w == doctest::Approx(kind == 3 ? std::sqrt(0.5) : 1.0).epsilon(0.0001));
				CHECK(boundaryGeometry.w == doctest::Approx(kind == 2 ? -0.075 : 0.0).epsilon(0.001));
				CHECK(glm::length(glm::vec3(boundaryGeometry)) == doctest::Approx(1.0).epsilon(0.0001));
			}
			if (kind == 3)
			{
				fixture.Normals[fixture.Target.y * fixture.Size + fixture.Target.x].z = std::numeric_limits<float>::quiet_NaN();
				const auto invalidOrientation = fixture.Run(graphics, &boundaryGeometry, 2);
				CHECK(std::isnan(invalidOrientation.x));
				CHECK(std::isnan(invalidOrientation.w));
				CHECK(std::isfinite(boundaryGeometry.x));
				CHECK(std::isnan(boundaryGeometry.w));
			}
			if (kind != 0)
			{
				// Remove only the foreground candidate: each otherwise-blocked farther source is a real exact hit.
				fixture.Guides[fixture.Target.y * fixture.Size + fixture.Target.x].z = 0;
				const auto unblocked = fixture.Run(graphics);
				CHECK(unblocked.x == 0);
				CHECK(unblocked.y > 0.99f);
				CHECK(unblocked.w > 0.99f);
				CHECK(fixture.Run(graphics, &details).x == float(farther.x));
			}
		}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: refinement cell and global iteration caps retain the first approximate candidate")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		TraceFixture fixture(128);
		fixture.Receiver = {16, 64};
		fixture.Target = {32, 64};
		fixture.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 100.0f, 0.1f);
		fixture.Position = {(16.5f / 128.0f) * 2 - 1, -0.5f / 64, -2};
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		const float boundaryX = 33.0f / 64 - 1;
		const auto boundary = origin + fixture.Direction * ((boundaryX - origin.x) / fixture.Direction.x);
		const float depth = fixture.ProjectDepth(boundary - glm::vec3(0, 0, 0.09f));
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		for (uint32_t x = 32; x <= 45; ++x)
			fixture.Surface({x, 64}, depth, {0, 0, 1}, {1, 0, 0}, 0.5f);
		glm::vec4 details;
		const auto capped = fixture.Run(graphics, &details);
		CHECK(capped.x == 32);
		CHECK(capped.w == doctest::Approx(0.028).epsilon(0.001));
		CHECK(details.z == 5);
		glm::vec4 termination;
		fixture.Run(graphics, &termination, 1);
		CHECK(termination.x == 5);
		CHECK(termination.y == 4);
		CHECK(termination.z == 37);
		fixture.Steps = 16;
		const auto exhausted = fixture.Run(graphics, &details);
		CHECK(exhausted.x == 32);
		CHECK(exhausted.w == capped.w);
		CHECK(details.z == 6);
		fixture.Run(graphics, &termination, 1);
		CHECK(termination.x == 7);
		CHECK(termination.y <= 4);
		fixture.Steps = 256;
		for (uint32_t x = 32; x < 35; ++x)
			fixture.Guides[64 * fixture.Size + x].z = 0;
		const auto reachable = fixture.Run(graphics, &details);
		CHECK(reachable.x == 38);
		CHECK(reachable.w > 0.99f);
		CHECK(details.z == 5);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

TEST_CASE("ScreenSpaceReflections: hypothetical candidate planes reject parallel and behind ray intersections")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	{
		TraceFixture fixture(8);
		fixture.Receiver = {1, 4};
		fixture.Target = {3, 4};
		fixture.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 100.0f, 0.1f);
		fixture.Position = {-0.625f, -0.125f, -1.5f};
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		const auto boundary = origin + fixture.Direction * (-origin.x / fixture.Direction.x);
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		for (const auto normal : {glm::normalize(glm::vec3(1, 0, 1)), glm::normalize(glm::vec3(-1.03f, 0, -1))})
		{
			const auto point = boundary - glm::vec3(0, 0, 0.05f);
			fixture.Surface(fixture.Target, fixture.ProjectDepth(point), normal, {1, 0, 0}, 0.5f);
			const float denominator = glm::dot(normal, fixture.Direction);
			const glm::vec3 center{-0.125f, -0.125f, point.z};
			CHECK((std::abs(denominator) < 1e-6f || glm::dot(center - origin, normal) / denominator < 0));
			glm::vec4 termination;
			const auto candidate = fixture.Run(graphics, &termination);
			REQUIRE(glm::vec2(candidate) == glm::vec2(fixture.Target));
			REQUIRE(std::isfinite(candidate.z));
			const auto plane = fixture.Run(graphics, &termination, 1);
			CHECK(std::isnan(plane.x));
			CHECK(std::isnan(plane.z));
			CHECK((static_cast<uint32_t>(plane.w) & 15u) == 0);
			CHECK((static_cast<uint32_t>(plane.w) & 32u) == 0);
			if (denominator < -0.01f)
			{
				const auto fallback = fixture.Run(graphics);
				CHECK(fallback.x == doctest::Approx(1.0).epsilon(0.001));
				CHECK(fallback.w == doctest::Approx(0.5).epsilon(0.001));
			}
		}
		fixture.Surface(fixture.Target, fixture.ProjectDepth(boundary - glm::vec3(0, 0, 0.05f)), glm::normalize(glm::vec3(1, 0, 1.03f)), {1, 0, 0}, 0.5f);
		fixture.Distance = 2;
		glm::vec4 termination;
		const auto candidate = fixture.Run(graphics, &termination);
		REQUIRE(glm::vec2(candidate) == glm::vec2(fixture.Target));
		REQUIRE(std::isfinite(candidate.z));
		const auto distantPlane = fixture.Run(graphics, &termination, 1);
		CHECK(std::isfinite(distantPlane.z));
		CHECK(distantPlane.z > fixture.Distance);
		CHECK((static_cast<uint32_t>(distantPlane.w) & 1u) != 0);
		CHECK((static_cast<uint32_t>(distantPlane.w) & 4u) == 0);
		CHECK((static_cast<uint32_t>(distantPlane.w) & 32u) == 0);
		const auto fallback = fixture.Run(graphics);
		CHECK(fallback.x == doctest::Approx(1.0).epsilon(0.001));
		CHECK(fallback.w == doctest::Approx(0.5).epsilon(0.001));
		fixture.Guides[fixture.Target.y * fixture.Size + fixture.Target.x].x = std::numeric_limits<float>::quiet_NaN();
		const auto invalidCandidate = fixture.Run(graphics, &termination);
		REQUIRE(glm::vec2(invalidCandidate) == glm::vec2(fixture.Target));
		REQUIRE(std::isfinite(invalidCandidate.z));
		const auto nonfinitePlane = fixture.Run(graphics, &termination, 1);
		CHECK(std::isnan(nonfinitePlane.z));
		CHECK((static_cast<uint32_t>(nonfinitePlane.w) & 1u) == 0);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}

namespace {

	void CheckConnectedFacets(bool perspective, float horizontal, float depthSign)
	{
		if (!Testing::RequireTestGraphicsDevice())
			return;
		auto& graphics = *Testing::GetTestGraphicsDevice();
		const auto errors = graphics.GetValidationErrorCount();
		for (uint32_t kind = 0; kind < 14; ++kind)
		{
			// Connected bend, true nearer occluder, disconnected bend, depth step, gap, backface, parallel view plane.
			CAPTURE(perspective);
			CAPTURE(horizontal);
			CAPTURE(depthSign);
			CAPTURE(kind);
			TraceFixture fixture(8);
			fixture.Receiver = {horizontal > 0 ? 0u : 7u, 4};
			fixture.Target = {horizontal > 0 ? 3u : 4u, 4};
			const glm::uvec2 next{horizontal > 0 ? 4u : 3u, 4};
			fixture.Projection = perspective ? glm::perspectiveRH_ZO(2 * std::atan(0.04f), 1.0f, 100.0f, 0.1f)
											 : glm::orthoRH_ZO(-0.08f, 0.08f, -0.08f, 0.08f, 100.0f, 0.1f);
			fixture.Position = {-horizontal * 0.07f, -0.01f, -2};
			fixture.Direction = glm::normalize(glm::vec3(horizontal, 0, depthSign));
			const auto view = perspective ? glm::normalize(-fixture.Position) : glm::vec3(0, 0, 1);
			fixture.Normal = glm::normalize(view + fixture.Direction);
			const auto origin = fixture.Position + fixture.Normal * 0.01f;
			const auto edge = origin + fixture.Direction * (-origin.x / fixture.Direction.x);
			const glm::vec3 firstNormal{0, 0, -depthSign};
			const auto sharedPoint = edge + glm::vec3(0, 0, depthSign * (kind == 13 ? 0.055f : kind == 7 || kind >= 9 ? 0.03f
																													  : 0.005f));
			const auto firstPoint = kind == 1 ? edge - glm::vec3(0, 0, depthSign * 0.005f) : sharedPoint;
			const auto inverse = glm::inverse(glm::dmat4(fixture.Projection));
			auto cameraRay = [&](glm::dvec2 pixel) {
				auto unproject = [&](double depth) {
					const auto point = inverse * glm::dvec4(pixel.x / 4 - 1, 1 - pixel.y / 4, depth, 1);
					return glm::dvec3(point) / point.w;
				};
				const auto near = unproject(1);
				return std::array<glm::dvec3, 2>{near, glm::normalize(unproject(0.5) - near)};
			};
			auto planePoint = [&](glm::dvec2 pixel, glm::vec3 point, glm::vec3 normal) {
				const auto ray = cameraRay(pixel);
				const auto n = glm::dvec3(normal);
				return ray[0] + ray[1] * (glm::dot(glm::dvec3(point) - ray[0], n) / glm::dot(ray[1], n));
			};
			const float slope = -depthSign * horizontal * std::tan(glm::radians(kind == 2 ? 45.0f : kind == 13 ? 11.0f
																											   : 9.0f));
			auto nextNormal = glm::normalize(glm::vec3(slope * depthSign, 0, -depthSign));
			auto nextPoint = sharedPoint;
			if (kind == 2 || kind == 6)
				nextPoint = glm::vec3(planePoint(glm::dvec2(next) + 0.5, sharedPoint, firstNormal));
			if (kind == 3)
				nextPoint.z += depthSign * 0.03f;
			if (kind == 5)
				nextNormal = -nextNormal;
			if (kind == 6)
				nextNormal = glm::normalize(glm::vec3(-horizontal, 0, -depthSign * 1e-7f));
			const auto firstCenter = glm::vec3(planePoint(glm::dvec2(fixture.Target) + 0.5, firstPoint, firstNormal));
			const auto nextCenter = glm::vec3(planePoint(glm::dvec2(next) + 0.5, nextPoint, nextNormal));
			fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
			fixture.Surface(fixture.Target, fixture.ProjectDepth(firstCenter), firstNormal, {1, 0, 0}, 0.5f);
			if (kind != 4)
				fixture.Surface(next, fixture.ProjectDepth(nextCenter), nextNormal, {0, 1, 0}, 0.5f);
			if (kind == 8)
				fixture.Guides[next.y * fixture.Size + next.x].x = std::numeric_limits<float>::quiet_NaN();
			const glm::uvec2 later{horizontal > 0 ? 5u : 2u, 4};
			const glm::uvec2 beyond{horizontal > 0 ? 6u : 1u, 4};
			if (kind == 7 || kind >= 9)
			{
				const auto center = glm::vec3(planePoint(glm::dvec2(later) + 0.5, nextPoint, nextNormal));
				const auto laterNormal = kind == 9 ? glm::normalize(glm::vec3(-horizontal * std::tan(glm::radians(18.0f)), 0, -depthSign)) : nextNormal;
				fixture.Surface(later, fixture.ProjectDepth(center + glm::vec3(0, 0, kind == 10 ? depthSign * 0.01f : 0)), laterNormal, {0, 0, 1}, 0.5f);
				if (kind == 11)
					fixture.Guides[later.y * fixture.Size + later.x].z = 0;
				if (kind == 13)
				{
					const auto beyondCenter = glm::vec3(planePoint(glm::dvec2(beyond) + 0.5, nextPoint, nextNormal));
					fixture.Surface(beyond, fixture.ProjectDepth(beyondCenter), nextNormal, {0, 0, 1}, 0.5f);
				}
				if (kind == 12)
					fixture.Distance = -origin.x / fixture.Direction.x + 0.02f;
			}

			uint32_t primaryBoundVisit = 0;
			glm::uvec2 primaryBoundCell{0};
			if (kind == 13)
			{
				// Perspective world-space cell widths vary with depth; derive the first failed bound geometrically.
				const std::array<glm::uvec2, 3> cells{next, later, beyond};
				for (uint32_t i = 0; i < cells.size(); ++i)
				{
					const auto center = planePoint(glm::dvec2(cells[i]) + 0.5, nextPoint, nextNormal);
					const double deviation = std::abs(glm::dot(center - glm::dvec3(firstCenter), glm::dvec3(firstNormal)));
					if (deviation > 0.005)
					{
						REQUIRE(deviation > 0.0052);
						primaryBoundVisit = i + 1;
						primaryBoundCell = cells[i];
						break;
					}
				}
				REQUIRE(primaryBoundVisit >= 2);
				REQUIRE(primaryBoundVisit <= 3);
			}
			if (kind == 0)
				REQUIRE(glm::dot(firstNormal, nextNormal) < 0.99f);

			if (kind <= 1 || kind == 7)
			{
				// Intersect finite triangles independently of the tracer's depth slab and continuity rules.
				double nearest = std::numeric_limits<double>::infinity();
				uint32_t owner = 3;
				for (uint32_t face = 0; face < (kind == 7 ? 3u : 2u); ++face)
				{
					const auto pixel = glm::dvec2(face == 0 ? fixture.Target : face == 1 ? next
																						 : later);
					const auto point = face == 0 ? firstPoint : nextPoint;
					const auto normal = face == 0 ? firstNormal : nextNormal;
					const std::array<glm::dvec3, 4> quad{planePoint(pixel, point, normal), planePoint(pixel + glm::dvec2(1, 0), point, normal),
														 planePoint(pixel + glm::dvec2(1, 1), point, normal), planePoint(pixel + glm::dvec2(0, 1), point, normal)};
					for (uint32_t triangle = 1; triangle <= 2; ++triangle)
					{
						const auto e1 = quad[triangle] - quad[0], e2 = quad[triangle + 1] - quad[0];
						const auto h = glm::cross(glm::dvec3(fixture.Direction), e2);
						const double determinant = glm::dot(e1, h);
						if (std::abs(determinant) <= 1e-12)
							continue;
						const auto relative = glm::dvec3(origin) - quad[0], q = glm::cross(relative, e1);
						const double u = glm::dot(relative, h) / determinant, v = glm::dot(glm::dvec3(fixture.Direction), q) / determinant;
						const double distance = glm::dot(e2, q) / determinant;
						if (u >= 0 && v >= 0 && u + v <= 1 && distance > 0 && distance < nearest)
						{
							nearest = distance;
							owner = face;
						}
					}
				}
				REQUIRE(owner == (kind == 0 ? 1u : kind == 7 ? 2u
															 : 0u));
				REQUIRE(std::isfinite(nearest));
				glm::vec4 details;
				const auto inspected = fixture.Run(graphics, &details);
				CHECK(inspected.x == float(owner == 0 ? fixture.Target.x : owner == 1 ? next.x
																					  : later.x));
				CHECK(inspected.z == doctest::Approx(nearest).epsilon(0.002));
				CHECK(inspected.w > 0.99f);
				CHECK(details.z == 5);
				const auto color = fixture.Run(graphics);
				CHECK(color.x == doctest::Approx(owner == 0 ? 1.0 : 0.0).epsilon(0.0001));
				CHECK(color.y == doctest::Approx(owner == 1 ? 1.0 : 0.0).epsilon(0.0001));
				CHECK(color.z == doctest::Approx(owner == 2 ? 1.0 : 0.0).epsilon(0.0001));
				const auto plane = fixture.Run(graphics, &details, 1);
				CHECK((static_cast<uint32_t>(plane.w) & 32u) != 0);
				CHECK(details.x == 1);
			}
			else
			{
				glm::vec4 details;
				const auto retained = fixture.Run(graphics, &details);
				CHECK(retained.x == float(fixture.Target.x));
				CHECK(retained.w == doctest::Approx(kind == 13 ? 0.42525 : kind >= 9 ? 0.784
																					 : 0.99275)
										.epsilon(0.002));
				CHECK(retained.z == doctest::Approx(double(-origin.x / fixture.Direction.x)).epsilon(0.002));
				CHECK(details.z == 5);
				const auto plane = fixture.Run(graphics, &details, 1);
				CHECK((static_cast<uint32_t>(plane.w) & 32u) == 0);
				CHECK(details.x == (kind == 4 || kind == 11 ? 2 : kind == 10 || kind == 13 ? 4
															  : kind == 12                 ? 6
																						   : 3));
				CHECK(details.y == (kind == 13 ? primaryBoundVisit : kind >= 9 && kind <= 11 ? 2u
																							 : 1));
				CHECK(glm::vec2(details.z, details.w) == glm::vec2(kind == 13 ? primaryBoundCell : kind >= 9 && kind <= 11 ? later
																														   : next));
				const auto color = fixture.Run(graphics);
				CHECK(color.x == 1);
				CHECK(color.y == 0);
				CHECK(color.z == 0);
				if (kind == 6)
				{
					glm::vec4 geometry;
					fixture.Run(graphics, &geometry, 2);
					const auto storedNormal = glm::vec3(geometry);
					// The stored normal must reach the near-parallel denominator guard, not fail hemisphere agreement first.
					CHECK(glm::dot(storedNormal, firstNormal) > 0);
					CHECK(std::abs(glm::dot(glm::dvec3(storedNormal), cameraRay({4, 4.5})[1])) <= 1e-6);
				}
				if (kind == 13)
				{
					fixture.Guides[fixture.Target.y * fixture.Size + fixture.Target.x].z = 0;
					CHECK(fixture.Run(graphics, &details).x == float(beyond.x));
					CHECK(fixture.Run(graphics).z == 1);
				}
			}
		}
		CHECK(graphics.GetValidationErrorCount() == errors);
	}

}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in orthographic rays moving left with negative view Z direction")
{
	CheckConnectedFacets(false, -1.0f, -1.0f);
}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in orthographic rays moving left with positive view Z direction")
{
	CheckConnectedFacets(false, -1.0f, 1.0f);
}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in orthographic rays moving right with negative view Z direction")
{
	CheckConnectedFacets(false, 1.0f, -1.0f);
}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in orthographic rays moving right with positive view Z direction")
{
	CheckConnectedFacets(false, 1.0f, 1.0f);
}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in perspective rays moving left with negative view Z direction")
{
	CheckConnectedFacets(true, -1.0f, -1.0f);
}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in perspective rays moving left with positive view Z direction")
{
	CheckConnectedFacets(true, -1.0f, 1.0f);
}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in perspective rays moving right with negative view Z direction")
{
	CheckConnectedFacets(true, 1.0f, -1.0f);
}

TEST_CASE("ScreenSpaceReflections: connected facets preserve finite triangle nearest hits in perspective rays moving right with positive view Z direction")
{
	CheckConnectedFacets(true, 1.0f, 1.0f);
}

TEST_CASE("ScreenSpaceReflections: a certified facet cannot reset the cell or global iteration budget")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto errors = graphics.GetValidationErrorCount();
	for (bool cellCap : {false, true})
	{
		CAPTURE(cellCap);
		TraceFixture fixture(128);
		fixture.Receiver = {16, 64};
		fixture.Target = {22, 64};
		fixture.Projection = glm::orthoRH_ZO(-0.5f, 0.5f, -0.5f, 0.5f, 100.0f, 0.1f);
		fixture.Position = {16.5f / 128 - 0.5f, -0.5f / 128, -2};
		const auto origin = fixture.Position + fixture.Normal * 0.01f;
		const float edgeX = 23.0f / 128 - 0.5f;
		const float edgeDistance = (edgeX - origin.x) / fixture.Direction.x;
		const auto edge = origin + fixture.Direction * edgeDistance;
		const auto shared = edge - glm::vec3(0, 0, cellCap ? 0.04f : 0.02f);
		const float slope = std::tan(glm::radians(9.0f));
		const auto normal = glm::normalize(glm::vec3(-slope, 0, 1));
		fixture.Surface(fixture.Receiver, fixture.ProjectDepth(fixture.Position), fixture.Normal, {0, 0, 0}, 0.001f);
		for (uint32_t x = 18; x < 22; ++x)
		{
			const float worldX = (float(x) + 0.5f) / 128 - 0.5f;
			const auto point = origin + fixture.Direction * ((worldX - origin.x) / fixture.Direction.x);
			fixture.Surface({x, 64}, fixture.ProjectDepth(point), {0, 0, -1}, {0, 1, 0}, 0.5f);
		}
		fixture.Surface(fixture.Target, fixture.ProjectDepth(shared), {0, 0, 1}, {1, 0, 0}, 0.5f);
		for (uint32_t x = 23; x <= 30; ++x)
		{
			const float worldX = (float(x) + 0.5f) / 128 - 0.5f;
			const glm::vec3 point{worldX, fixture.Position.y, shared.z + slope * (worldX - edgeX)};
			fixture.Surface({x, 64}, fixture.ProjectDepth(point), normal, {0, 0, 1}, 0.5f);
		}
		const double distance = glm::dot(glm::dvec3(shared) - glm::dvec3(origin), glm::dvec3(normal)) /
								glm::dot(glm::dvec3(fixture.Direction), glm::dvec3(normal));
		const auto hit = glm::dvec3(origin) + glm::dvec3(fixture.Direction) * distance;
		const uint32_t expected = cellCap ? 27u : 25u;
		REQUIRE(std::floor((hit.x + 0.5) * 128) == double(expected));
		fixture.Steps = cellCap ? 256u : 16u;
		glm::vec4 details;
		const auto retained = fixture.Run(graphics, &details);
		CHECK(retained.x == 22);
		CHECK(retained.z == doctest::Approx(double(edgeDistance)).epsilon(0.002));
		CHECK(retained.w == doctest::Approx(cellCap ? 0.648 : 0.896).epsilon(0.002));
		CHECK(details.z == (cellCap ? 5 : 6));
		const auto plane = fixture.Run(graphics, &details, 1);
		CHECK((static_cast<uint32_t>(plane.w) & 32u) == 0);
		CHECK(details.x == (cellCap ? 5 : 7));
		CHECK(details.y == (cellCap ? 4 : 2));
		CHECK(details.z == (cellCap ? 27 : 24));
		const auto color = fixture.Run(graphics);
		CHECK(color.x == 1);
		CHECK(color.z == 0);
		// Removing only the first candidate buys one cell; increasing only iterations buys no cells.
		if (cellCap)
			fixture.Guides[64 * fixture.Size + fixture.Target.x].z = 0;
		fixture.Steps = 256;
		const auto reachable = fixture.Run(graphics, &details);
		CHECK(reachable.x == float(expected));
		CHECK(reachable.z == doctest::Approx(distance).epsilon(0.002));
		CHECK(reachable.w > 0.99f);
		const auto reachedColor = fixture.Run(graphics);
		CHECK(reachedColor.x == 0);
		CHECK(reachedColor.z == 1);
	}
	CHECK(graphics.GetValidationErrorCount() == errors);
}
