#pragma once

#include "Paralax/Renderer/DeferredSurfaces.h"
#include "Paralax/Renderer/GPUResourceCache.h"

#include <unordered_map>

namespace Paralax {

	// Portable ID rasterization and per-draw material resolve. Reuses persistent indexed mesh buffers as raw SRVs.
	// Main-thread-only; graphics and optional factory outlive it. Prepare runs before static lighting and waits
	// for retirement on target changes. Null bindings or false Prepare require Forward fallback for the frame.
	class VisibilitySurfaces
	{
	public:
		explicit VisibilitySurfaces(GraphicsDevice& graphics, DeferredResourceFactory* factory = nullptr);
		VisibilitySurfaces(const VisibilitySurfaces&) = delete;
		VisibilitySurfaces& operator=(const VisibilitySurfaces&) = delete;
		static bool FitsTriangleIDs(uint64_t base, uint32_t count) { return count <= 0x7fffffffu && base <= UINT32_MAX && count <= UINT32_MAX - base; }
		static bool FitsGeometry(uint64_t vertexBytes, uint64_t indexBytes, uint64_t limit) { return vertexBytes > 0 && indexBytes > 0 && vertexBytes <= limit && indexBytes <= limit && vertexBytes <= UINT32_MAX && indexBytes <= UINT32_MAX; }
		bool Prepare(bool enabled, nvrhi::ITexture* depth, nvrhi::IFramebuffer* surfaces, nvrhi::IBindingLayout* view,
					 nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe);
		bool PrepareMesh(const GPUMesh& mesh);
		void EndPreparation(bool success);
		nvrhi::IBindingSet* GetBindings(const GPUMesh& mesh, bool resolve) const;
		nvrhi::IFramebuffer* GetFramebuffer() const { return m_Framebuffer; }
		nvrhi::IGraphicsPipeline* GetPipeline(bool doubleSided, bool mirrored) const { return m_Pipelines[doubleSided ? 2 : mirrored ? 1
																																	 : 0]; }
		nvrhi::IGraphicsPipeline* GetResolvePipeline() const { return m_ResolvePipeline; }
		const char* GetFallbackReason() const { return m_FallbackReason; }
		uint64_t GetOwnedBytes() const { return m_OwnedBytes; }
		uint64_t GetGeometryBytes() const;
		uint64_t GetAllocationAttempts() const { return m_AllocationAttempts; }
		uint64_t GetGeneration() const { return m_Generation; }
		void Clear(nvrhi::ICommandList* commands);

	private:
		struct MeshBindings
		{
			nvrhi::BufferHandle Vertices, Indices;
			nvrhi::BindingSetHandle Visibility, Resolve;
			uint64_t LastFrame = 0;
		};
		void ReleaseTargets();
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		nvrhi::DeviceHandle m_Device;
		DeferredResourceFactory m_DefaultFactory;
		DeferredResourceFactory& m_Factory;
		nvrhi::TextureHandle m_IDs, m_Depth;
		nvrhi::FramebufferHandle m_Framebuffer, m_Surfaces;
		nvrhi::BindingLayoutHandle m_VisibilityLayout, m_ResolveLayout;
		nvrhi::ShaderHandle m_VS, m_PS, m_ResolveVS, m_ResolvePS;
		std::array<nvrhi::GraphicsPipelineHandle, 3> m_Pipelines;
		nvrhi::GraphicsPipelineHandle m_ResolvePipeline;
		std::unordered_map<nvrhi::IBuffer*, MeshBindings> m_MeshBindings;
		uint64_t m_Frame = 0, m_Generation = 0, m_AllocationAttempts = 0, m_OwnedBytes = 0;
		const char* m_FallbackReason = "";
	};

}
