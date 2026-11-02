#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/ImGui/ImGuiRenderer.h>
#include <Paralax/Renderer/GraphicsDevice.h>

#include <imgui.h>

using namespace Paralax;

TEST_CASE("ImGuiRenderer: constructing it without a device is an asserted precondition")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(ImGuiRenderer(nullptr), Paralax::Testing::AssertionFailedError);
}

// Render's other verified precondition, that ImGui's textures are RGBA32, is checked while the textures are uploaded,
// after Render has opened its command list. A test that failed it would leave that list open and its command buffer
// alive past the device, so it is not tested; the precondition holds for every texture ImGui itself creates.
TEST_CASE("ImGuiRenderer: rendering without draw data or a framebuffer is an asserted precondition")
{
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	GraphicsDevice& graphics = *Paralax::Testing::GetTestGraphicsDevice();
	const uint32_t initialErrors = graphics.GetValidationErrorCount();
	{
		nvrhi::IDevice* device = graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = 16;
		desc.height = 16;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.debugName = "ImGuiRendererTestTarget";
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		nvrhi::TextureHandle target = device->createTexture(desc);
		REQUIRE(target);
		nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		REQUIRE(framebuffer);

		// The verify comes before anything is recorded, so the refused calls leave nothing behind.
		ImGuiRenderer renderer(device);
		ImDrawData drawData;
		Paralax::Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(renderer.Render(nullptr, framebuffer), Paralax::Testing::AssertionFailedError);
		CHECK_THROWS_AS(renderer.Render(&drawData, nullptr), Paralax::Testing::AssertionFailedError);
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

// A device cannot be made to refuse an allocation without validation errors, so the bookkeeping the renderer logs
// through is tested on its own.
TEST_CASE("ImGuiTextureFailures: a texture that keeps failing to be created is reported once until it is created or destroyed")
{
	ImGuiTextureFailures failures;
	// ImGui asks again every frame: three failing frames, one report.
	CHECK(failures.Failed(7));
	CHECK_FALSE(failures.Failed(7));
	CHECK_FALSE(failures.Failed(7));
	// Another texture is reported on its own.
	CHECK(failures.Failed(8));
	CHECK_FALSE(failures.Failed(8));

	// Created at last: a later failure is new, and reported.
	failures.Created(7);
	CHECK(failures.GetCount() == 1);
	CHECK(failures.Failed(7));
	CHECK_FALSE(failures.Failed(7));

	// Destroyed likewise; Shutdown forgets them all.
	failures.Destroyed(7);
	CHECK(failures.Failed(7));
	CHECK(failures.GetCount() == 2);
	failures.Clear();
	CHECK(failures.GetCount() == 0);
	CHECK(failures.Failed(8));
}
