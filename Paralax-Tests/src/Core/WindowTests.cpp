#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/Window.h>
#include <Platform/GLFW/GLFWWindow.h>
#include <Paralax/Events/ApplicationEvent.h>

#include <vector>

using namespace Paralax;

namespace {

	WindowSpecification HeadlessSpecification()
	{
		WindowSpecification specification;
		specification.Title = "Window Test";
		specification.Width = 320;
		specification.Height = 240;
		specification.Headless = true;
		return specification;
	}

}

TEST_CASE("Window: headless window reports its specification")
{
	Scope<Window> window = Window::Create(HeadlessSpecification());
	REQUIRE(window);
	CHECK(window->IsHeadless());
	CHECK(window->GetNativeWindow() != nullptr);
	CHECK(window->GetWidth() == 320);
	CHECK(window->GetHeight() == 240);
	CHECK(window->GetTitle() == "Window Test");
	window->SetTitle("Renamed");
	CHECK(window->GetTitle() == "Renamed");

	window->PollEvents();
}

TEST_CASE("Window: resizing emits a resize event and updates the size")
{
	Scope<Window> window = Window::Create(HeadlessSpecification());

	std::vector<std::pair<uint32_t, uint32_t>> resizes;
	window->SetEventCallback([&](Event& event) {
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>([&](WindowResizeEvent& e) {
			resizes.emplace_back(e.GetWidth(), e.GetHeight());
			return true;
		}); });

	window->SetSize(640, 480);
	window->PollEvents();

	REQUIRE_FALSE(resizes.empty());
	CHECK(resizes.back() == std::pair<uint32_t, uint32_t>(640, 480));
	CHECK(window->GetWidth() == 640);
	CHECK(window->GetHeight() == 480);
}

TEST_CASE("Window: multiple headless windows can coexist and be destroyed in any order")
{
	Scope<Window> first = Window::Create(HeadlessSpecification());
	Scope<Window> second = Window::Create(HeadlessSpecification());
	CHECK(first->GetNativeWindow() != second->GetNativeWindow());

	CHECK(GLFWWindow::GetGLFWReferenceCount() == 2);
	first.reset();
	second->PollEvents();
	CHECK(GLFWWindow::GetGLFWReferenceCount() == 1);
	second.reset();
	CHECK(GLFWWindow::GetGLFWReferenceCount() == 0);

	// GLFW must re-initialise cleanly after the last window was destroyed.
	Scope<Window> third = Window::Create(HeadlessSpecification());
	CHECK(third->GetNativeWindow() != nullptr);
}

TEST_CASE("Window: invalid sizes fail creation with an error instead of aborting")
{
	Paralax::Testing::ScopedLogCapture capture;

	for (auto [width, height] : {std::pair<uint32_t, uint32_t>{0, 240}, {320, 0}, {100000, 240}})
	{
		CAPTURE(width);
		CAPTURE(height);
		WindowSpecification specification = HeadlessSpecification();
		specification.Width = width;
		specification.Height = height;
		CHECK(Window::Create(specification) == nullptr);
	}
	CHECK(capture.Contains(spdlog::level::err, "Invalid window size"));

	CHECK(GLFWWindow::GetGLFWReferenceCount() == 0);
}

TEST_CASE("Window: native windows cannot be created while headless windows exist")
{
	Paralax::Testing::ScopedLogCapture capture;
	Scope<Window> headless = Window::Create(HeadlessSpecification());
	REQUIRE(headless);

	WindowSpecification native = HeadlessSpecification();
	native.Headless = false;
	CHECK(Window::Create(native) == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "Cannot create a native window while headless windows exist"));
	CHECK(headless->GetNativeWindow() != nullptr);
	// The rejected window must not hold a GLFW reference.
	CHECK(GLFWWindow::GetGLFWReferenceCount() == 1);
	headless.reset();
	CHECK(GLFWWindow::GetGLFWReferenceCount() == 0);
}

#if defined(PX_ENABLE_ASSERTS)
TEST_CASE("Window: SetSize with a zero dimension is an asserted precondition")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	Scope<Window> window = Window::Create(HeadlessSpecification());
	CHECK_THROWS_AS(window->SetSize(0, 100), Paralax::Testing::AssertionFailedError);
	CHECK(window->GetWidth() == 320);
}
#endif

TEST_CASE("Window: headless windows keep an in-process clipboard")
{
	Scope<Window> window = Window::Create(HeadlessSpecification());
	REQUIRE(window);
	window->SetClipboardText("Paralax \xE2\x9C\x93");
	CHECK(window->GetClipboardText() == "Paralax \xE2\x9C\x93");
	window->SetClipboardText("");
	CHECK(window->GetClipboardText().empty());
}
