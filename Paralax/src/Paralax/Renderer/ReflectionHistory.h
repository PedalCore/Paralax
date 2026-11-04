#pragma once

#include "Paralax/Renderer/PostEffectResources.h"
#include "Paralax/Renderer/TemporalState.h"

#include <array>
#include <optional>
#include <string>

namespace Paralax {

	// Main-thread-only history for one view. Graphics, shared budget and optional factory outlive this owner.
	// Optional allocation failure leaves stateless reflections usable. Commit follows successful submission.
	class ReflectionHistory
	{
	public:
		struct Inputs
		{
			// Candidate, geometric depth, packed mapped/shadow normals, receiver guide, hit world, opaque color, exact environment.
			std::array<nvrhi::TextureHandle, 7> Textures;
			nvrhi::TextureHandle HDR;
			nvrhi::BufferHandle ViewConstants;
			// A new tiny identity for every recreated scratch set; retaining it never retains GPU backing.
			Ref<const void> Generation;
			bool operator==(const Inputs&) const = default;
		};
		ReflectionHistory(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~ReflectionHistory();
		ReflectionHistory(const ReflectionHistory&) = delete;
		ReflectionHistory& operator=(const ReflectionHistory&) = delete;
		bool Prepare(const Inputs& inputs);
		void Release();
		// Drops scratch references while retaining completed per-view history, including its camera metadata.
		void DetachInputs();
		void ResolveAndCompose(nvrhi::ICommandList* commands, const glm::mat4& rasterViewProjection, float weight, float thickness, bool reflectionOnly);
		void Commit();
		TemporalState& GetState() { return m_State; }
		const TemporalState& GetState() const { return m_State; }
		bool IsReady() const { return m_Ready; }
		bool MatchesSize(glm::uvec2 size) const { return m_Ready && m_Size == size; }
		const std::string& GetFallbackReason() const { return m_Reason; }
		uint64_t GetOwnedBytes() const { return m_Textures.GetOwnedBytes() + m_Buffers.GetOwnedBytes(); }
		uint64_t GetAllocationAttempts() const { return m_ObjectAttempts + m_Textures.GetAllocationAttempts() + m_Buffers.GetAllocationAttempts(); }
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts() + m_Buffers.GetBackingAttempts(); }

	private:
		friend class SceneRenderer;
		std::optional<uint64_t> m_ResourcePlan;
		bool Fail(const char* reason);
		void ReleaseResources();
		GraphicsDevice& m_Graphics;
		PostEffectBudget& m_Budget;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		PostEffectBuffers m_Buffers;
		TemporalState m_State;
		std::optional<Inputs> m_Inputs;
		struct Failure
		{
			// Retry identities never own scratch resources across private-view detachment.
			std::array<uintptr_t, 9> InputIDs;
			Ref<const void> Generation;
			glm::uvec2 Size;
			uint64_t OtherBytes;
		};
		std::optional<Failure> m_Failure;
		glm::uvec2 m_Size{0};
		nvrhi::Format m_OutputFormat = nvrhi::Format::UNKNOWN;
		std::array<std::array<nvrhi::ITexture*, 4>, 2> m_History{};
		std::array<nvrhi::FramebufferHandle, 2> m_Framebuffers;
		nvrhi::FramebufferHandle m_ComposeFramebuffer;
		nvrhi::BindingLayoutHandle m_Layout, m_ComposeLayout;
		std::array<nvrhi::BindingSetHandle, 2> m_Bindings, m_ComposeBindings;
		nvrhi::GraphicsPipelineHandle m_Pipeline, m_ComposePipeline;
		nvrhi::IBuffer* m_Parameters = nullptr;
		glm::mat4 m_PreviousRaster{1}, m_PendingRaster{1};
		uint32_t m_WriteIndex = 0;
		uint64_t m_ObjectAttempts = 0;
		bool m_Ready = false, m_Clear = true, m_Recorded = false;
		std::string m_Reason;
	};

}
