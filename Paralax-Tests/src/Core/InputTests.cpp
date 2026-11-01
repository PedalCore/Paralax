#include <doctest/doctest.h>

#include <Paralax/Core/Input.h>
#include <Paralax/Events/ApplicationEvent.h>
#include <Paralax/Events/KeyEvent.h>
#include <Paralax/Events/MouseEvent.h>

using namespace Paralax;

namespace {

	struct InputFixture
	{
		InputFixture() { Input::Reset(); }
		~InputFixture() { Input::Reset(); }
	};

}

TEST_CASE_FIXTURE(InputFixture, "Input: key press is reported as down and pressed for one frame")
{
	Input::OnEvent(KeyPressedEvent(Key::Space));

	CHECK(Input::IsKeyDown(Key::Space));
	CHECK(Input::WasKeyPressed(Key::Space));
	CHECK_FALSE(Input::WasKeyReleased(Key::Space));
	CHECK_FALSE(Input::IsKeyDown(Key::A));

	Input::EndFrame();
	CHECK(Input::IsKeyDown(Key::Space));
	CHECK_FALSE(Input::WasKeyPressed(Key::Space));

	Input::OnEvent(KeyReleasedEvent(Key::Space));
	CHECK_FALSE(Input::IsKeyDown(Key::Space));
	CHECK(Input::WasKeyReleased(Key::Space));

	Input::EndFrame();
	CHECK_FALSE(Input::WasKeyReleased(Key::Space));
}

TEST_CASE_FIXTURE(InputFixture, "Input: repeat events do not create new press transitions")
{
	Input::OnEvent(KeyPressedEvent(Key::W));
	Input::EndFrame();
	Input::OnEvent(KeyPressedEvent(Key::W, true));
	CHECK(Input::IsKeyDown(Key::W));
	CHECK_FALSE(Input::WasKeyPressed(Key::W));

	// After everything was released (focus loss), the held key's next repeat is not a new press.
	Input::OnEvent(WindowLostFocusEvent());
	Input::EndFrame();
	Input::OnEvent(KeyPressedEvent(Key::W, true));
	CHECK_FALSE(Input::IsKeyDown(Key::W));
	CHECK_FALSE(Input::WasKeyPressed(Key::W));
}

TEST_CASE_FIXTURE(InputFixture, "Input: press and release within one frame reports both transitions")
{
	Input::OnEvent(KeyPressedEvent(Key::Enter));
	Input::OnEvent(KeyReleasedEvent(Key::Enter));
	CHECK_FALSE(Input::IsKeyDown(Key::Enter));
	CHECK(Input::WasKeyPressed(Key::Enter));
	CHECK(Input::WasKeyReleased(Key::Enter));
}

TEST_CASE_FIXTURE(InputFixture, "Input: releasing a key that is not down is ignored")
{
	Input::OnEvent(KeyReleasedEvent(Key::Q));
	CHECK_FALSE(Input::WasKeyReleased(Key::Q));
}

TEST_CASE_FIXTURE(InputFixture, "Input: out-of-range codes are ignored and report false")
{
	Input::OnEvent(KeyPressedEvent(Key::Count));
	Input::OnEvent(MouseButtonPressedEvent(Mouse::Count));
	CHECK_FALSE(Input::IsKeyDown(Key::Count));
	CHECK_FALSE(Input::IsKeyDown(UINT16_MAX));
	CHECK_FALSE(Input::IsMouseButtonDown(Mouse::Count));
}

TEST_CASE_FIXTURE(InputFixture, "Input: every key code below Key::Count is tracked")
{
	for (KeyCode key = 0; key < Key::Count; key++)
		Input::OnEvent(KeyPressedEvent(key));
	for (KeyCode key = 0; key < Key::Count; key++)
		REQUIRE(Input::IsKeyDown(key));
}

TEST_CASE_FIXTURE(InputFixture, "Input: mouse buttons, position and scroll")
{
	Input::OnEvent(MouseButtonPressedEvent(Mouse::ButtonLeft));
	Input::OnEvent(MouseMovedEvent(10.5f, 20.0f));
	Input::OnEvent(MouseScrolledEvent(0.0f, 1.0f));
	Input::OnEvent(MouseScrolledEvent(0.5f, 2.0f));

	CHECK(Input::IsMouseButtonDown(Mouse::ButtonLeft));
	CHECK(Input::WasMouseButtonPressed(Mouse::ButtonLeft));
	CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonRight));
	CHECK(Input::GetMouseX() == 10.5f);
	CHECK(Input::GetMouseY() == 20.0f);
	CHECK(Input::GetMouseScrollDelta() == glm::vec2(0.5f, 3.0f));

	Input::EndFrame();
	CHECK(Input::GetMouseScrollDelta() == glm::vec2(0.0f));
	CHECK(Input::GetMousePosition() == glm::vec2(10.5f, 20.0f));

	Input::OnEvent(MouseButtonReleasedEvent(Mouse::ButtonLeft));
	CHECK(Input::WasMouseButtonReleased(Mouse::ButtonLeft));
	CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonLeft));
}

TEST_CASE_FIXTURE(InputFixture, "Input: losing window focus releases all keys and buttons")
{
	Input::OnEvent(KeyPressedEvent(Key::LeftShift));
	Input::OnEvent(MouseButtonPressedEvent(Mouse::ButtonRight));
	Input::OnEvent(MouseMovedEvent(5.0f, 6.0f));
	Input::EndFrame();

	Input::OnEvent(WindowLostFocusEvent());
	CHECK_FALSE(Input::IsKeyDown(Key::LeftShift));
	CHECK(Input::WasKeyReleased(Key::LeftShift));
	CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonRight));
	CHECK(Input::WasMouseButtonReleased(Mouse::ButtonRight));
	CHECK(Input::GetMousePosition() == glm::vec2(5.0f, 6.0f));
}

TEST_CASE_FIXTURE(InputFixture, "Input: Reset clears everything")
{
	Input::OnEvent(KeyPressedEvent(Key::A));
	Input::OnEvent(MouseMovedEvent(1.0f, 2.0f));
	Input::Reset();
	CHECK_FALSE(Input::IsKeyDown(Key::A));
	CHECK_FALSE(Input::WasKeyPressed(Key::A));
	CHECK(Input::GetMousePosition() == glm::vec2(0.0f));
}
