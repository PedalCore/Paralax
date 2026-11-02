#pragma once

#include "Paralax/Renderer/GraphicsDevice.h"

#include <vector>

namespace Paralax {

	// Optional non-owning allocation seam. Called on the renderer thread; null handles mean allocation failed.
	class LocalShadowResourceFactory
	{
	public:
		virtual ~LocalShadowResourceFactory() = default;
		virtual nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) { return device->createGraphicsPipeline(desc, info); }
		virtual nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) { return device->createTexture(desc); }
		virtual nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) { return device->createFramebuffer(desc); }
	};

	// Owns a bounded, transactionally replaced depth pool. Main-thread-only; device and optional factory outlive it.
	// A failed replacement exposes the lit fallback until a later Ensure succeeds. Wait for GPU idle before Ensure and before destroying this object.
	class LocalShadowMaps
	{
	public:
		LocalShadowMaps(nvrhi::IDevice* device, const GraphicsDeviceLimits& limits, LocalShadowResourceFactory* factory = nullptr);
		LocalShadowMaps(const LocalShadowMaps&) = delete;
		LocalShadowMaps& operator=(const LocalShadowMaps&) = delete;
		// Creates and clears the permanent lit fallback on an open command list. False on allocation failure.
		bool Initialize(nvrhi::ICommandList* commands);
		bool NeedsUpdate(uint32_t resolution, uint32_t requestedLights) const;
		// False on allocation failure (logged once until recovery); zero requested lights is a successful release.
		bool Ensure(uint32_t resolution, uint32_t requestedLights);
		nvrhi::ITexture* GetTexture() const { return m_Ready && m_Texture ? m_Texture.Get() : m_Fallback.Get(); }
		nvrhi::IFramebuffer* GetFramebuffer(uint32_t layer) const;
		nvrhi::IFramebuffer* GetFallbackFramebuffer() const { return m_FallbackFramebuffer; }
		uint32_t GetLayers() const { return m_Ready ? static_cast<uint32_t>(m_Framebuffers.size()) : 0; }
		uint32_t GetResolution() const { return m_Resolution; }
		uint64_t GetAllocationAttempts() const { return m_AllocationAttempts; }

	private:
		nvrhi::DeviceHandle m_Device;
		GraphicsDeviceLimits m_Limits;
		LocalShadowResourceFactory m_DefaultFactory;
		LocalShadowResourceFactory& m_Factory;
		nvrhi::TextureHandle m_Fallback, m_Texture;
		nvrhi::FramebufferHandle m_FallbackFramebuffer;
		std::vector<nvrhi::FramebufferHandle> m_Framebuffers;
		uint32_t m_Resolution = 0, m_RequestedLights = 0;
		uint64_t m_AllocationAttempts = 0;
		bool m_Ready = false, m_ReportedFailure = false;
	};

}
