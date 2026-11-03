#pragma once

#include "Paralax/Renderer/PostEffectResources.h"

#include <array>

namespace Paralax {

	// Optional scene-linear HDR bloom. Main-thread-only; device, shared budget and factory outlive it.
	// Prepare failures expose a zero-bloom fallback; recorded work retains its resources through submission.
	class Bloom
	{
	public:
		static constexpr uint32_t MaxLevels = 6;
		Bloom(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~Bloom();
		bool Prepare(bool enabled, nvrhi::ITexture* hdr, nvrhi::ITexture* neutral, uint32_t levels);
		void Release();
		bool IsReady() const { return m_Ready; }
		bool MatchesShape(uint32_t width, uint32_t height, uint32_t levels) const { return m_Ready && m_HDR->getDesc().width == width && m_HDR->getDesc().height == height && m_RequestedLevels == levels; }
		bool MatchesInputs(nvrhi::ITexture* hdr, uint32_t levels) const { return m_Ready && m_HDR == hdr && m_RequestedLevels == levels; }
		nvrhi::ITexture* GetTexture() const { return m_Ready ? (m_Levels > 1 ? m_Up[0] : m_Down[0]) : nullptr; }
		void Render(nvrhi::ICommandList* commands, float threshold, float softKnee);
		uint32_t GetPassCount() const { return m_Ready ? 2 * m_Levels - 1 : 0; }
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_ObjectAttempts; }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		nvrhi::TextureHandle m_HDR;
		std::array<nvrhi::ITexture*, MaxLevels> m_Down{}, m_Up{};
		std::array<nvrhi::FramebufferHandle, MaxLevels> m_DownFramebuffers, m_UpFramebuffers;
		std::array<nvrhi::BindingSetHandle, MaxLevels> m_DownBindings, m_UpBindings;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::SamplerHandle m_Linear;
		nvrhi::GraphicsPipelineHandle m_DownPipeline, m_UpPipeline;
		uint32_t m_Levels = 0, m_RequestedLevels = 0;
		uint64_t m_ObjectAttempts = 0;
		bool m_Ready = false;
		const char* m_Reason = "";
	};

}
