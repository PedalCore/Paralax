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
