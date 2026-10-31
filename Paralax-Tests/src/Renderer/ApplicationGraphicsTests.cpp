#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Core/Application.h>
#include <Paralax/Renderer/TextureReadback.h>

using namespace Paralax;

namespace {

	ApplicationSpecification GraphicsSpecification(uint64_t frames)
	{
		ApplicationSpecification specification;
		specification.Name = "Graphics Test";
		specification.Window.Width = 48;
		specification.Window.Height = 32;
		specification.Window.Headless = true;
		specification.MaxFrames = frames;
		specification.FixedTimestep = 1.0f / 60.0f;
		specification.Graphics.Enabled = true;
		specification.Graphics.Validation = true;
		return specification;
	}

	// Clears the back buffer to a fixed linear color each frame and captures the last frame.
	class ClearAndCaptureLayer : public Layer
	{
	public:
		void OnAttach() override { m_CommandList = Application::Get().GetGraphicsDevice().GetDevice()->createCommandList(); }

		void OnRender() override
		{
			Application& app = Application::Get();
			nvrhi::IDevice* device = app.GetGraphicsDevice().GetDevice();
			m_CommandList->open();
			m_CommandList->clearTextureFloat(app.GetSwapchain().GetCurrentBackBuffer(), nvrhi::AllSubresources, nvrhi::Color(0.5f, 0.0f, 1.0f, 1.0f));
			m_CommandList->close();
			device->executeCommandList(m_CommandList);

			RenderCount++;
			if (RenderCount == app.GetSpecification().MaxFrames)
				LastFrame = ReadbackTexture(device, app.GetSwapchain().GetCurrentBackBuffer());
		}

		void OnDetach() override { m_CommandList = nullptr; }

		uint64_t RenderCount = 0;
		Image LastFrame;

	private:
		nvrhi::CommandListHandle m_CommandList;
	};

}

TEST_CASE("Application graphics: a headless application renders every frame into an sRGB back buffer")
{
	// Only one GraphicsDevice may exist; the application creates its own.
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	Application app(GraphicsSpecification(5));
	REQUIRE(app.IsInitialized());
	REQUIRE(app.HasGraphics());
	CHECK(app.GetSwapchain().GetWidth() == 48);
	CHECK(app.GetSwapchain().GetHeight() == 32);

	auto* layer = app.PushLayer<ClearAndCaptureLayer>();
	app.Run();

	CHECK(app.GetRenderedFrameCount() == 5);
	CHECK(layer->RenderCount == 5);
	CHECK(app.GetExitCode() == 0);
	CHECK(app.GetGraphicsDevice().GetValidationErrorCount() == 0);

	REQUIRE_FALSE(layer->LastFrame.IsEmpty());
	layer->LastFrame.SavePNG(Paralax::Testing::GetTestOutputDirectory() / "ApplicationClear.png");
	// Linear 0.5 is stored sRGB-encoded as ~188.
	const glm::u8vec4 pixel = layer->LastFrame.GetPixel(10, 10);
	CHECK(std::abs(int(pixel.r) - 188) <= 2);
	CHECK(pixel.g == 0);
	CHECK(pixel.b == 255);
}

TEST_CASE("Application graphics: disabled graphics never calls OnRender and has no device")
{
	class RenderCounter : public Layer
	{
	public:
		void OnRender() override { Count++; }
		int Count = 0;
	};

	ApplicationSpecification specification = GraphicsSpecification(3);
	specification.Graphics.Enabled = false;
	Application app(specification);
	auto* layer = app.PushLayer<RenderCounter>();
	app.Run();

	CHECK_FALSE(app.HasGraphics());
	CHECK(layer->Count == 0);
	CHECK(app.GetRenderedFrameCount() == 0);

	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(app.GetGraphicsDevice(), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(app.GetSwapchain(), Paralax::Testing::AssertionFailedError);
}

TEST_CASE("Application graphics: a minimized window skips rendering")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	class RenderCounter : public Layer
	{
	public:
		void OnRender() override { Count++; }
		int Count = 0;
	};

	Application app(GraphicsSpecification(4));
	REQUIRE(app.IsInitialized());
	auto* layer = app.PushLayer<RenderCounter>();
	WindowMinimizeEvent minimize(true);
	app.OnEvent(minimize);
	app.Run();

	CHECK(layer->Count == 0);
	CHECK(app.GetRenderedFrameCount() == 0);
}

TEST_CASE("ApplyCommandLineOptions: --validation and --gpu configure graphics")
{
	ApplicationSpecification specification;
	specification.CommandLineArgs = CommandLine({"--validation", "--gpu", "llvmpipe"}, GetStandardFlagOptions());
	REQUIRE(ApplyCommandLineOptions(specification));
	CHECK(specification.Graphics.Validation);
	CHECK(specification.Graphics.FailOnValidationErrors);
	CHECK(specification.Graphics.PreferredAdapter == "llvmpipe");
}
