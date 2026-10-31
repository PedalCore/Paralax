#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Events/Event.h"

#include <cstdint>
#include <functional>
#include <string>

namespace Paralax {

	struct WindowSpecification
	{
		std::string Title = "Paralax";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool VSync = true;
		bool Resizable = true;
		// Headless windows are never shown and need no display server; used by tests, CI and agent-driven runs.
		bool Headless = false;
	};

	// Platform window abstraction. Windows must be created, used and destroyed on the main thread.
	class Window
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window() = default;

		// Processes pending OS events, dispatching them to the event callback.
		virtual void PollEvents() = 0;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
		// Requests a new size in screen coordinates (both dimensions must be non-zero).
		virtual void SetSize(uint32_t width, uint32_t height) = 0;

		virtual const std::string& GetTitle() const = 0;
		virtual void SetTitle(const std::string& title) = 0;

		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;
		virtual bool IsHeadless() const = 0;

		// Returns the platform handle (GLFWwindow* on every desktop platform).
		virtual void* GetNativeWindow() const = 0;

		// Creates the platform window. Returns nullptr (with the reason logged) if it cannot be created, e.g. the
		// size is zero, no display server is available, or headless and native windows would be mixed.
		static Scope<Window> Create(const WindowSpecification& specification = WindowSpecification());
	};

}
