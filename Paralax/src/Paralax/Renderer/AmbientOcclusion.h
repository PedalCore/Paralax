#pragma once

#include "Paralax/Renderer/PostEffectResources.h"

#include <glm/glm.hpp>

#include <array>

namespace Paralax {

	// Optional screen-space diffuse occlusion. Main-thread-only; device, budget and factory outlive it.
	// Prepare returns false on unsupported inputs/allocation failure, with a neutral-lighting fallback.
	class AmbientOcclusion
	{
	public:
		AmbientOcclusion(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~AmbientOcclusion();
		bool Prepare(bool enabled, nvrhi::ITexture* depth, nvrhi::ITexture* deferredGuide, nvrhi::ITexture* neutral,
					 nvrhi::IBuffer* viewConstants, nvrhi::IInputLayout* input, nvrhi::IBindingLayout* view,
					 nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe);
		void Release();
		bool IsReady() const { return m_Ready; }
		bool MatchesInputs(nvrhi::ITexture* depth, nvrhi::ITexture* guide) const { return m_Ready && m_Depth == depth && m_DeferredGuide == guide; }
		bool NeedsPrepass() const { return m_Ready && !m_DeferredGuide; }
		nvrhi::ITexture* GetTexture() const { return m_Ready ? m_Targets[4] : nullptr; }
		nvrhi::IFramebuffer* GetGuideFramebuffer() const { return m_GuideFramebuffer; }
		nvrhi::IGraphicsPipeline* GetGuidePipeline(bool doubleSided, bool mirrored) const { return m_GuidePipelines[doubleSided ? 2 : mirrored ? 1
																																			   : 0]; }
		void ClearGuide(nvrhi::ICommandList* commands);
		void Render(nvrhi::ICommandList* commands, const glm::mat4& projection, bool orthographic, float radius, float bias, float strength, uint32_t samples);
		uint64_t GetOwnedBytes() const { return m_Textures.GetOwnedBytes(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_ObjectAttempts; }
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts(); }
		uint64_t GetGeneration() const { return m_Generation; }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		nvrhi::TextureHandle m_Depth, m_DeferredGuide;
		std::array<nvrhi::ITexture*, 5> m_Targets{};
		std::array<nvrhi::ITexture*, 2> m_Guides{};
		nvrhi::FramebufferHandle m_GuideFramebuffer;
		std::array<nvrhi::FramebufferHandle, 5> m_Framebuffers;
		std::array<nvrhi::BindingSetHandle, 5> m_Bindings;
		nvrhi::BindingSetHandle m_ViewBindings;
		nvrhi::BindingLayoutHandle m_ViewLayout, m_InputLayout;
		std::array<nvrhi::GraphicsPipelineHandle, 3> m_GuidePipelines;
		std::array<nvrhi::GraphicsPipelineHandle, 5> m_Pipelines;
		uint64_t m_Generation = 0, m_ObjectAttempts = 0;
		bool m_Ready = false;
		const char* m_Reason = "";
	};

}
