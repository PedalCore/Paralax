#pragma once

#include "Paralax/Core/Base.h"

#include <concepts>
#include <cstdint>
#include <ostream>
#include <string>

namespace Paralax {

	// Events are currently blocking: they are dispatched immediately when they occur.

	enum class EventType
	{
		None = 0,
		WindowClose,
		WindowResize,
		WindowFocus,
		WindowLostFocus,
		WindowMoved,
		WindowMinimize,
		KeyPressed,
		KeyReleased,
		KeyTyped,
		MouseButtonPressed,
		MouseButtonReleased,
		MouseMoved,
		MouseScrolled
	};

	enum EventCategory : uint32_t
	{
		EventCategoryNone = 0,
		EventCategoryApplication = PX_BIT(0),
		EventCategoryInput = PX_BIT(1),
		EventCategoryKeyboard = PX_BIT(2),
		EventCategoryMouse = PX_BIT(3),
		EventCategoryMouseButton = PX_BIT(4)
	};

	// clang-format off
#define PX_EVENT_CLASS_TYPE(type)                                              \
	static EventType GetStaticType() { return EventType::type; }                \
	virtual EventType GetEventType() const override { return GetStaticType(); } \
	virtual const char* GetName() const override { return #type; }

#define PX_EVENT_CLASS_CATEGORY(category) \
	virtual uint32_t GetCategoryFlags() const override { return category; }
	// clang-format on

	class Event
	{
	public:
		virtual ~Event() = default;

		bool Handled = false;

		virtual EventType GetEventType() const = 0;
		virtual const char* GetName() const = 0;
		virtual uint32_t GetCategoryFlags() const = 0;
		virtual std::string ToString() const { return GetName(); }

		bool IsInCategory(EventCategory category) const { return (GetCategoryFlags() & category) != 0; }
	};

	// Routes an event to a handler if it is of the handler's event type.
	class EventDispatcher
	{
	public:
		explicit EventDispatcher(Event& event)
			: m_Event(event)
		{
		}

		// F is deduced by the compiler and must be callable as bool(T&).
		// Returns true if the event was of type T (whether or not the handler marked it handled).
		template<typename T, typename F>
		bool Dispatch(const F& func)
		{
			if (m_Event.GetEventType() != T::GetStaticType())
				return false;

			m_Event.Handled |= func(static_cast<T&>(m_Event));
			return true;
		}

	private:
		Event& m_Event;
	};

	inline std::ostream& operator<<(std::ostream& os, const Event& e)
	{
		return os << e.ToString();
	}

}

template<std::derived_from<Paralax::Event> T>
struct fmt::formatter<T> : fmt::formatter<std::string>
{
	auto format(const T& event, fmt::format_context& ctx) const
	{
		return fmt::formatter<std::string>::format(event.ToString(), ctx);
	}
};
