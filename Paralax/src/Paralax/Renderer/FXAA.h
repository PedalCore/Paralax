#pragma once

#include "Paralax/Renderer/PostEffectResources.h"

namespace Paralax {

	// Main-thread-only native encoded-sRGB FXAA filtering. Graphics/budget/factory outlive it.
	// Prepare fails locally without jitter and caches failures until output/budget changes or Release.
	// Input contains undithered encoded sRGB in an ordinary float texture; output is linear plus dither.
	class FXAA
	{
	public:
		FXAA(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~FXAA();
		FXAA(const FXAA&) = delete;
		FXAA& operator=(const FXAA&) = delete;
		bool Prepare(bool needed, nvrhi::IFramebuffer* output);
		void Release();
		bool MatchesOutput(nvrhi::IFramebuffer* output) const;
		bool IsReady() const { return m_Ready; }
		uint64_t GetOwnedBytes() const { return m_Textures.GetOwnedBytes(); }
		nvrhi::IFramebuffer* GetFramebuffer() const { return m_Framebuffer; }
		void Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output);
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_ObjectAttempts; }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		void ReleaseResources();
		bool Fail(const char* reason);
		PostEffectBudget& m_Budget;
		GraphicsDevice& m_Graphics;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		nvrhi::ITexture* m_LDR = nullptr;
		nvrhi::FramebufferHandle m_Framebuffer;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Bindings;
		nvrhi::GraphicsPipelineHandle m_Pipeline;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::FramebufferInfo m_OutputInfo;
		nvrhi::FramebufferInfo m_FailedInfo;
		uint32_t m_FailedWidth = 0, m_FailedHeight = 0, m_FailedArraySize = 0;
		uint64_t m_FailedAvailableBytes = 0;
		bool m_Failed = false;
		uint64_t m_ObjectAttempts = 0;
		bool m_Ready = false;
		const char* m_Reason = "";
	};

}
