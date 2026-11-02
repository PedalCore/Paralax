#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Core/Application.h>
#include <Paralax/Events/MouseEvent.h>
#include <Paralax/ImGui/ImGuiLayer.h>
#include <Paralax/ImGui/ImGuiRenderer.h>
#include <Paralax/Renderer/TextureReadback.h>

#include <imgui.h>

using namespace Paralax;

namespace {

	class UILayer : public Layer
	{
	public:
		void OnAttach() override { m_CommandList = Application::Get().GetGraphicsDevice().GetDevice()->createCommandList(); }
		void OnDetach() override { m_CommandList = nullptr; }

		void OnRender() override
		{
			Application& app = Application::Get();
			m_CommandList->open();
			m_CommandList->clearTextureFloat(app.GetSwapchain().GetCurrentBackBuffer(), nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
			m_CommandList->close();
			app.GetGraphicsDevice().GetDevice()->executeCommandList(m_CommandList);
		}

		void OnImGuiRender() override
		{
			ImGui::SetNextWindowPos(ImVec2(10, 10));
			ImGui::SetNextWindowSize(ImVec2(220, 120));
			ImGui::Begin("Paralax");
			ImGui::Text("Hello, world");
			ImGui::Button("Button");
			ImGui::End();
			Frames++;
		}

		int Frames = 0;

	private:
		nvrhi::CommandListHandle m_CommandList;
	};

}

TEST_CASE("ImGuiRenderer: constructing it without a device is an asserted precondition")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(ImGuiRenderer(nullptr), Paralax::Testing::AssertionFailedError);
}

TEST_CASE("ImGuiLayer: renders UI into the back buffer without validation errors")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	ApplicationSpecification specification;
	specification.Window.Width = 320;
	specification.Window.Height = 200;
	specification.Window.Headless = true;
	specification.MaxFrames = 3;
	specification.FixedTimestep = 1.0f / 60.0f;
	specification.Graphics.Enabled = true;
	specification.Graphics.Validation = true;
	specification.ImGui.Enabled = true;

	Application app(specification);
	REQUIRE(app.IsInitialized());
	REQUIRE(app.GetImGuiLayer() != nullptr);
	auto* layer = app.PushLayer<UILayer>();
	app.Run();

	CHECK(layer->Frames == 3);
	CHECK(app.GetGraphicsDevice().GetValidationErrorCount() == 0);

	Image image = ReadbackTexture(app.GetGraphicsDevice().GetDevice(), app.GetSwapchain().GetCurrentBackBuffer());
	REQUIRE_FALSE(image.IsEmpty());
	image.SavePNG(Paralax::Testing::GetTestOutputDirectory() / "ImGui.png");

	// The window area is lighter than the black background; outside it is black.
	uint32_t windowPixels = 0;
	for (uint32_t y = 12; y < 128; y++)
		for (uint32_t x = 12; x < 228; x++)
			windowPixels += image.GetPixel(x, y).r > 10 ? 1 : 0;
	CHECK(windowPixels > 200 * 100);
	CHECK(image.GetPixel(300, 180).r < 5);
}

namespace {

	// Draws a single anti-aliased triangle and nothing else: 21 16-bit indices, 42 bytes.
	class TriangleLayer : public UILayer
	{
	public:
		void OnImGuiRender() override
		{
			ImGui::GetForegroundDrawList()->AddTriangleFilled(ImVec2(0, 0), ImVec2(300, 0), ImVec2(0, 190), IM_COL32(255, 255, 255, 255));
			TotalIndices = ImGui::GetForegroundDrawList()->IdxBuffer.Size;
		}

		int TotalIndices = 0;
	};

}

TEST_CASE("ImGuiLayer: index data whose size is not a multiple of four bytes is uploaded and drawn")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	ApplicationSpecification specification;
	specification.Window.Width = 320;
	specification.Window.Height = 200;
	specification.Window.Headless = true;
	specification.MaxFrames = 2;
	specification.FixedTimestep = 1.0f / 60.0f;
	specification.Graphics.Enabled = true;
	specification.Graphics.Validation = true;
	specification.ImGui.Enabled = true;

	Application app(specification);
	REQUIRE(app.IsInitialized());
	auto* layer = app.PushLayer<TriangleLayer>();
	app.Run();

	// Under AddressSanitizer an upload that rounds the size up would be reported as a heap overflow.
	CHECK(layer->TotalIndices % 2 == 1);
	CHECK(app.GetGraphicsDevice().GetValidationErrorCount() == 0);
	Image image = ReadbackTexture(app.GetGraphicsDevice().GetDevice(), app.GetSwapchain().GetCurrentBackBuffer());
	REQUIRE_FALSE(image.IsEmpty());
	CHECK(image.GetPixel(20, 20).r > 250);
	CHECK(image.GetPixel(300, 180).r < 5);
}

namespace {

	// Drives a button with synthetic Paralax events only (no GLFW input), one step per frame, and records which
	// scroll events reach this layer below the ImGui overlay.
	class SyntheticInputLayer : public Layer
	{
	public:
		void OnUpdate(Timestep) override
		{
			Application& app = Application::Get();
			Frame++;
			if (Frame == 2)
			{
				MouseMovedEvent move(60.0f, 60.0f);
				app.OnEvent(move);
			}
			else if (Frame == 3)
			{
				MouseButtonPressedEvent press(Mouse::ButtonLeft);
				app.OnEvent(press);
			}
			else if (Frame == 4)
			{
				MouseButtonReleasedEvent release(Mouse::ButtonLeft);
				app.OnEvent(release);
			}
			else if (Frame == 5)
			{
				// Over an ImGui window with blocking on: ImGui consumes the scroll.
				MouseScrolledEvent scroll(0.0f, 1.0f);
				app.OnEvent(scroll);
				app.GetImGuiLayer()->SetBlockEvents(false);
			}
			else if (Frame == 6)
			{
				MouseScrolledEvent scroll(0.0f, 1.0f);
				app.OnEvent(scroll);
			}
		}

		void OnEvent(Event& event) override
		{
			if (event.GetEventType() == EventType::MouseScrolled)
				ScrollsReceived.push_back(Frame);
		}

		void OnImGuiRender() override
		{
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(200, 120));
			ImGui::Begin("Input", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
			ImGui::SetCursorPos(ImVec2(20, 20));
			if (ImGui::Button("Press me", ImVec2(120, 80)))
				Clicks++;
			ImGui::End();
			DeltaTime = ImGui::GetIO().DeltaTime;
		}

		int Frame = 0;
		int Clicks = 0;
		float DeltaTime = 0.0f;
		std::vector<int> ScrollsReceived;
	};

}

TEST_CASE("ImGuiLayer: synthetic Paralax events drive the UI and frame time comes from the timestep")
{
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (!Paralax::Testing::RequireTestGraphicsDevice())
		return;
	Paralax::Testing::ReleaseTestGraphicsDevice();

	ApplicationSpecification specification;
	specification.Window.Width = 320;
	specification.Window.Height = 200;
	specification.Window.Headless = true;
	specification.MaxFrames = 8;
	specification.FixedTimestep = 1.0f / 50.0f;
	specification.Graphics.Enabled = true;
	specification.ImGui.Enabled = true;

	Application app(specification);
	REQUIRE(app.IsInitialized());
	auto* layer = app.PushLayer<SyntheticInputLayer>();
	app.Run();

	CHECK(layer->Clicks == 1);
	CHECK(layer->DeltaTime == doctest::Approx(1.0 / 50.0));
	// The first scroll was over the window while blocking: only the second reached the layer.
	CHECK(layer->ScrollsReceived == std::vector<int>{6});
}

TEST_CASE("ImGuiLayer: without graphics ImGui is disabled and no layer is created")
{
	ApplicationSpecification specification;
	specification.Window.Headless = true;
	specification.MaxFrames = 1;
	specification.Graphics.Enabled = false;
	specification.ImGui.Enabled = true;
	Paralax::Testing::ScopedLogCapture log;
	Application app(specification);
	REQUIRE(app.IsInitialized());
	CHECK(app.GetImGuiLayer() == nullptr);
	app.Run();
}
