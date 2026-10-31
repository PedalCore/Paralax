#pragma once

#include "Paralax/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Paralax {

	class GraphicsDevice;
	class Window;

	struct SwapchainSpecification
	{
		bool VSync = true;
		// Frames the CPU may record ahead of the GPU before BeginFrame blocks.
		uint32_t MaxFramesInFlight = 2;
	};

	// The images a window is rendered into. For native windows these are presentable Vulkan swapchain images;
	// for headless windows an offscreen render target of the window's size is used instead, so the same frame
	// code runs (and can be captured) without a display. Main-thread-only.
	//
	// The back buffer is an 8-bit sRGB-encoded format: shaders write linear values and the hardware encodes them.
	class Swapchain
	{
	public:
		virtual ~Swapchain() = default;

		// Returns nullptr (with an error logged) if the swapchain cannot be created.
		static Scope<Swapchain> Create(GraphicsDevice& device, Window& window, const SwapchainSpecification& specification = {});

		// Acquires the next back buffer. Returns false when there is nothing to render to this frame (e.g. the
		// window is minimised); the caller then skips rendering and Present.
		virtual bool BeginFrame() = 0;
		// Presents the current back buffer. All rendering into it must have been submitted.
		virtual void Present() = 0;

		// Recreates the back buffers at the window's current framebuffer size on the next BeginFrame.
		virtual void RequestResize() = 0;

		virtual nvrhi::ITexture* GetCurrentBackBuffer() const = 0;
		virtual nvrhi::IFramebuffer* GetCurrentFramebuffer() const = 0;
		virtual nvrhi::Format GetFormat() const = 0;
		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;

		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;
	};

}
