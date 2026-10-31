#pragma once

#include "Paralax/Renderer/Swapchain.h"

namespace Paralax {

	// Swapchain substitute for headless windows: a single offscreen sRGB render target sized to the window's
	// framebuffer. Present only advances the frame and throttles the CPU to the GPU.
	class OffscreenSwapchain : public Swapchain
	{
	public:
		OffscreenSwapchain(GraphicsDevice& device, Window& window, const SwapchainSpecification& specification);
		~OffscreenSwapchain() override;

		OffscreenSwapchain(const OffscreenSwapchain&) = delete;
		OffscreenSwapchain& operator=(const OffscreenSwapchain&) = delete;

		bool BeginFrame() override;
		void Present() override;
		void RequestResize() override { m_ResizeRequested = true; }

		nvrhi::ITexture* GetCurrentBackBuffer() const override { return m_BackBuffer; }
		nvrhi::IFramebuffer* GetCurrentFramebuffer() const override { return m_Framebuffer; }
		nvrhi::Format GetFormat() const override { return s_Format; }
		uint32_t GetWidth() const override { return m_Width; }
		uint32_t GetHeight() const override { return m_Height; }

		void SetVSync(bool enabled) override { m_VSync = enabled; }
		bool IsVSync() const override { return m_VSync; }

	private:
		bool CreateBackBuffer();

	private:
		static constexpr nvrhi::Format s_Format = nvrhi::Format::SRGBA8_UNORM;

		GraphicsDevice& m_Device;
		Window& m_Window;
		uint32_t m_MaxFramesInFlight;
		bool m_VSync;
		bool m_ResizeRequested = false;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		nvrhi::TextureHandle m_BackBuffer;
		nvrhi::FramebufferHandle m_Framebuffer;
		std::vector<nvrhi::EventQueryHandle> m_FramesInFlight;
	};

}
