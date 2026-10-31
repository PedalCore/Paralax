#include "pxpch.h"
#include "Paralax/Renderer/OffscreenSwapchain.h"

#include "Paralax/Core/Window.h"
#include "Paralax/Renderer/GraphicsDevice.h"

namespace Paralax {

	OffscreenSwapchain::OffscreenSwapchain(GraphicsDevice& device, Window& window, const SwapchainSpecification& specification)
		: m_Device(device)
		, m_Window(window)
		, m_MaxFramesInFlight(std::max(specification.MaxFramesInFlight, 1u))
		, m_VSync(specification.VSync)
	{
		CreateBackBuffer();
	}

	OffscreenSwapchain::~OffscreenSwapchain()
	{
		m_Device.WaitForIdle();
	}

	bool OffscreenSwapchain::CreateBackBuffer()
	{
		uint32_t width = 0, height = 0;
		m_Window.GetFramebufferSize(width, height);
		if (width == 0 || height == 0)
			return false;

		if (m_BackBuffer && width == m_Width && height == m_Height)
			return true;

		m_Device.WaitForIdle();
		m_Framebuffer = nullptr;
		m_BackBuffer = nullptr;

		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = s_Format;
		desc.isRenderTarget = true;
		desc.debugName = "OffscreenBackBuffer";
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.setClearValue(nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));

		nvrhi::IDevice* device = m_Device.GetDevice();
		m_BackBuffer = device->createTexture(desc);
		if (!m_BackBuffer)
		{
			PX_CORE_ERROR("Failed to create the offscreen back buffer ({}x{})", width, height);
			return false;
		}
		m_Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_BackBuffer));
		m_Width = width;
		m_Height = height;
		return true;
	}

	bool OffscreenSwapchain::BeginFrame()
	{
		if (m_ResizeRequested || !m_BackBuffer)
		{
			m_ResizeRequested = false;
			if (!CreateBackBuffer())
				return false;
		}
		return true;
	}

	void OffscreenSwapchain::Present()
	{
		nvrhi::IDevice* device = m_Device.GetDevice();

		// Throttle: never let the CPU run more than MaxFramesInFlight frames ahead of the GPU.
		nvrhi::EventQueryHandle query = device->createEventQuery();
		device->setEventQuery(query, nvrhi::CommandQueue::Graphics);
		m_FramesInFlight.push_back(query);
		while (m_FramesInFlight.size() > m_MaxFramesInFlight)
		{
			device->waitEventQuery(m_FramesInFlight.front());
			m_FramesInFlight.erase(m_FramesInFlight.begin());
		}

		m_Device.CollectGarbage();
	}

}
