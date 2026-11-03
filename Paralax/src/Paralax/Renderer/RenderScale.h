#pragma once

#include "Paralax/Renderer/PostEffectResources.h"

#include <glm/glm.hpp>

namespace Paralax {

	// Main-thread-only native text-depth reconstruction for a scaled scene raster. Graphics/budget/factory outlive it.
	// The caller releases text framebuffer references before replacement. Failure requires effective scale one.
	class RenderScale
	{
	public:
		RenderScale(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~RenderScale();
		RenderScale(const RenderScale&) = delete;
		RenderScale& operator=(const RenderScale&) = delete;
		// Pure dimension policy: finite scale clamped to [0.5,2], nonfinite treated as one; zero output stays zero and overflow saturates to UINT32_MAX.
		static glm::uvec2 CalculateDimensions(uint32_t outputWidth, uint32_t outputHeight, float scale);
		bool Prepare(bool needed, nvrhi::ITexture* sceneDepth, uint32_t outputWidth, uint32_t outputHeight);
		void Release();
		bool MatchesInputs(nvrhi::ITexture* sceneDepth, uint32_t outputWidth, uint32_t outputHeight) const;
		bool IsReady() const { return m_Ready; }
		nvrhi::ITexture* GetDepth() const { return m_Ready ? m_NativeDepth : nullptr; }
		void RenderDepth(nvrhi::ICommandList* commands);
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_ObjectAttempts; }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		nvrhi::TextureHandle m_Source;
		nvrhi::ITexture* m_NativeDepth = nullptr;
		nvrhi::FramebufferHandle m_Framebuffer;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Bindings;
		nvrhi::GraphicsPipelineHandle m_Pipeline;
		uint64_t m_ObjectAttempts = 0;
		bool m_Ready = false;
		const char* m_Reason = "";
	};

}
