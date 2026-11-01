#include "pxpch.h"
#include "Paralax/Core/Input.h"

#include "Paralax/Events/ApplicationEvent.h"
#include "Paralax/Events/KeyEvent.h"
#include "Paralax/Events/MouseEvent.h"

#include <array>
#include <cstdint>

namespace Paralax {

	namespace {

		enum ButtonStateFlags : uint8_t
		{
			ButtonStateDown = PX_BIT(0),
			ButtonStatePressed = PX_BIT(1),
			ButtonStateReleased = PX_BIT(2)
		};

		struct InputState
		{
			std::array<uint8_t, Key::Count> Keys{};
			std::array<uint8_t, Mouse::Count> MouseButtons{};
			glm::vec2 MousePosition{0.0f};
			glm::vec2 ScrollDelta{0.0f};
		};

		InputState s_State;

		template<size_t N>
		bool HasFlag(const std::array<uint8_t, N>& states, uint16_t code, uint8_t flag)
		{
			return code < N && (states[code] & flag) != 0;
		}

		template<size_t N>
		void SetDown(std::array<uint8_t, N>& states, uint16_t code)
		{
			if (code >= N)
				return;

			if (!(states[code] & ButtonStateDown))
				states[code] |= ButtonStateDown | ButtonStatePressed;
		}

		template<size_t N>
		void SetUp(std::array<uint8_t, N>& states, uint16_t code)
		{
			if (code >= N)
				return;

			if (states[code] & ButtonStateDown)
			{
				states[code] &= static_cast<uint8_t>(~ButtonStateDown);
				states[code] |= ButtonStateReleased;
			}
		}

		template<size_t N>
		void ReleaseAll(std::array<uint8_t, N>& states)
		{
			for (uint8_t& state : states)
			{
				if (state & ButtonStateDown)
					state = static_cast<uint8_t>((state & ~ButtonStateDown) | ButtonStateReleased);
			}
		}

	}

	bool Input::IsKeyDown(KeyCode key)
	{
		return HasFlag(s_State.Keys, key, ButtonStateDown);
	}
	bool Input::WasKeyPressed(KeyCode key)
	{
		return HasFlag(s_State.Keys, key, ButtonStatePressed);
	}
	bool Input::WasKeyReleased(KeyCode key)
	{
		return HasFlag(s_State.Keys, key, ButtonStateReleased);
	}

	bool Input::IsMouseButtonDown(MouseCode button)
	{
		return HasFlag(s_State.MouseButtons, button, ButtonStateDown);
	}
	bool Input::WasMouseButtonPressed(MouseCode button)
	{
		return HasFlag(s_State.MouseButtons, button, ButtonStatePressed);
	}
	bool Input::WasMouseButtonReleased(MouseCode button)
	{
		return HasFlag(s_State.MouseButtons, button, ButtonStateReleased);
	}

	glm::vec2 Input::GetMousePosition()
	{
		return s_State.MousePosition;
	}
	glm::vec2 Input::GetMouseScrollDelta()
	{
		return s_State.ScrollDelta;
	}

	void Input::OnEvent(const Event& event)
	{
		switch (event.GetEventType())
		{
			case EventType::KeyPressed:
			{
				// A repeat only continues a press already seen: after keys were released behind the keyboard's back
				// (focus loss, play mode starting), a key still physically held must not turn into a new press.
				const auto& pressed = static_cast<const KeyPressedEvent&>(event);
				if (!pressed.IsRepeat())
					SetDown(s_State.Keys, pressed.GetKeyCode());
				break;
			}
			case EventType::KeyReleased:
				SetUp(s_State.Keys, static_cast<const KeyReleasedEvent&>(event).GetKeyCode());
				break;
			case EventType::MouseButtonPressed:
				SetDown(s_State.MouseButtons, static_cast<const MouseButtonPressedEvent&>(event).GetMouseButton());
				break;
			case EventType::MouseButtonReleased:
				SetUp(s_State.MouseButtons, static_cast<const MouseButtonReleasedEvent&>(event).GetMouseButton());
				break;
			case EventType::MouseMoved:
			{
				const auto& moved = static_cast<const MouseMovedEvent&>(event);
				s_State.MousePosition = {moved.GetX(), moved.GetY()};
				break;
			}
			case EventType::MouseScrolled:
			{
				const auto& scrolled = static_cast<const MouseScrolledEvent&>(event);
				s_State.ScrollDelta += glm::vec2(scrolled.GetXOffset(), scrolled.GetYOffset());
				break;
			}
			case EventType::WindowLostFocus:
			{
				// Keys released while unfocused never reach us, so release everything to avoid stuck input.
				ReleaseAll(s_State.Keys);
				ReleaseAll(s_State.MouseButtons);
				break;
			}
			default:
				break;
		}
	}

	void Input::EndFrame()
	{
		constexpr uint8_t keepMask = ButtonStateDown;
		for (auto& state : s_State.Keys)
			state &= keepMask;
		for (auto& state : s_State.MouseButtons)
			state &= keepMask;
		s_State.ScrollDelta = glm::vec2(0.0f);
	}

	void Input::Reset()
	{
		s_State = InputState{};
	}

}
