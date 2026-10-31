#pragma once

#include "Paralax/Core/Window.h"

struct GLFWwindow;

namespace Paralax {

	// GLFW-backed window. Created without a client API (the renderer creates its own Vulkan surface).
	// Headless windows use GLFW's null platform, which requires no display server. Main-thread-only.
	class GLFWWindow : public Window
	{
	public:
		// Returns nullptr (with the reason logged) if the window cannot be created.
		static Scope<GLFWWindow> Create(const WindowSpecification& specification);
		// Number of live references keeping GLFW initialised (one per successfully initialised window).
		static uint32_t GetGLFWReferenceCount();
		~GLFWWindow() override;

		GLFWWindow(const GLFWWindow&) = delete;
		GLFWWindow& operator=(const GLFWWindow&) = delete;

		void PollEvents() override;

		uint32_t GetWidth() const override { return m_Data.Width; }
		uint32_t GetHeight() const override { return m_Data.Height; }
		void SetSize(uint32_t width, uint32_t height) override;

		const std::string& GetTitle() const override { return m_Data.Title; }
		void SetTitle(const std::string& title) override;

		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		// Stored preference; applied by the renderer when it (re)creates the swapchain.
		void SetVSync(bool enabled) override { m_Data.VSync = enabled; }
		bool IsVSync() const override { return m_Data.VSync; }
		bool IsHeadless() const override { return m_Headless; }

		void* GetNativeWindow() const override { return m_Window; }

	private:
		GLFWWindow() = default;
		bool Init(const WindowSpecification& specification);
		void InstallCallbacks();
		static void DispatchEvent(GLFWwindow* window, Event& event);

	private:
		GLFWwindow* m_Window = nullptr;
		bool m_Headless = false;
		bool m_OwnsGLFWReference = false;

		// Accessed from GLFW callbacks through the window user pointer.
		struct WindowData
		{
			std::string Title;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool VSync = true;
			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
	};

}
