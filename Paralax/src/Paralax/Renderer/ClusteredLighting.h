#pragma once

#include "Paralax/Renderer/SceneRendererTypes.h"
#include "Paralax/Renderer/PostEffectResources.h"

#include <optional>
#include <span>

namespace Paralax {

	// Pure grid preparation; no resources or threading restrictions. Invalid/unsupported inputs return empty.
	// Matrices and returned float boundaries are the exact inputs consumed by the GPU certificate.
	class ClusteredLightingGrid
	{
	public:
		static constexpr uint32_t TileSize = 64, DepthSlices = 16, WordsPerCell = 8;
		static constexpr uint64_t PrefixBytes = GPU::MaxLights * sizeof(GPU::Light);
		static std::optional<ClusteredLightingGrid> Create(uint32_t width, uint32_t height, uint32_t lightCount,
														   float nearDepth, float farDepth, const glm::mat4& view, const glm::mat4& projection, uint64_t maxBufferBytes);
		const GPU::ClusterGridConstants& GetConstants() const { return m_Constants; }
		uint32_t GetCellCount() const { return m_Constants.Dimensions.z * m_Constants.Dimensions.w * DepthSlices; }
		uint64_t GetBufferBytes() const { return PrefixBytes + uint64_t(GetCellCount()) * WordsPerCell * sizeof(uint32_t); }

	private:
		GPU::ClusterGridConstants m_Constants{};
	};
	// Main-thread-only optional mask storage. Graphics, budget, factory and submitted resources outlive it.
	// Prepare allocates early; FinalizeView only clones descriptors after environment identities settle.
	// Failures return false/null; unchanged resource failures are cached until the request/budget changes.
	class ClusteredLighting
	{
	public:
		ClusteredLighting(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~ClusteredLighting();
		ClusteredLighting(const ClusteredLighting&) = delete;
		ClusteredLighting& operator=(const ClusteredLighting&) = delete;
		bool Prepare(const ClusteredLightingGrid& grid, nvrhi::IBuffer* viewConstants);
		// Publishes constants and a complete cloned view together (null/active=0 on failure).
		// Only its t0 raw binding differs from baseline; no backing allocation happens here.
		nvrhi::IBindingSet* FinalizeView(nvrhi::IBindingSet* baseline, nvrhi::IBindingLayout* layout, GPU::ClusterGridConstants& publishedConstants);
		void InvalidateView();
		void Release();
		// Caller uploads matching view constants first. Invalid counts fail before recording any access.
		bool Dispatch(nvrhi::ICommandList* commands, std::span<const GPU::Light> lights);
		bool IsReady() const { return m_Data != nullptr && m_Pipeline != nullptr; }
		nvrhi::IBuffer* GetBuffer() const { return m_Data; }
		uint64_t GetOwnedBytes() const { return m_Buffers.GetOwnedBytes(); }
		uint64_t GetBackingAttempts() const { return m_Buffers.GetBackingAttempts(); }
		uint64_t GetAllocationAttempts() const { return m_Buffers.GetAllocationAttempts() + m_ObjectAttempts; }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		void ReleaseResources();
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		PostEffectBudget& m_Budget;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectBuffers m_Buffers;
		nvrhi::IBuffer* m_Data = nullptr;
		nvrhi::BufferHandle m_ViewBuffer, m_FailedViewBuffer;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Bindings, m_BaseView, m_ClusterView;
		nvrhi::BindingLayoutHandle m_ViewLayout;
		nvrhi::ComputePipelineHandle m_Pipeline;
		GPU::ClusterGridConstants m_Grid{};
		uint64_t m_FailedBytes = 0, m_FailedAvailable = 0, m_FailedCloneAvailable = 0, m_ObjectAttempts = 0;
		const char* m_Reason = "";
	};

}
