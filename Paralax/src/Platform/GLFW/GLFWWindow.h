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
		void WaitEvents(double timeoutSeconds) override;

		uint32_t GetWidth() const override { return m_Data.Width; }
		uint32_t GetHeight() const override { return m_Data.Height; }
		void SetSize(uint32_t width, uint32_t height) override;

		const std::string& GetTitle() const override { return m_Data.Title; }
		void SetTitle(const std::string& title) override;

		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		bool IsHeadless() const override { return m_Headless; }

		void* GetNativeWindow() const override { return m_Window; }

		void GetFramebufferSize(uint32_t& width, uint32_t& height) const override;
		std::string GetClipboardText() const override;
		void SetClipboardText(const std::string& text) override;
		std::vector<std::string> GetRequiredVulkanInstanceExtensions() const override;
		void* CreateVulkanSurface(void* vkInstance) const override;

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
			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
	};

}
