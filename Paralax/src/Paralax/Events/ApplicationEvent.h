#pragma once

#include "Paralax/Events/Event.h"

#include <cstdint>
#include <spdlog/fmt/fmt.h>

namespace Paralax {

	class WindowResizeEvent : public Event
	{
	public:
		WindowResizeEvent(uint32_t width, uint32_t height)
			: m_Width(width)
			, m_Height(height)
		{
		}

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		std::string ToString() const override { return fmt::format("WindowResizeEvent: {}, {}", m_Width, m_Height); }

		PX_EVENT_CLASS_TYPE(WindowResize)
		PX_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		uint32_t m_Width, m_Height;
	};

	class WindowCloseEvent : public Event
	{
	public:
		WindowCloseEvent() = default;

		PX_EVENT_CLASS_TYPE(WindowClose)
		PX_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowFocusEvent : public Event
	{
	public:
		WindowFocusEvent() = default;

		PX_EVENT_CLASS_TYPE(WindowFocus)
		PX_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowLostFocusEvent : public Event
	{
	public:
		WindowLostFocusEvent() = default;

		PX_EVENT_CLASS_TYPE(WindowLostFocus)
		PX_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowMovedEvent : public Event
	{
	public:
		WindowMovedEvent(int32_t x, int32_t y)
			: m_X(x)
			, m_Y(y)
		{
		}

		int32_t GetX() const { return m_X; }
		int32_t GetY() const { return m_Y; }

		std::string ToString() const override { return fmt::format("WindowMovedEvent: {}, {}", m_X, m_Y); }

		PX_EVENT_CLASS_TYPE(WindowMoved)
		PX_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		int32_t m_X, m_Y;
	};

	// Sent when the window is minimised (iconified) or restored.
	class WindowMinimizeEvent : public Event
	{
	public:
		explicit WindowMinimizeEvent(bool minimized)
			: m_Minimized(minimized)
		{
		}

		bool IsMinimized() const { return m_Minimized; }

		std::string ToString() const override { return fmt::format("WindowMinimizeEvent: {}", m_Minimized); }

		PX_EVENT_CLASS_TYPE(WindowMinimize)
		PX_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		bool m_Minimized;
	};

}
