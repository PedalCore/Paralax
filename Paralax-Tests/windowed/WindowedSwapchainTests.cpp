#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/Application.h>
#include <Paralax/Renderer/TextureReadback.h>

// Runs with real (non-headless) windows and a real Vulkan swapchain; registered with CTest under xvfb-run.

using namespace Paralax;

namespace {

	// Clears to a fixed color, resizes the window every few frames and toggles vsync, forcing many swapchain
	// recreations, then captures the final back buffer.
	class SwapchainStressLayer : public Layer
	{
	public:
		void OnAttach() override { m_CommandList = Application::Get().GetGraphicsDevice().GetDevice()->createCommandList(); }
		void OnDetach() override { m_CommandList = nullptr; }

		void OnUpdate(Timestep) override
		{
			Application& app = Application::Get();
			m_Frame++;
			if (m_Frame % 7 == 0)
			{
				const uint32_t width = 200 + (m_Frame * 37) % 300;
				const uint32_t height = 150 + (m_Frame * 53) % 200;
				app.GetWindow().SetSize(width, height);
				Resizes++;
			}
			if (m_Frame % 45 == 0)
				app.GetSwapchain().SetVSync(!app.GetSwapchain().IsVSync());
		}

		void OnRender() override
		{
			Application& app = Application::Get();
			nvrhi::IDevice* device = app.GetGraphicsDevice().GetDevice();
			nvrhi::ITexture* backBuffer = app.GetSwapchain().GetCurrentBackBuffer();
			m_CommandList->open();
			m_CommandList->clearTextureFloat(backBuffer, nvrhi::AllSubresources, nvrhi::Color(0.0f, 1.0f, 0.0f, 1.0f));
			m_CommandList->close();
			device->executeCommandList(m_CommandList);

			Rendered++;
			LastWidth = app.GetSwapchain().GetWidth();
			LastHeight = app.GetSwapchain().GetHeight();
			if (app.GetFrameCount() + 1 == app.GetSpecification().MaxFrames)
				LastFrame = ReadbackTexture(device, backBuffer);
		}

		int Resizes = 0;
		uint64_t Rendered = 0;
		uint32_t LastWidth = 0;
		uint32_t LastHeight = 0;
		Image LastFrame;

	private:
		nvrhi::CommandListHandle m_CommandList;
		uint32_t m_Frame = 0;
	};

}

TEST_CASE("Windowed swapchain: survives repeated resizes and vsync changes without validation errors")
{
	ApplicationSpecification specification;
	specification.Name = "Windowed Swapchain Test";
	specification.Window.Width = 320;
	specification.Window.Height = 240;
	specification.MaxFrames = 300;
	specification.FixedTimestep = 1.0f / 60.0f;
	specification.Graphics.Enabled = true;
	specification.Graphics.Validation = true;
	specification.Graphics.FailOnValidationErrors = true;

	Application app(specification);
	REQUIRE(app.IsInitialized());
	REQUIRE_FALSE(app.GetWindow().IsHeadless());

	auto* layer = app.PushLayer<SwapchainStressLayer>();
	app.Run();

	CHECK(layer->Resizes >= 40);
	// A frame may be skipped while the swapchain is recreated, but nearly all must render.
	CHECK(layer->Rendered >= 290);
	CHECK(app.GetGraphicsDevice().GetValidationErrorCount() == 0);
	CHECK(app.GetExitCode() == 0);

	uint32_t width = 0, height = 0;
	app.GetWindow().GetFramebufferSize(width, height);
	CHECK(layer->LastWidth == width);
	CHECK(layer->LastHeight == height);

	REQUIRE_FALSE(layer->LastFrame.IsEmpty());
	CHECK(layer->LastFrame.GetPixel(layer->LastFrame.GetWidth() / 2, layer->LastFrame.GetHeight() / 2) == glm::u8vec4(0, 255, 0, 255));
}
