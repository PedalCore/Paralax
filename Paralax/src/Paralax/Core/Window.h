#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Events/Event.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Paralax {

	struct WindowSpecification
	{
		std::string Title = "Paralax";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		// Initial vertical sync for the window's swapchain (change it later through Swapchain::SetVSync).
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
		// Like PollEvents, but blocks until an event arrives or the timeout elapses (used while minimised).
		virtual void WaitEvents(double timeoutSeconds) = 0;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
		// Requests a new size in screen coordinates (both dimensions must be non-zero).
		virtual void SetSize(uint32_t width, uint32_t height) = 0;

		virtual const std::string& GetTitle() const = 0;
		virtual void SetTitle(const std::string& title) = 0;

		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual bool IsHeadless() const = 0;

		// Returns the platform handle (GLFWwindow* on every desktop platform).
		virtual void* GetNativeWindow() const = 0;

		// Size of the drawable area in pixels (differs from GetWidth/GetHeight on high-DPI displays).
		virtual void GetFramebufferSize(uint32_t& width, uint32_t& height) const = 0;

		// Vulkan instance extensions needed to present to this window (empty for headless windows).
		virtual std::vector<std::string> GetRequiredVulkanInstanceExtensions() const = 0;
		// Creates a VkSurfaceKHR for this window on the given VkInstance. Returns nullptr (with an error logged) on
		// failure or for headless windows. The caller owns and destroys the surface.
		virtual void* CreateVulkanSurface(void* vkInstance) const = 0;

		// Creates the platform window. Returns nullptr (with the reason logged) if it cannot be created, e.g. the
		// size is zero, no display server is available, or headless and native windows would be mixed.
		static Scope<Window> Create(const WindowSpecification& specification = WindowSpecification());
	};

}
