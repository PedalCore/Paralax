#pragma once

#include "Paralax/Renderer/PostEffectResources.h"
#include "Paralax/Renderer/TemporalState.h"

#include <array>
#include <optional>

namespace Paralax {

	enum class AntiAliasingMode
	{
		Off = 0,
		Spatial = 1,
		Temporal = 2
	};

	struct TemporalGPURecord
	{
		glm::mat4 PreviousClip{1}, PreviousView{1};
		glm::uvec4 Identity{0}; // stable ID, reactive, unused, unused
	};
	static_assert(sizeof(TemporalGPURecord) == 144);

	// Main-thread-only temporal resources for one viewport/capture. Graphics, budget and factory outlive it.
	// Preparation failure leaves no partial history. The caller commits State only after submission.
	class TemporalAA
	{
	public:
		TemporalAA(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~TemporalAA();
		TemporalAA(const TemporalAA&) = delete;
		TemporalAA& operator=(const TemporalAA&) = delete;
		bool Prepare(nvrhi::ITexture* hdr, nvrhi::ITexture* depth, uint32_t drawCount, nvrhi::IInputLayout* input,
					 nvrhi::IBindingLayout* view, nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe);
		void Release();
		void UploadAndClear(nvrhi::ICommandList* commands, std::span<const TemporalGPURecord> records);
		void Resolve(nvrhi::ICommandList* commands, const glm::mat4& inverseProjection, float historyWeight);
		void Commit();
		TemporalState& GetState() { return m_State; }
		const TemporalState& GetState() const { return m_State; }
		bool IsReady() const { return m_Ready; }
		AntiAliasingMode GetEffectiveMode() const { return m_EffectiveMode; }
		const std::string& GetRenderFallbackReason() const { return m_RenderFallback; }
		uint32_t GetPassCount() const { return m_LastPasses; }
		uint64_t GetOwnedBytes() const { return m_Textures.GetOwnedBytes() + m_Buffers.GetOwnedBytes(); }
		nvrhi::ITexture* GetCompletedOutput() const { return m_Ready && m_State.IsValid() ? m_History[1 - m_WriteIndex][0] : nullptr; }
		bool MatchesSize(uint32_t width, uint32_t height) const { return m_Ready && m_HDR->getDesc().width == width && m_HDR->getDesc().height == height; }
		nvrhi::ITexture* GetOutput() const { return m_Ready ? m_History[m_WriteIndex][0] : nullptr; }
		nvrhi::IFramebuffer* GetGuideFramebuffer() const { return m_GuideFramebuffer; }
		nvrhi::IFramebuffer* GetReactiveFramebuffer() const { return m_ReactiveFramebuffer; }
		nvrhi::IBindingSet* GetDrawBindings() const { return m_DrawBindings; }
		nvrhi::IGraphicsPipeline* GetGuidePipeline(bool doubleSided, bool mirrored) const;
		nvrhi::IGraphicsPipeline* GetReactivePipeline() const { return m_ReactivePipeline; }
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts() + m_Buffers.GetBackingAttempts(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_Buffers.GetAllocationAttempts() + m_ObjectAttempts; }
		const char* GetFallbackReason() const { return m_Reason; }

	private:
		friend class SceneRenderer;
		// Retained identity tokens distinguish new capture owners even when their object addresses are reused.
		const Ref<const void> m_OwnerToken = CreateRef<uint8_t>(0);
		AntiAliasingMode m_EffectiveMode = AntiAliasingMode::Off;
		std::string m_RenderFallback;
		uint32_t m_LastPasses = 0;
		struct ScaleFailure
		{
			glm::uvec2 Output;
			float Scale;
			bool WorldText;
			std::string Reason;
		};
		std::optional<ScaleFailure> m_FailedScale;
		struct MemoryFailure
		{
			glm::uvec2 InternalSize, OutputSize;
			uint32_t DrawCount;
			uint64_t OtherBytes;
		};
		std::optional<MemoryFailure> m_MemoryFailure;
		bool Fail(const char* reason);
		bool CreateBindings();
		GraphicsDevice& m_Graphics;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		PostEffectBuffers m_Buffers;
		TemporalState m_State;
		nvrhi::TextureHandle m_HDR, m_Depth;
		std::array<nvrhi::ITexture*, 4> m_Guides{};
		nvrhi::ITexture* m_Reactive = nullptr;
		// Color, packed normal/reactivity, exact ID, signed view depth.
		std::array<std::array<nvrhi::ITexture*, 4>, 2> m_History{};
		nvrhi::IBuffer* m_DrawBuffer = nullptr;
		uint32_t m_BufferCapacity = 0, m_WriteIndex = 0;
		nvrhi::BindingLayoutHandle m_DrawLayout, m_ResolveLayout;
		nvrhi::BindingSetHandle m_DrawBindings;
		std::array<nvrhi::BindingSetHandle, 2> m_ResolveBindings;
		nvrhi::FramebufferHandle m_GuideFramebuffer, m_ReactiveFramebuffer;
		std::array<nvrhi::FramebufferHandle, 2> m_ResolveFramebuffers;
		std::array<nvrhi::GraphicsPipelineHandle, 3> m_GuidePipelines;
		nvrhi::GraphicsPipelineHandle m_ReactivePipeline, m_ResolvePipeline;
		uint64_t m_ObjectAttempts = 0;
		bool m_Ready = false, m_ClearHistory = true;
		const char* m_Reason = "";
	};

}
