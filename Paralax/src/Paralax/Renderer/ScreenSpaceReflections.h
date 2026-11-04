#pragma once

#include "Paralax/Renderer/DepthHierarchy.h"
#include "Paralax/Renderer/ReflectionHistory.h"
#include "Paralax/Renderer/SceneRendererTypes.h"

#include <array>
#include <optional>
#include <string>

namespace Paralax {

	class RenderProfiler;

	struct ReflectionPixelInspection
	{
		uint64_t Frame = 0, Generation = 0;
		uint32_t Path = 0, Status = 0, Termination = 0, RefinementCells = 0, PlaneFlags = 0;
		glm::ivec2 TerminationCell{-1};
		glm::vec2 PlanePixel{0};
		float PlaneDistance = 0;
		glm::vec3 CandidateNormal{0}, TerminationNormal{0};
		float CandidateNormalDot = 0, CandidatePlaneOffset = 0;
		glm::uvec2 Size{0}, Pixel{0};
		// Candidate values are NaN when tracing never evaluated a covered level-zero candidate.
		glm::vec2 CandidateTexel{0};
		float Distance = 0, Confidence = 0, Residual = 0, Facing = 0, FilterWeight = 0;
	};

	struct ReflectionSettings
	{
		float MaxDistance = 30, Thickness = 0.1f, MaxRoughness = 0.8f;
		uint32_t MaxSteps = 96;
	};

	// Main-thread-only optional opaque reflection passes. Graphics/budget/factory outlive this owner.
	// Prepare is transactional: failure leaves original opaque rendering available, with a reason.
	// Borrowed inputs are retained while prepared; source radiance is copied before composition.
	class ScreenSpaceReflections
	{
	public:
		static constexpr uint32_t SampledTextureCount = 24;
		ScreenSpaceReflections(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~ScreenSpaceReflections();
		ScreenSpaceReflections(const ScreenSpaceReflections&) = delete;
		ScreenSpaceReflections& operator=(const ScreenSpaceReflections&) = delete;
		bool Prepare(bool enabled, nvrhi::ITexture* hdr, nvrhi::ITexture* depth, const std::array<nvrhi::ITexture*, 3>& deferredGuides,
					 nvrhi::IBuffer* viewConstants, nvrhi::ITexture* brdf, nvrhi::IInputLayout* input, nvrhi::IBindingLayout* view,
					 nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe, nvrhi::IBindingLayout* deferredLayout);
		void Release();
		void ClearGuides(nvrhi::ICommandList* commands);
		void Render(nvrhi::ICommandList* commands, const ReflectionSettings& settings, bool reflectionOnly, ReflectionHistory* history = nullptr, const glm::mat4& rasterViewProjection = glm::mat4(1), float historyWeight = 0.85f, RenderProfiler* profiler = nullptr);
		ReflectionHistory::Inputs GetHistoryInputs() const;
		// Commit only after the Render command list was submitted; stores exact raster camera/settings for probes.
		void CommitInspectionFrame(const GPU::ViewConstants& view, uint64_t frame, uint32_t path);
		// Synchronous diagnostic of one pixel in that completed frame. Does not alter images/history/stats.
		// Returns null on stale/unavailable input, invalid coordinates, allocation or readback failure.
		std::optional<ReflectionPixelInspection> InspectPixel(glm::uvec2 pixel, uint64_t expectedFrame);
		bool IsReady() const { return m_Ready; }
		bool NeedsGuides() const { return m_Ready && m_Forward; }
		nvrhi::IFramebuffer* GetOpaqueFramebuffer() const { return m_OpaqueFramebuffer; }
		nvrhi::IFramebuffer* GetGuideFramebuffer() const { return m_GuideFramebuffer; }
		nvrhi::IGraphicsPipeline* GetOpaquePipeline(bool doubleSided, bool mirrored) const { return m_OpaquePipelines[m_Forward ? (doubleSided ? 2 : mirrored ? 1
																																							  : 0)
																																: 0]; }
		nvrhi::IGraphicsPipeline* GetGuidePipeline(bool doubleSided, bool mirrored) const { return m_GuidePipelines[doubleSided ? 2 : mirrored ? 1
																																			   : 0]; }
		uint64_t GetOwnedBytes() const { return m_Textures.GetOwnedBytes() + m_Hierarchy.GetOwnedBytes(); }
		uint64_t GetAllocationAttempts() const { return m_Textures.GetAllocationAttempts() + m_Hierarchy.GetAllocationAttempts() + m_ObjectAttempts; }
		uint64_t GetBackingAttempts() const { return m_Textures.GetBackingAttempts() + m_Hierarchy.GetBackingAttempts(); }
		uint32_t GetHierarchyLevels() const { return m_Hierarchy.GetLevelCount(); }
		const std::string& GetFallbackReason() const { return m_Reason; }

	private:
		struct ReflectionConstants
		{
			glm::vec4 Settings{0};
			glm::uvec4 Dimensions{0}, Probe{0};
		};
		ReflectionConstants m_RecordedConstants;
		GPU::ViewConstants m_RecordedView{};
		uint64_t m_Generation = 0, m_RecordedFrame = 0;
		uint32_t m_RecordedPath = 0;
		bool m_PendingInspectionFrame = false, m_InspectionReady = false;
		bool Fail(const char* reason);
		GraphicsDevice& m_Graphics;
		PostEffectBudget& m_Budget;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		PostEffectTextures m_Textures;
		DepthHierarchy m_Hierarchy;
		nvrhi::TextureHandle m_HDR, m_Depth;
		std::array<nvrhi::TextureHandle, 3> m_Guides;
		nvrhi::ITexture* m_Environment = nullptr;
		nvrhi::ITexture* m_Source = nullptr;
		nvrhi::ITexture* m_Color = nullptr;
		nvrhi::ITexture* m_Hit = nullptr;
		nvrhi::FramebufferHandle m_OpaqueFramebuffer, m_GuideFramebuffer, m_TraceFramebuffer, m_ComposeFramebuffer;
		std::array<nvrhi::GraphicsPipelineHandle, 3> m_OpaquePipelines, m_GuidePipelines;
		nvrhi::GraphicsPipelineHandle m_TracePipeline, m_ComposePipeline;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_TraceBindings, m_ComposeBindings;
		nvrhi::SamplerHandle m_Linear;
		struct InputKey
		{
			nvrhi::TextureHandle HDR, Depth;
			std::array<nvrhi::TextureHandle, 3> Guides;
			nvrhi::BufferHandle ViewConstants;
			nvrhi::TextureHandle BRDF;
			nvrhi::InputLayoutHandle Input;
			nvrhi::BindingLayoutHandle View, Material, Probe, Deferred;
			bool operator==(const InputKey&) const = default;
		};
		struct FailedRequest
		{
			InputKey Inputs;
			uint64_t OtherBytes = 0;
		};
		Ref<const void> m_HistoryInputToken;
		std::optional<InputKey> m_Inputs;
		std::optional<FailedRequest> m_Failed;
		uint64_t m_ObjectAttempts = 0;
		bool m_Ready = false, m_Forward = false;
		std::string m_Reason;
	};

}
