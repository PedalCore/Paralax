#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Core/Application.h>
#include <Paralax/Renderer/Image.h>
#include <Paralax/Renderer/Swapchain.h>

using namespace Paralax;

namespace {

	ApplicationSpecification GraphicsSpecification(uint64_t frames)
	{
		ApplicationSpecification specification;
		specification.Window.Width = 64;
		specification.Window.Height = 48;
		specification.Window.Headless = true;
		specification.MaxFrames = frames;
		specification.FixedTimestep = 1.0f / 60.0f;
		specification.Graphics.Enabled = true;
		return specification;
	}

	class CaptureLayer : public Layer
	{
	public:
		explicit CaptureLayer(std::filesystem::path path)
			: m_Path(std::move(path))
		{
		}

		void OnUpdate(Timestep) override
		{
			if (++m_Frame == 2)
				Application::Get().CaptureNextFrame(m_Path);
		}

	private:
		std::filesystem::path m_Path;
		int m_Frame = 0;
	};

}

TEST_CASE("Application: ScreenshotPath saves the last frame of a frame-limited run")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("Screenshot") / "Last.png";
	ApplicationSpecification specification = GraphicsSpecification(3);
	specification.ScreenshotPath = path;
	Application app(specification);
	REQUIRE(app.IsInitialized());
	app.Run();
	Image image = Image::LoadFromFile(path);
	CHECK(image.GetWidth() == app.GetSwapchain().GetWidth());
	CHECK(image.GetHeight() == app.GetSwapchain().GetHeight());
}

TEST_CASE("Application: CaptureNextFrame saves the next presented frame")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("CaptureNextFrame") / "Frame.png";
	Application app(GraphicsSpecification(4));
	REQUIRE(app.IsInitialized());
	app.PushLayer<CaptureLayer>(path);
	app.Run();
	CHECK(std::filesystem::exists(path));
}

TEST_CASE("Application: a screenshot that cannot be taken is reported at startup")
{
	Paralax::Testing::ScopedLogCapture log;
	ApplicationSpecification specification;
	specification.Window.Headless = true;
	specification.MaxFrames = 1;
	specification.ScreenshotPath = "Never.png";
	{
		Application app(specification); // No graphics.
		CHECK(log.Contains(spdlog::level::err, "graphics are disabled"));
	}
}

TEST_CASE("Application: a screenshot without a frame limit is reported at startup")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	Paralax::Testing::ScopedLogCapture log;
	ApplicationSpecification specification = GraphicsSpecification(0);
	specification.ScreenshotPath = "Never.png";
	Application app(specification);
	REQUIRE(app.IsInitialized());
	CHECK(log.Contains(spdlog::level::err, "without a frame limit"));
}
