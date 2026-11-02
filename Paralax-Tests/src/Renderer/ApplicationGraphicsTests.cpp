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

	// Copies into a buffer and out of it again without the barrier that orders the two, a hazard synchronization
	// validation reports as an error.
	void SubmitSynchronizationHazard(nvrhi::IDevice* device)
	{
		const auto makeBuffer = [&](const char* name, nvrhi::ResourceStates state) {
			return device->createBuffer(nvrhi::BufferDesc().setByteSize(256).setInitialState(state).setKeepInitialState(true).setDebugName(name));
		};
		nvrhi::BufferHandle source = makeBuffer("HazardSource", nvrhi::ResourceStates::CopySource);
		nvrhi::BufferHandle middle = makeBuffer("HazardMiddle", nvrhi::ResourceStates::CopyDest);
		nvrhi::BufferHandle destination = makeBuffer("HazardDestination", nvrhi::ResourceStates::CopyDest);
		REQUIRE(source);
		REQUIRE(middle);
		REQUIRE(destination);

		nvrhi::CommandListHandle commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->setEnableAutomaticBarriers(false);
		commands->copyBuffer(middle, 0, source, 0, 256);
		commands->copyBuffer(destination, 0, middle, 0, 256);
		commands->setEnableAutomaticBarriers(true);
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
	}

	// Causes a validation error on the application's device as it attaches or as it detaches, and optionally
	// requests an exit code as it detaches.
	class ValidationErrorLayer : public Layer
	{
	public:
		enum class Moment
		{
			Attach,
			Detach
		};

		explicit ValidationErrorLayer(Moment moment, int detachExitCode = 0)
			: m_Moment(moment)
			, m_DetachExitCode(detachExitCode)
		{
		}

		void OnAttach() override
		{
			if (m_Moment == Moment::Attach)
				SubmitSynchronizationHazard(Application::Get().GetGraphicsDevice().GetDevice());
		}

		void OnDetach() override
		{
			if (m_DetachExitCode != 0)
				Application::Get().SetExitCode(m_DetachExitCode);
			if (m_Moment == Moment::Detach)
				SubmitSynchronizationHazard(Application::Get().GetGraphicsDevice().GetDevice());
		}

	private:
		Moment m_Moment;
		int m_DetachExitCode;
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

TEST_CASE("Application graphics: Shutdown fails the run with exit code 3 for validation errors, including those raised while layers detach")
{
	// The application creates its own device, which the deliberate errors are confined to.
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireSynchronizationValidation())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	Paralax::Testing::ScopedLogCapture capture;
	ApplicationSpecification specification = GraphicsSpecification(2);
	specification.Graphics.FailOnValidationErrors = true;

	SUBCASE("errors raised while a layer detaches")
	{
		Application app(specification);
		REQUIRE(app.IsInitialized());
		REQUIRE(app.GetGraphicsDevice().GetInfo().SynchronizationValidation);
		app.PushLayer<ValidationErrorLayer>(ValidationErrorLayer::Moment::Detach);
		app.Run();
		CHECK(app.GetExitCode() == 0);

		app.Shutdown();
		CHECK(app.GetGraphicsDevice().GetValidationErrorCount() > 0);
		CHECK(app.GetExitCode() == 3);
		CHECK(capture.Contains(spdlog::level::err, "graphics validation error(s) were reported"));
	}

	SUBCASE("an exit code a layer requests while detaching comes first: the validation check requests last")
	{
		Application app(specification);
		REQUIRE(app.IsInitialized());
		REQUIRE(app.GetGraphicsDevice().GetInfo().SynchronizationValidation);
		app.PushLayer<ValidationErrorLayer>(ValidationErrorLayer::Moment::Attach, 4);
		app.Run();
		app.Shutdown();
		CHECK(app.GetGraphicsDevice().GetValidationErrorCount() > 0);
		CHECK(app.GetExitCode() == 4);
	}

	SUBCASE("without FailOnValidationErrors the errors are reported but do not fail the run")
	{
		specification.Graphics.FailOnValidationErrors = false;
		Application app(specification);
		REQUIRE(app.IsInitialized());
		REQUIRE(app.GetGraphicsDevice().GetInfo().SynchronizationValidation);
		app.PushLayer<ValidationErrorLayer>(ValidationErrorLayer::Moment::Detach);
		app.Run();
		app.Shutdown();
		CHECK(app.GetGraphicsDevice().GetValidationErrorCount() > 0);
		CHECK(app.GetExitCode() == 0);
		CHECK(capture.Contains(spdlog::level::err, "graphics validation error(s) were reported"));
	}
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
