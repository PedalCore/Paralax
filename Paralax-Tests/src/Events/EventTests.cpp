#include <doctest/doctest.h>

#include <Paralax/Events/ApplicationEvent.h>
#include <Paralax/Events/KeyEvent.h>
#include <Paralax/Events/MouseEvent.h>

#include <sstream>

using namespace Paralax;

TEST_CASE("Event: static and dynamic types, names and categories")
{
	KeyPressedEvent key(Key::A, true);
	CHECK(key.GetEventType() == EventType::KeyPressed);
	CHECK(KeyPressedEvent::GetStaticType() == EventType::KeyPressed);
	CHECK(std::string(key.GetName()) == "KeyPressed");
	CHECK(key.IsInCategory(EventCategoryKeyboard));
	CHECK(key.IsInCategory(EventCategoryInput));
	CHECK_FALSE(key.IsInCategory(EventCategoryMouse));
	CHECK(key.IsRepeat());
	CHECK(key.GetKeyCode() == Key::A);

	MouseButtonPressedEvent button(Mouse::ButtonMiddle);
	CHECK(button.IsInCategory(EventCategoryMouseButton));
	CHECK(button.IsInCategory(EventCategoryMouse));
	CHECK(button.GetMouseButton() == Mouse::ButtonMiddle);

	WindowCloseEvent close;
	CHECK(close.IsInCategory(EventCategoryApplication));
	CHECK_FALSE(close.IsInCategory(EventCategoryInput));
}

TEST_CASE("Event: string formatting via ToString, ostream and fmt")
{
	WindowResizeEvent resize(1280, 720);
	CHECK(resize.ToString() == "WindowResizeEvent: 1280, 720");

	std::ostringstream stream;
	stream << resize;
	CHECK(stream.str() == "WindowResizeEvent: 1280, 720");

	CHECK(fmt::format("{}", resize) == "WindowResizeEvent: 1280, 720");
	CHECK(KeyTypedEvent(0x41).ToString() == "KeyTypedEvent: U+0041");
	CHECK(WindowCloseEvent().ToString() == "WindowClose");
	CHECK(MouseScrolledEvent(1.5f, -2.0f).ToString() == "MouseScrolledEvent: 1.5, -2");
}

TEST_CASE("EventDispatcher: only matching handlers run, and their result sets Handled")
{
	KeyReleasedEvent event(Key::Escape);
	EventDispatcher dispatcher(event);

	bool mouseCalled = false;
	CHECK_FALSE(dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent&) { mouseCalled = true; return true; }));
	CHECK_FALSE(mouseCalled);
	CHECK_FALSE(event.Handled);

	KeyCode received = 0;
	CHECK(dispatcher.Dispatch<KeyReleasedEvent>([&](KeyReleasedEvent& e) { received = e.GetKeyCode(); return false; }));
	CHECK(received == Key::Escape);
	CHECK_FALSE(event.Handled);

	CHECK(dispatcher.Dispatch<KeyReleasedEvent>([](KeyReleasedEvent&) { return true; }));
	CHECK(event.Handled);

	// Once handled, a later handler returning false must not clear the flag.
	CHECK(dispatcher.Dispatch<KeyReleasedEvent>([](KeyReleasedEvent&) { return false; }));
	CHECK(event.Handled);
}
