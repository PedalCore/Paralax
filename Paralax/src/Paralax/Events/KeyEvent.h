#pragma once

#include "Paralax/Core/KeyCodes.h"
#include "Paralax/Events/Event.h"

#include <spdlog/fmt/fmt.h>

namespace Paralax {

	class KeyEvent : public Event
	{
	public:
		KeyCode GetKeyCode() const { return m_KeyCode; }

		PX_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	protected:
		explicit KeyEvent(KeyCode keycode)
			: m_KeyCode(keycode)
		{
		}

		KeyCode m_KeyCode;
	};

	class KeyPressedEvent : public KeyEvent
	{
	public:
		explicit KeyPressedEvent(KeyCode keycode, bool isRepeat = false)
			: KeyEvent(keycode)
			, m_IsRepeat(isRepeat)
		{
		}

		bool IsRepeat() const { return m_IsRepeat; }

		std::string ToString() const override { return fmt::format("KeyPressedEvent: {} (repeat = {})", m_KeyCode, m_IsRepeat); }

		PX_EVENT_CLASS_TYPE(KeyPressed)

	private:
		bool m_IsRepeat;
	};

	class KeyReleasedEvent : public KeyEvent
	{
	public:
		explicit KeyReleasedEvent(KeyCode keycode)
			: KeyEvent(keycode)
		{
		}

		std::string ToString() const override { return fmt::format("KeyReleasedEvent: {}", m_KeyCode); }

		PX_EVENT_CLASS_TYPE(KeyReleased)
	};

	// A Unicode code point produced by text input (layout- and modifier-aware).
	class KeyTypedEvent : public Event
	{
	public:
		explicit KeyTypedEvent(uint32_t codepoint)
			: m_Codepoint(codepoint)
		{
		}

		uint32_t GetCodepoint() const { return m_Codepoint; }

		std::string ToString() const override { return fmt::format("KeyTypedEvent: U+{:04X}", m_Codepoint); }

		PX_EVENT_CLASS_TYPE(KeyTyped)
		PX_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	private:
		uint32_t m_Codepoint;
	};

}
