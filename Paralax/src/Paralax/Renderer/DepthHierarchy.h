#pragma once

#include "Paralax/Renderer/PostEffectResources.h"

#include <glm/glm.hpp>

#include <array>

namespace Paralax {

	// Main-thread-only opaque reversed-Z intervals. Graphics, budget and factory outlive this owner.
	// Each level covers ceil-divided 2x2 children; (-1,-1) means uncovered, while covered depth zero is valid.
	// Preparation failure releases partial allocations and returns false. Returned textures are non-owning.
	class DepthHierarchy
	{
	public:
		static constexpr uint32_t MaxLevels = 16;
		DepthHierarchy(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~DepthHierarchy();
		DepthHierarchy(const DepthHierarchy&) = delete;
		DepthHierarchy& operator=(const DepthHierarchy&) = delete;

		// Zero/oversized dimensions return zero levels. No device allocation occurs.
		static uint32_t LevelCount(glm::uvec2 size);
		static glm::uvec2 LevelSize(glm::uvec2 size, uint32_t level);
		// Depth accepts D32/R32_FLOAT. Coverage accepts RGBA16/32_FLOAT, R32_FLOAT or R8_UNORM;
		// the selected stored component > 0 indicates coverage independently of depth.
		bool Prepare(nvrhi::ITexture* depth, nvrhi::ITexture* coverage, uint32_t coverageComponent = 2);
		// Requires prepared resources and an open command list; records one draw per level.
		void Render(nvrhi::ICommandList* commands);
		void Release();
		bool IsReady() const { return m_Count != 0; }
		uint32_t GetLevelCount() const { return m_Count; }
		nvrhi::ITexture* GetLevel(uint32_t level) const { return level < m_Count ? m_Levels[level] : nullptr; }
		uint64_t GetOwnedBytes() const { return m_Textures.GetOwnedBytes(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_ObjectAttempts; }
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts(); }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		nvrhi::TextureHandle m_Depth, m_Coverage;
		nvrhi::BindingLayoutHandle m_Layout;
		std::array<nvrhi::ITexture*, MaxLevels> m_Levels{};
		std::array<nvrhi::FramebufferHandle, MaxLevels> m_Framebuffers;
		std::array<nvrhi::BindingSetHandle, MaxLevels> m_Bindings;
		std::array<nvrhi::GraphicsPipelineHandle, 2> m_Pipelines;
		uint32_t m_Count = 0, m_Component = 0;
		uint64_t m_ObjectAttempts = 0;
		const char* m_Reason = "";
	};

}
