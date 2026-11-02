#pragma once

#include "Paralax/Renderer/GraphicsDevice.h"

#include <array>

namespace Paralax {

	// Main-thread-only, non-owning allocation seam. Null handles report recoverable resource failures.
	class DeferredResourceFactory
	{
	public:
		virtual ~DeferredResourceFactory() = default;
		virtual nvrhi::FormatSupport QueryFormatSupport(nvrhi::IDevice* device, nvrhi::Format format) { return device->queryFormatSupport(format); }
		virtual nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) { return device->createTexture(desc); }
		virtual nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) { return device->createFramebuffer(desc); }
		virtual nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) { return device->createBindingSet(desc, layout); }
		virtual nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) { return device->createGraphicsPipeline(desc, info); }
	};

	// Owns the portable 24-byte/pixel surface buffers and their raster/lighting pipelines. Main-thread-only.
	// Device and optional factory outlive this object. Prepare waits for retirement before replacing resources;
	// call it before static lighting generation. Failure releases partial targets and exposes a Forward fallback reason.
	class DeferredSurfaces
	{
	public:
		explicit DeferredSurfaces(GraphicsDevice& graphics, DeferredResourceFactory* factory = nullptr);
		DeferredSurfaces(const DeferredSurfaces&) = delete;
		DeferredSurfaces& operator=(const DeferredSurfaces&) = delete;

		// Bounds only this path's three additional attachments, with overflow-safe arithmetic.
		static bool FitsCapacity(uint32_t width, uint32_t height) { return width > 0 && height > 0 && uint64_t(width) * height <= (256ull * 1024 * 1024) / 24; }
		bool Prepare(bool enabled, nvrhi::ITexture* hdr, nvrhi::ITexture* depth, nvrhi::IInputLayout* input,
					 nvrhi::IBindingLayout* view, nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe);
		bool IsReady() const { return m_Ready; }
		const char* GetFallbackReason() const { return m_FallbackReason; }
		uint64_t GetAllocationAttempts() const { return m_AllocationAttempts; }
		uint64_t GetOwnedBytes() const { return m_OwnedBytes; }
		uint64_t GetGeneration() const { return m_Generation; }
		nvrhi::IFramebuffer* GetFramebuffer() const { return m_Framebuffer; }
		nvrhi::IGraphicsPipeline* GetMaterialPipeline(bool doubleSided, bool mirrored) const { return m_MaterialPipelines[doubleSided ? 2 : mirrored ? 1
																																					 : 0]; }
		// Clears coverage before material rasterization; positive roughness distinguishes geometry even at depth zero.
		void ClearCoverage(nvrhi::ICommandList* commands);
		// Records into an open list after material rasterization; reads depth and adds lighting to HDR.
		void Resolve(nvrhi::ICommandList* commands, nvrhi::IBindingSet* view, nvrhi::IBindingSet* probe);

	private:
		void ReleaseTargets();
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		nvrhi::DeviceHandle m_Device;
		DeferredResourceFactory m_DefaultFactory;
		DeferredResourceFactory& m_Factory;
		std::array<nvrhi::TextureHandle, 3> m_Surfaces;
		nvrhi::TextureHandle m_HDR, m_Depth;
		nvrhi::FramebufferHandle m_Framebuffer, m_ResolveFramebuffer;
		nvrhi::BindingLayoutHandle m_SurfaceLayout;
		nvrhi::BindingSetHandle m_SurfaceBindings;
		nvrhi::ShaderHandle m_MaterialVS, m_MaterialPS, m_ResolveVS, m_ResolvePS;
		std::array<nvrhi::GraphicsPipelineHandle, 3> m_MaterialPipelines;
		nvrhi::GraphicsPipelineHandle m_ResolvePipeline;
		uint64_t m_AllocationAttempts = 0, m_OwnedBytes = 0, m_Generation = 0;
		bool m_Ready = false;
		const char* m_FallbackReason = "";
	};

}
