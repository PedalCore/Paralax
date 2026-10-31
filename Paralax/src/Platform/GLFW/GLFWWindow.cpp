#include "pxpch.h"
#include "Platform/GLFW/GLFWWindow.h"

#include "Paralax/Events/ApplicationEvent.h"
#include "Paralax/Events/KeyEvent.h"
#include "Paralax/Events/MouseEvent.h"
#include "Platform/Vulkan/VulkanLoader.h"

// The engine renders through Vulkan (GLFW_NO_API); never pull in OpenGL headers. Including the Vulkan core header
// first makes GLFW declare its Vulkan surface functions.
#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace Paralax {

	namespace {

		// GLFW is a process-wide library; it is initialised with the first window and terminated with the last.
		// The platform (native or null/headless) is fixed for the lifetime of that initialisation.
		uint32_t s_GLFWWindowCount = 0;
		bool s_GLFWHeadless = false;

		void GLFWErrorCallback(int error, const char* description)
		{
			PX_CORE_ERROR("GLFW error ({}): {}", error, description);
		}

		bool AcquireGLFW(bool headless)
		{
			if (s_GLFWWindowCount > 0)
			{
				if (s_GLFWHeadless != headless)
				{
					PX_CORE_ERROR("Cannot create a {} window while {} windows exist", headless ? "headless" : "native", s_GLFWHeadless ? "headless" : "native");
					return false;
				}
				s_GLFWWindowCount++;
				return true;
			}

			glfwSetErrorCallback(GLFWErrorCallback);
			glfwInitHint(GLFW_PLATFORM, headless ? GLFW_PLATFORM_NULL : GLFW_ANY_PLATFORM);
			// Make GLFW use the same Vulkan loader as the graphics device (GLFW's own search misses some installs).
			if (!headless)
			{
				if (PFN_vkGetInstanceProcAddr getInstanceProcAddr = VulkanLoader::GetInstanceProcAddr())
					glfwInitVulkanLoader(getInstanceProcAddr);
			}
			if (!glfwInit())
			{
				PX_CORE_ERROR("Failed to initialise GLFW{}", headless ? "" : " (is a display server available? use --headless otherwise)");
				return false;
			}

			s_GLFWHeadless = headless;
			s_GLFWWindowCount = 1;
			return true;
		}

		void ReleaseGLFW()
		{
			PX_CORE_VERIFY(s_GLFWWindowCount > 0, "GLFW reference count underflow");
			if (--s_GLFWWindowCount == 0)
				glfwTerminate();
		}

	}

	void GLFWWindow::DispatchEvent(GLFWwindow* window, Event& event)
	{
		WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
		if (data.EventCallback)
			data.EventCallback(event);
	}

	Scope<Window> Window::Create(const WindowSpecification& specification)
	{
		return GLFWWindow::Create(specification);
	}

	uint32_t GLFWWindow::GetGLFWReferenceCount()
	{
		return s_GLFWWindowCount;
	}

	Scope<GLFWWindow> GLFWWindow::Create(const WindowSpecification& specification)
	{
		Scope<GLFWWindow> window(new GLFWWindow());
		if (!window->Init(specification))
			return nullptr;
		return window;
	}

	bool GLFWWindow::Init(const WindowSpecification& specification)
	{
		m_Headless = specification.Headless;
		m_Data.Title = specification.Title;
		m_Data.Width = specification.Width;
		m_Data.Height = specification.Height;

		PX_CORE_INFO("Creating {}window '{}' ({}x{})", m_Headless ? "headless " : "", m_Data.Title, m_Data.Width, m_Data.Height);

		constexpr uint32_t maxDimension = 16384;
		if (m_Data.Width == 0 || m_Data.Height == 0 || m_Data.Width > maxDimension || m_Data.Height > maxDimension)
		{
			PX_CORE_ERROR("Invalid window size {}x{} (each dimension must be 1-{})", m_Data.Width, m_Data.Height, maxDimension);
			return false;
		}

		if (!AcquireGLFW(m_Headless))
			return false;
		m_OwnsGLFWReference = true;

		glfwDefaultWindowHints();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, specification.Resizable ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_VISIBLE, m_Headless ? GLFW_FALSE : GLFW_TRUE);

		m_Window = glfwCreateWindow(static_cast<int>(m_Data.Width), static_cast<int>(m_Data.Height), m_Data.Title.c_str(), nullptr, nullptr);
		if (!m_Window)
		{
			PX_CORE_ERROR("Failed to create GLFW window '{}'", m_Data.Title);
			return false;
		}

		// The window manager may not honour the requested size exactly.
		int width = 0, height = 0;
		glfwGetWindowSize(m_Window, &width, &height);
		m_Data.Width = static_cast<uint32_t>(width);
		m_Data.Height = static_cast<uint32_t>(height);

		glfwSetWindowUserPointer(m_Window, &m_Data);
		InstallCallbacks();
		return true;
	}

	GLFWWindow::~GLFWWindow()
	{
		if (m_Window)
		{
			glfwDestroyWindow(m_Window);
			m_Window = nullptr;
		}

		if (m_OwnsGLFWReference)
			ReleaseGLFW();
	}

	void GLFWWindow::PollEvents()
	{
		glfwPollEvents();
	}

	void GLFWWindow::WaitEvents(double timeoutSeconds)
	{
		glfwWaitEventsTimeout(timeoutSeconds);
	}

	void GLFWWindow::SetSize(uint32_t width, uint32_t height)
	{
		PX_CORE_ASSERT(width > 0 && height > 0, "Window size must be non-zero");
		if (width == 0 || height == 0)
			return;
		glfwSetWindowSize(m_Window, static_cast<int>(width), static_cast<int>(height));
	}

	void GLFWWindow::GetFramebufferSize(uint32_t& width, uint32_t& height) const
	{
		int w = 0, h = 0;
		glfwGetFramebufferSize(m_Window, &w, &h);
		width = static_cast<uint32_t>(std::max(w, 0));
		height = static_cast<uint32_t>(std::max(h, 0));
	}

	std::vector<std::string> GLFWWindow::GetRequiredVulkanInstanceExtensions() const
	{
		if (m_Headless)
			return {};
		if (!glfwVulkanSupported())
		{
			PX_CORE_ERROR("GLFW reports no Vulkan support; the window cannot be rendered to");
			return {};
		}

		uint32_t count = 0;
		const char** names = glfwGetRequiredInstanceExtensions(&count);
		std::vector<std::string> extensions;
		for (uint32_t i = 0; names && i < count; i++)
			extensions.emplace_back(names[i]);
		return extensions;
	}

	void* GLFWWindow::CreateVulkanSurface(void* vkInstance) const
	{
		if (m_Headless)
		{
			PX_CORE_ERROR("Cannot create a Vulkan surface for a headless window");
			return nullptr;
		}

		VkSurfaceKHR surface = VK_NULL_HANDLE;
		const VkResult result = glfwCreateWindowSurface(static_cast<VkInstance>(vkInstance), m_Window, nullptr, &surface);
		if (result != VK_SUCCESS)
		{
			PX_CORE_ERROR("glfwCreateWindowSurface failed ({})", static_cast<int>(result));
			return nullptr;
		}
		return reinterpret_cast<void*>(surface);
	}

	void GLFWWindow::SetTitle(const std::string& title)
	{
		m_Data.Title = title;
		glfwSetWindowTitle(m_Window, m_Data.Title.c_str());
	}

	void GLFWWindow::InstallCallbacks()
	{
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			data.Width = static_cast<uint32_t>(width);
			data.Height = static_cast<uint32_t>(height);

			WindowResizeEvent event(data.Width, data.Height);
			DispatchEvent(window, event); });

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window) {
			// Closing is decided by the application (layers may veto it).
			glfwSetWindowShouldClose(window, GLFW_FALSE);
			WindowCloseEvent event;
			DispatchEvent(window, event); });

		glfwSetWindowFocusCallback(m_Window, [](GLFWwindow* window, int focused) {
			if (focused)
			{
				WindowFocusEvent event;
				DispatchEvent(window, event);
			}
			else
			{
				WindowLostFocusEvent event;
				DispatchEvent(window, event);
			} });

		// The drawable size can change without a window-size change (e.g. moving to a monitor with a different
		// scale), so a framebuffer change also produces a resize event; the swapchain sizes itself from it.
		glfwSetFramebufferSizeCallback(m_Window, [](GLFWwindow* window, [[maybe_unused]] int width, [[maybe_unused]] int height) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			WindowResizeEvent event(data.Width, data.Height);
			DispatchEvent(window, event);
		});

		// Not every platform reports minimisation as a 0x0 resize (macOS does not), so track iconify explicitly.
		glfwSetWindowIconifyCallback(m_Window, [](GLFWwindow* window, int iconified) {
			WindowMinimizeEvent event(iconified == GLFW_TRUE);
			DispatchEvent(window, event);
		});

		glfwSetWindowPosCallback(m_Window, [](GLFWwindow* window, int x, int y) {
			WindowMovedEvent event(x, y);
			DispatchEvent(window, event); });

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, [[maybe_unused]] int scancode, int action, [[maybe_unused]] int mods) {
			if (key < 0 || key >= static_cast<int>(Key::Count))
				return;

			const auto keyCode = static_cast<KeyCode>(key);
			switch (action)
			{
				case GLFW_PRESS:
				{
					KeyPressedEvent event(keyCode, false);
					DispatchEvent(window, event);
					break;
				}
				case GLFW_REPEAT:
				{
					KeyPressedEvent event(keyCode, true);
					DispatchEvent(window, event);
					break;
				}
				case GLFW_RELEASE:
				{
					KeyReleasedEvent event(keyCode);
					DispatchEvent(window, event);
					break;
				}
				default:
					break;
			} });

		glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int codepoint) {
			KeyTypedEvent event(codepoint);
			DispatchEvent(window, event); });

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, [[maybe_unused]] int mods) {
			if (button < 0 || button >= static_cast<int>(Mouse::Count))
				return;

			const auto mouseCode = static_cast<MouseCode>(button);
			if (action == GLFW_PRESS)
			{
				MouseButtonPressedEvent event(mouseCode);
				DispatchEvent(window, event);
			}
			else if (action == GLFW_RELEASE)
			{
				MouseButtonReleasedEvent event(mouseCode);
				DispatchEvent(window, event);
			} });

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset) {
			MouseScrolledEvent event(static_cast<float>(xOffset), static_cast<float>(yOffset));
			DispatchEvent(window, event); });

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double x, double y) {
			MouseMovedEvent event(static_cast<float>(x), static_cast<float>(y));
			DispatchEvent(window, event); });
	}

}
