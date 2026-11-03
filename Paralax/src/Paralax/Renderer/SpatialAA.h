#pragma once

#include "Paralax/Renderer/PostEffectResources.h"

namespace Paralax {

	// Main-thread-only native linear-LDR edge filtering. Graphics/budget/factory outlive it.
	// Prepare fails locally without changing the scene raster or enabling jitter.
	class SpatialAA
	{
	public:
		SpatialAA(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~SpatialAA();
		SpatialAA(const SpatialAA&) = delete;
		SpatialAA& operator=(const SpatialAA&) = delete;
		bool Prepare(bool needed, nvrhi::IFramebuffer* output);
		void Release();
		bool IsReady() const { return m_Ready; }
		uint64_t GetOwnedBytes() const { return m_Textures.GetOwnedBytes(); }
		nvrhi::IFramebuffer* GetFramebuffer() const { return m_Framebuffer; }
		void Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output);
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_ObjectAttempts; }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		bool Fail(const char* reason);
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
		uint64_t m_ObjectAttempts = 0;
		bool m_Ready = false;
		const char* m_Reason = "";
	};

}
