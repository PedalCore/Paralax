#include "pxpch.h"
#include "Paralax/Renderer/Swapchain.h"

#include "Paralax/Core/Window.h"
#include "Paralax/Renderer/GraphicsDevice.h"
#include "Paralax/Renderer/OffscreenSwapchain.h"

#include <nvrhi/vulkan.h>

#include <algorithm>
#include <limits>

namespace Paralax {

	namespace {

		template<typename T>
		T MakeVkStruct(VkStructureType type)
		{
			T value{};
			value.sType = type;
			return value;
		}

		// Swapchain-related Vulkan entry points, resolved through the graphics device.
		struct SwapchainFunctions
		{
			PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetSurfaceSupport = nullptr;
			PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetSurfaceCapabilities = nullptr;
			PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetSurfaceFormats = nullptr;
			PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetSurfacePresentModes = nullptr;
			PFN_vkDestroySurfaceKHR DestroySurface = nullptr;
			PFN_vkCreateSwapchainKHR CreateSwapchain = nullptr;
			PFN_vkDestroySwapchainKHR DestroySwapchain = nullptr;
			PFN_vkGetSwapchainImagesKHR GetSwapchainImages = nullptr;
			PFN_vkAcquireNextImageKHR AcquireNextImage = nullptr;
			PFN_vkQueuePresentKHR QueuePresent = nullptr;
			PFN_vkCreateSemaphore CreateVkSemaphore = nullptr;
			PFN_vkDestroySemaphore DestroyVkSemaphore = nullptr;

			bool Load(const GraphicsDevice& device)
			{
				auto load = [&device](const char* name, auto& out) {
					out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(device.GetVulkanProcAddress(name));
					return out != nullptr;
				};
				return load("vkGetPhysicalDeviceSurfaceSupportKHR", GetSurfaceSupport) && load("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", GetSurfaceCapabilities) && load("vkGetPhysicalDeviceSurfaceFormatsKHR", GetSurfaceFormats) && load("vkGetPhysicalDeviceSurfacePresentModesKHR", GetSurfacePresentModes) && load("vkDestroySurfaceKHR", DestroySurface) && load("vkCreateSwapchainKHR", CreateSwapchain) && load("vkDestroySwapchainKHR", DestroySwapchain) && load("vkGetSwapchainImagesKHR", GetSwapchainImages) && load("vkAcquireNextImageKHR", AcquireNextImage) && load("vkQueuePresentKHR", QueuePresent) && load("vkCreateSemaphore", CreateVkSemaphore) && load("vkDestroySemaphore", DestroyVkSemaphore);
			}
		};

	}

	// Presents to a native window through VK_KHR_swapchain. Synchronisation follows the standard pattern: an
	// acquire semaphore the graphics queue waits on before rendering, and a per-image present semaphore the queue
	// signals after rendering, which vkQueuePresentKHR waits on.
	class VulkanSwapchain : public Swapchain
	{
	public:
		VulkanSwapchain(GraphicsDevice& device, Window& window, const SwapchainSpecification& specification)
			: m_Device(device)
			, m_Window(window)
			, m_VSync(specification.VSync)
			, m_MaxFramesInFlight(std::max(specification.MaxFramesInFlight, 1u))
		{
		}

		~VulkanSwapchain() override
		{
			m_Device.WaitForIdle();
			DestroySwapchain();
			if (m_Surface)
				m_Functions.DestroySurface(static_cast<VkInstance>(m_Device.GetNativeInstance()), m_Surface, nullptr);
		}

		VulkanSwapchain(const VulkanSwapchain&) = delete;
		VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

		bool Init()
		{
			m_BackendDevice = dynamic_cast<nvrhi::vulkan::IDevice*>(m_Device.GetBackendDevice());
			if (!m_BackendDevice)
			{
				PX_CORE_ERROR("VulkanSwapchain requires a Vulkan graphics device");
				return false;
			}

			m_PresentCommandList = m_Device.GetDevice()->createCommandList();
			if (!m_Functions.Load(m_Device))
			{
				PX_CORE_ERROR("VK_KHR_surface/VK_KHR_swapchain functions are unavailable (was the device created headless?)");
				return false;
			}

			m_Surface = reinterpret_cast<VkSurfaceKHR>(m_Window.CreateVulkanSurface(m_Device.GetNativeInstance()));
			if (!m_Surface)
				return false;

			VkBool32 supported = VK_FALSE;
			m_Functions.GetSurfaceSupport(static_cast<VkPhysicalDevice>(m_Device.GetNativePhysicalDevice()), m_Device.GetGraphicsQueueFamily(), m_Surface, &supported);
			if (!supported)
			{
				PX_CORE_ERROR("The graphics queue cannot present to this window");
				return false;
			}

			return CreateSwapchain();
		}

		bool BeginFrame() override
		{
			if (m_ResizeRequested || !m_Swapchain)
			{
				m_ResizeRequested = false;
				if (!CreateSwapchain())
					return false;
			}

			// Two attempts: the first acquire may report that the swapchain is out of date after a resize.
			for (int attempt = 0; attempt < 2; attempt++)
			{
				VkSemaphore acquireSemaphore = m_AcquireSemaphores[m_AcquireIndex];
				const VkResult result = m_Functions.AcquireNextImage(static_cast<VkDevice>(m_Device.GetNativeDevice()), m_Swapchain,
																	 std::numeric_limits<uint64_t>::max(), acquireSemaphore, VK_NULL_HANDLE, &m_ImageIndex);

				if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
				{
					m_AcquireIndex = (m_AcquireIndex + 1) % static_cast<uint32_t>(m_AcquireSemaphores.size());
					m_BackendDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, acquireSemaphore, 0);
					if (result == VK_SUBOPTIMAL_KHR)
						m_ResizeRequested = true;
					return true;
				}

				if (result != VK_ERROR_OUT_OF_DATE_KHR || !CreateSwapchain())
				{
					if (result != VK_ERROR_OUT_OF_DATE_KHR)
						PX_CORE_ERROR("vkAcquireNextImageKHR failed ({})", static_cast<int>(result));
					return false;
				}
			}
			return false;
		}

		void Present() override
		{
			VkSemaphore presentSemaphore = m_PresentSemaphores[m_ImageIndex];
			m_BackendDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, presentSemaphore, 0);
			// NVRHI attaches pending semaphore waits/signals to the next submission and skips empty ones, so submit
			// an (empty) command list to carry them. It also transitions the back buffer back to Present.
			m_PresentCommandList->open();
			m_PresentCommandList->close();
			m_Device.GetDevice()->executeCommandList(m_PresentCommandList);

			VkPresentInfoKHR presentInfo = MakeVkStruct<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &presentSemaphore;
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = &m_Swapchain;
			presentInfo.pImageIndices = &m_ImageIndex;

			const VkResult result = m_Functions.QueuePresent(static_cast<VkQueue>(m_Device.GetNativeGraphicsQueue()), &presentInfo);
			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
				m_ResizeRequested = true;
			else if (result != VK_SUCCESS)
				PX_CORE_ERROR("vkQueuePresentKHR failed ({})", static_cast<int>(result));

			ThrottleFrames();
			m_Device.CollectGarbage();
		}

		void RequestResize() override { m_ResizeRequested = true; }

		nvrhi::ITexture* GetCurrentBackBuffer() const override { return m_BackBuffers.empty() ? nullptr : m_BackBuffers[m_ImageIndex].Get(); }
		nvrhi::IFramebuffer* GetCurrentFramebuffer() const override { return m_Framebuffers.empty() ? nullptr : m_Framebuffers[m_ImageIndex].Get(); }
		nvrhi::Format GetFormat() const override { return m_Format; }
		uint32_t GetWidth() const override { return m_Width; }
		uint32_t GetHeight() const override { return m_Height; }

		void SetVSync(bool enabled) override
		{
			if (enabled != m_VSync)
			{
				m_VSync = enabled;
				m_ResizeRequested = true;
			}
		}
		bool IsVSync() const override { return m_VSync; }

	private:
		void ThrottleFrames()
		{
			nvrhi::IDevice* device = m_Device.GetDevice();
			nvrhi::EventQueryHandle query = device->createEventQuery();
			device->setEventQuery(query, nvrhi::CommandQueue::Graphics);
			m_FramesInFlight.push_back(query);
			while (m_FramesInFlight.size() > m_MaxFramesInFlight)
			{
				device->waitEventQuery(m_FramesInFlight.front());
				m_FramesInFlight.erase(m_FramesInFlight.begin());
			}
		}

		void DestroySwapchain()
		{
			VkDevice device = static_cast<VkDevice>(m_Device.GetNativeDevice());
			m_Framebuffers.clear();
			m_BackBuffers.clear();
			m_FramesInFlight.clear();
			for (VkSemaphore semaphore : m_AcquireSemaphores)
				m_Functions.DestroyVkSemaphore(device, semaphore, nullptr);
			for (VkSemaphore semaphore : m_PresentSemaphores)
				m_Functions.DestroyVkSemaphore(device, semaphore, nullptr);
			m_AcquireSemaphores.clear();
			m_PresentSemaphores.clear();
			if (m_Swapchain)
			{
				m_Functions.DestroySwapchain(device, m_Swapchain, nullptr);
				m_Swapchain = VK_NULL_HANDLE;
			}
		}

		bool CreateSwapchain()
		{
			VkPhysicalDevice physicalDevice = static_cast<VkPhysicalDevice>(m_Device.GetNativePhysicalDevice());
			VkDevice device = static_cast<VkDevice>(m_Device.GetNativeDevice());

			VkSurfaceCapabilitiesKHR capabilities{};
			if (m_Functions.GetSurfaceCapabilities(physicalDevice, m_Surface, &capabilities) != VK_SUCCESS)
			{
				PX_CORE_ERROR("Failed to query surface capabilities");
				return false;
			}

			uint32_t width = 0, height = 0;
			m_Window.GetFramebufferSize(width, height);
			if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
			{
				width = capabilities.currentExtent.width;
				height = capabilities.currentExtent.height;
			}
			width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
			if (width == 0 || height == 0)
				return false; // Minimised: try again next frame.

			uint32_t formatCount = 0;
			m_Functions.GetSurfaceFormats(physicalDevice, m_Surface, &formatCount, nullptr);
			std::vector<VkSurfaceFormatKHR> formats(formatCount);
			m_Functions.GetSurfaceFormats(physicalDevice, m_Surface, &formatCount, formats.data());

			VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
			for (VkFormat preferred : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
			{
				auto it = std::find_if(formats.begin(), formats.end(), [preferred](const VkSurfaceFormatKHR& f) {
					return f.format == preferred && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
				});
				if (it != formats.end())
				{
					chosen = *it;
					break;
				}
			}
			if (chosen.format == VK_FORMAT_UNDEFINED)
			{
				PX_CORE_ERROR("The window surface supports no 8-bit sRGB format");
				return false;
			}
			m_Format = chosen.format == VK_FORMAT_B8G8R8A8_SRGB ? nvrhi::Format::SBGRA8_UNORM : nvrhi::Format::SRGBA8_UNORM;

			uint32_t modeCount = 0;
			m_Functions.GetSurfacePresentModes(physicalDevice, m_Surface, &modeCount, nullptr);
			std::vector<VkPresentModeKHR> modes(modeCount);
			m_Functions.GetSurfacePresentModes(physicalDevice, m_Surface, &modeCount, modes.data());
			auto hasMode = [&modes](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };

			VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // Always supported.
			if (!m_VSync)
			{
				if (hasMode(VK_PRESENT_MODE_MAILBOX_KHR))
					presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
				else if (hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR))
					presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			}

			uint32_t imageCount = std::max(capabilities.minImageCount + 1, 3u);
			if (capabilities.maxImageCount > 0)
				imageCount = std::min(imageCount, capabilities.maxImageCount);

			VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			// Transfer usage enables screenshots and blits when the surface allows it.
			usage |= capabilities.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

			m_Device.WaitForIdle();
			VkSwapchainKHR oldSwapchain = m_Swapchain;
			m_Framebuffers.clear();
			m_BackBuffers.clear();

			VkSwapchainCreateInfoKHR createInfo = MakeVkStruct<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
			createInfo.surface = m_Surface;
			createInfo.minImageCount = imageCount;
			createInfo.imageFormat = chosen.format;
			createInfo.imageColorSpace = chosen.colorSpace;
			createInfo.imageExtent = {width, height};
			createInfo.imageArrayLayers = 1;
			createInfo.imageUsage = usage;
			createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			createInfo.preTransform = capabilities.currentTransform;
			createInfo.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
			createInfo.presentMode = presentMode;
			createInfo.clipped = VK_TRUE;
			createInfo.oldSwapchain = oldSwapchain;

			VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
			const VkResult result = m_Functions.CreateSwapchain(device, &createInfo, nullptr, &newSwapchain);
			// The old swapchain (and everything tied to it) is retired either way.
			m_Swapchain = oldSwapchain;
			DestroySwapchain();
			if (result != VK_SUCCESS)
			{
				PX_CORE_ERROR("vkCreateSwapchainKHR failed ({})", static_cast<int>(result));
				return false;
			}
			m_Swapchain = newSwapchain;

			uint32_t actualCount = 0;
			m_Functions.GetSwapchainImages(device, m_Swapchain, &actualCount, nullptr);
			std::vector<VkImage> images(actualCount);
			m_Functions.GetSwapchainImages(device, m_Swapchain, &actualCount, images.data());

			nvrhi::IDevice* nvrhiDevice = m_Device.GetDevice();
			for (VkImage image : images)
			{
				nvrhi::TextureDesc desc;
				desc.width = width;
				desc.height = height;
				desc.format = m_Format;
				desc.isRenderTarget = true;
				desc.debugName = "SwapchainImage";
				desc.initialState = nvrhi::ResourceStates::Present;
				desc.keepInitialState = true;

				nvrhi::TextureHandle texture = nvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), desc);
				m_BackBuffers.push_back(texture);
				m_Framebuffers.push_back(nvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture)));
			}

			VkSemaphoreCreateInfo semaphoreInfo = MakeVkStruct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
			// One more acquire semaphore than frames in flight, so a semaphore is never reused while still pending.
			const size_t acquireCount = std::max<size_t>(actualCount, m_MaxFramesInFlight + 1);
			for (size_t i = 0; i < acquireCount; i++)
			{
				VkSemaphore semaphore = VK_NULL_HANDLE;
				m_Functions.CreateVkSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
				m_AcquireSemaphores.push_back(semaphore);
			}
			for (uint32_t i = 0; i < actualCount; i++)
			{
				VkSemaphore semaphore = VK_NULL_HANDLE;
				m_Functions.CreateVkSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
				m_PresentSemaphores.push_back(semaphore);
			}

			m_AcquireIndex = 0;
			m_ImageIndex = 0;
			m_Width = width;
			m_Height = height;
			PX_CORE_INFO("Swapchain: {}x{}, {} images, {}", width, height, actualCount, m_VSync ? "vsync" : "no vsync");
			return true;
		}

	private:
		GraphicsDevice& m_Device;
		Window& m_Window;
		bool m_VSync;
		uint32_t m_MaxFramesInFlight;
		bool m_ResizeRequested = false;

		SwapchainFunctions m_Functions;
		nvrhi::vulkan::IDevice* m_BackendDevice = nullptr;
		nvrhi::CommandListHandle m_PresentCommandList;
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		nvrhi::Format m_Format = nvrhi::Format::UNKNOWN;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_ImageIndex = 0;
		uint32_t m_AcquireIndex = 0;

		std::vector<nvrhi::TextureHandle> m_BackBuffers;
		std::vector<nvrhi::FramebufferHandle> m_Framebuffers;
		std::vector<VkSemaphore> m_AcquireSemaphores;
		std::vector<VkSemaphore> m_PresentSemaphores;
		std::vector<nvrhi::EventQueryHandle> m_FramesInFlight;
	};

	Scope<Swapchain> Swapchain::Create(GraphicsDevice& device, Window& window, const SwapchainSpecification& specification)
	{
		if (window.IsHeadless())
			return CreateScope<OffscreenSwapchain>(device, window, specification);

		auto swapchain = CreateScope<VulkanSwapchain>(device, window, specification);
		if (!swapchain->Init())
			return nullptr;
		return swapchain;
	}

}
