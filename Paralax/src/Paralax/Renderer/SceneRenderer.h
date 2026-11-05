#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Core/Reflection.h"
#include "Paralax/Math/AABB.h"
#include "Paralax/Renderer/DeferredSurfaces.h"
#include "Paralax/Renderer/AmbientOcclusion.h"
#include "Paralax/Renderer/Bloom.h"
#include "Paralax/Renderer/EnvironmentLighting.h"
#include "Paralax/Renderer/GPUResourceCache.h"
#include "Paralax/Renderer/LocalShadowMaps.h"
#include "Paralax/Renderer/LocalShadowSelection.h"
#include "Paralax/Renderer/ProbeGI.h"
#include "Paralax/Renderer/RenderProfiler.h"
#include "Paralax/Renderer/ClusteredLighting.h"
#include "Paralax/Renderer/RenderScale.h"
#include "Paralax/Renderer/SceneRendererTypes.h"
#include "Paralax/Renderer/TextRenderer.h"
#include "Paralax/Renderer/TemporalAA.h"
#include "Paralax/Renderer/SpatialAA.h"
#include "Paralax/Renderer/FXAA.h"
#include "Paralax/Renderer/ScreenSpaceReflections.h"
#include "Paralax/Renderer/VisibilitySurfaces.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Paralax {

	class AssetManager;
	class Entity;
	class GraphicsDevice;
	class MaterialAsset;
	class Scene;

	enum class RenderPath
	{
		Forward = 0,
		Deferred = 1,
		Visibility = 2
	};

	enum class AmbientOcclusionQuality
	{
		Low = 0,
		Medium = 1,
		High = 2
	};

	enum class TonemapOperator
	{
		AgX = 0,
		ACES = 1,
		None = 2
	};

	enum class RenderDebugView
	{
		None = 0,
		Normals = 1,
		BaseColor = 2,
		Indirect = 3,
		GICoverage = 4,
		DirectDiffuse = 5,
		DirectSpecular = 6,
		EnvironmentDiffuse = 7,
		EnvironmentSpecular = 8,
		Emissive = 9,
		IndirectDiffuse = 10,
		GIWeightSum = 11,
		GIValidNeighbours = 12,
		GIProbeWeight = 13,
		GIMeanDistance = 14,
		GISecondMoment = 15,
		GIVariance = 16,
		LocalShadow = 17,
		AmbientOcclusion = 18,
		AmbientFill = 19,
		Reflections = 20
	};

	enum class ShadowQuality
	{
		Low = 0,    // 1024 texels per cascade
		Medium = 1, // 2048
		High = 2,   // 4096
	};

	enum class LocalShadowQuality
	{
		Low = 0,    // 512 texels per face
		Medium = 1, // 1024
		High = 2    // 2048
	};

	// Renderer settings. Reflected (GetFields) so the editor UI, scene tooling and agents share one description.
	struct SceneRendererSettings
	{
		RenderPath Path = RenderPath::Forward;
		bool ProfilingEnabled = false;
		bool ClusteredLightingEnabled = false;
		float InternalRenderScale = 1.0f;
		AntiAliasingMode AntiAliasing = AntiAliasingMode::Off;
		float AAHistoryWeight = 0.9f;
		bool SSREnabled = false, SSRTemporalEnabled = true;
		float SSRHistoryWeight = 0.85f;
		float SSRMaxDistance = 30, SSRThickness = 0.1f, SSRMaxRoughness = 0.8f;
		int SSRMaxSteps = 96;
		bool SSAOEnabled = false;
		float SSAORadius = 0.5f;
		float SSAOBias = 0.02f;
		float SSAOStrength = 1.0f;
		// 0/1/2 select 8/16/32 fixed hemisphere samples.
		AmbientOcclusionQuality SSAOQuality = AmbientOcclusionQuality::Medium;
		glm::vec3 AmbientFillColor{1.0f};
		float AmbientFillIntensity = 0.0f;
		bool BloomEnabled = false;
		float BloomThreshold = 1.0f;
		float BloomSoftKnee = 0.5f;
		float BloomIntensity = 0.0f;
		int BloomLevels = 5;
		float Exposure = 1.0f;
		TonemapOperator Tonemapper = TonemapOperator::AgX;
		bool Shadows = true;
		ShadowQuality ShadowResolution = ShadowQuality::Medium;
		float ShadowDistance = 80.0f;
		// Penumbra size in world units (larger = softer shadows).
		float ShadowSoftness = 0.06f;
		// Fraction of each cascade blended into the next (0 disables blending).
		float ShadowCascadeBlend = 0.1f;
		LocalShadowQuality LocalShadowResolution = LocalShadowQuality::Medium;
		int MaxShadowedLocalLights = 4;
		float LocalShadowSoftness = 0.06f;
		// Environment intensity when the scene has no SkyLight (procedural sky lit by the sun).
		float DefaultEnvironmentIntensity = 0.65f;
		bool GIEnabled = true;
		bool GIFreeze = false;
		bool GIVisibility = true;
		bool GIBounceFeedback = true;
		bool EnvironmentSpecular = true;
		int GIDebugProbe = -1;
		float GIDebugScale = 1.0f;
		bool GIShowProbes = false;
		// Rays traced per updated probe (32–256).
		int GIRaysPerProbe = 64;
		// Round-robin update budget (1–1728); initialization fills the whole volume.
		int GIProbesPerFrame = 32;
		float GIHysteresis = 0.85f;
		// Receiver normal offset in metres (0–0.05).
		float GINormalBias = 0.05f;
		// View offset as a fraction of spacing (0–0.5), capped at half the view-ray distance.
		float GIViewBias = 0.2f;
		RenderDebugView DebugView = RenderDebugView::None;

		uint32_t GetShadowMapSize() const { return 1024u << std::clamp(static_cast<int>(ShadowResolution), 0, 2); }
		uint32_t GetLocalShadowMapSize() const { return 512u << std::clamp(static_cast<int>(LocalShadowResolution), 0, 2); }
		static std::span<const FieldInfo> GetFields();
	};

	struct GIState
	{
		bool Enabled = false, Supported = false, Frozen = false, HistoryValid = false, Available = false;
		uint32_t Updates = 0, Queued = 0;
		glm::vec3 Origin{0.0f};
		float Spacing = 0.0f;
		glm::uvec3 Counts{0};
		uint64_t Frame = 0;
	};

	// A camera to render from (view and projection are D3D-style, reversed Z).
	struct CameraView
	{
		glm::mat4 View{1.0f};
		glm::mat4 Projection{1.0f};
		glm::vec3 Position{0.0f};
		float Near = 0.1f;
		float Far = 1000.0f;
		float VerticalFOV = glm::radians(60.0f); // Perspective cameras (used for shadow cascade fitting).
		bool Orthographic = false;
		uint64_t Identity = 0; // Camera entity UUID, or zero for an explicitly controlled view.

		// Builds the view of a camera entity (CameraComponent + world transform) for the given aspect ratio.
		static std::optional<CameraView> FromEntity(Scene& scene, Entity entity, float aspectRatio);
		static CameraView LookAt(const glm::vec3& position, const glm::vec3& target, float verticalFOV, float aspectRatio, float nearPlane = 0.1f, float farPlane = 1000.0f);
	};

	struct SceneRenderStats
	{
		uint64_t RenderFrame = 0;
		bool ClusteredLightingRequested = false, ClusteredLightingActive = false;
		std::string ClusteredLightingFallbackReason;
		glm::uvec3 ClusterGrid{};
		uint32_t ClusterCells = 0, ClusterDispatches = 0;
		uint64_t ClusterLogicalBytes = 0, ClusterOwnedBytes = 0, ClusterAllocationAttempts = 0, ClusterBackingAttempts = 0;
		uint64_t ClusterPrefixUploadBytes = 0, ClusterLightTestsUpperBound = 0;
		AntiAliasingMode RequestedAA = AntiAliasingMode::Off, EffectiveAA = AntiAliasingMode::Off;
		std::string AAFallbackReason, AAResetReason;
		bool AAHistoryValid = false;
		uint64_t AACompletedFrames = 0;
		uint32_t AASampleIndex = 0, AAPasses = 0;
		RenderPath RequestedPath = RenderPath::Forward, EffectivePath = RenderPath::Forward;
		std::string PathFallbackReason;
		float RequestedRenderScale = 1.0f, EffectiveRenderScale = 1.0f;
		uint32_t InternalWidth = 0, InternalHeight = 0, OutputWidth = 0, OutputHeight = 0, NativeDepthPasses = 0;
		std::string ScaleFallbackReason;
		bool ProfilingEnabled = false;
		std::optional<double> CPURenderMilliseconds;
		std::optional<RenderTimingSample> GPUTiming;
		std::optional<GraphicsDeviceInfo> ProfilingDevice;
		std::string GPUProfilingStatus = "disabled";
		uint32_t GPUProfilingPending = 0;
		uint64_t GPUProfilingSkipped = 0, GPUProfilingInvalid = 0;
		std::string GPUProfilingLastResult = "none";
		bool BloomActive = false;
		bool SSRActive = false;
		std::string SSRFallbackReason;
		uint32_t SSRPasses = 0, SSRHierarchyLevels = 0;
		uint64_t SSROwnedBytes = 0, SSRHistoryCompletedFrames = 0;
		bool SSRHistoryActive = false, SSRHistoryValid = false, SSRHistoryEligible = false;
		std::string SSRHistoryReason;
		std::string BloomFallbackReason;
		uint32_t BloomPasses = 0;
		bool SSAOActive = false;
		const char* SSAOFallbackReason = "";
		uint64_t EffectOwnedBytes = 0, EffectPeakBytes = 0, EffectAllocationAttempts = 0;
		uint32_t SSAOPasses = 0;
		uint64_t SurfaceOwnedBytes = 0, SurfaceAllocationAttempts = 0;
		uint32_t SurfaceResolveDrawCalls = 0, VisibilityResolveDrawCalls = 0;
		uint64_t VisibilityOwnedBytes = 0, VisibilityGeometryBytes = 0, VisibilityAllocationAttempts = 0;
		uint32_t DrawCalls = 0;
		uint32_t ShadowDrawCalls = 0;
		uint32_t CulledSubmeshes = 0;
		uint32_t TextGlyphs = 0;
		uint64_t Triangles = 0;
		uint32_t Lights = 0;
		uint32_t RequestedLocalShadows = 0, ShadowedLocalLights = 0, LocalShadowLayers = 0, LocalShadowCapacity = 0;
		uint32_t LocalShadowDrawCalls = 0;
		uint64_t LocalShadowAllocationAttempts = 0;
		std::vector<uint64_t> ShadowedLocalLightIDs;
		// 1 when this frame regenerated the BRDF LUT and environment maps after allocating render targets (see Render).
		uint32_t StaticLightingGenerations = 0;
	};

	// Renders a Scene: cascaded soft shadows for the sun, forward PBR with point/spot lights and image-based lighting
	// (procedural sky or an HDR environment), sky background, alpha-blended surfaces and tone mapping into an 8-bit
	// sRGB framebuffer. Main-thread-only.
	class SceneRenderer
	{
	public:
		// The optional allocation factory is non-owning and must outlive the renderer.
		SceneRenderer(GraphicsDevice& device, AssetManager& assets, LocalShadowResourceFactory* shadowFactory = nullptr, DeferredResourceFactory* deferredFactory = nullptr, PostEffectResourceFactory* effectFactory = nullptr, uint64_t effectBudgetCapacity = PostEffectBudget::DefaultCapacity);
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Creates pipelines, the BRDF LUT and default resources. Returns false (with an error logged) on failure.
		bool Initialize();

		// Renders the scene into output (a framebuffer with one 8-bit color attachment, e.g. a swapchain image).
		// Internal targets are resized to the output's size as needed.
		// Shadow history is optional. Temporal history defaults to the renderer-owned view; captures pass a fresh owner.
		void Render(Scene& scene, const CameraView& camera, nvrhi::IFramebuffer* output, LocalShadowHistory* shadowHistory = nullptr, TemporalAA* temporalHistory = nullptr, ReflectionHistory* reflectionHistory = nullptr);
		// The returned independent view/capture history must be destroyed before this renderer.
		Scope<TemporalAA> CreateTemporalHistory();
		// Private view histories retain no scene objects. The returned owner must die before this renderer.
		Scope<ReflectionHistory> CreateReflectionHistory();
		ReflectionHistory& GetDefaultReflectionHistory() { return m_ReflectionHistory; }
		void ResetTemporalHistories(const char* reason);
		TemporalAA& GetDefaultTemporalHistory() { return m_TemporalAA; }

		SceneRendererSettings& GetSettings() { return m_Settings; }
		const SceneRenderStats& GetStats() const { return m_Stats; }
		// The split-sum BRDF lookup table (128 x 128, RG16_FLOAT) and the current environment maps, for diagnostics and
		// tests. Both are rewritten by Render; see StaticLightingGenerations.
		nvrhi::ITexture* GetBRDFLut() const { return m_Environment.GetBRDFLut(); }
		const EnvironmentMaps& GetEnvironmentMaps() const { return m_EnvironmentMaps; }
		nvrhi::ITexture* GetHDRTarget() const { return m_HDRTarget; }
		GPUResourceCache& GetResourceCache() { return m_Resources; }
		// Diagnostic state is owned by this renderer and updated on the main thread; these queries do not render.
		GIState GetGIState() const;
		// Reset schedules one full refill on the next render, even while frozen. False if GI is unavailable.
		bool ResetGIHistory();
		// Queue 1..1024 deterministic updates while frozen. False if unavailable or the bounded queue would overflow.
		bool QueueGIUpdates(uint32_t frames);
		// Main-thread synchronous diagnostic of the latest SSR frame; returns owned data or nullopt on failure.
		// Allocations schedule static-lighting recovery on the next render without changing this frame or history.
		std::optional<ReflectionPixelInspection> InspectReflectionPixel(glm::uvec2 pixel, uint64_t frame)
		{
			const auto before = m_Reflections.GetInspectionBackingAttempts();
			auto result = m_Reflections.InspectPixel(pixel, frame);
			m_StaticLightingDirty |= before != m_Reflections.GetInspectionBackingAttempts();
			return result;
		}
		// Synchronous, main-thread-only GPU inspection between renders; returns owned data or nullopt.
		std::optional<GPU::ProbeInspection> InspectGI(glm::vec3 position, glm::vec3 normal, glm::vec3 view, float viewDistance = 1000000.0f)
		{
			return m_Settings.GIEnabled ? m_ProbeGI.Inspect(position, normal, view, viewDistance) : std::nullopt;
		}

	private:
		struct DrawItem
		{
			const GPUMesh* Mesh = nullptr;
			uint64_t EntityID = 0, MeshID = 0, TemporalVersion = 0;
			uint32_t TemporalIndex = 0;
			uint32_t SubmeshOrdinal = 0, TriangleBase = 0;
			uint32_t IndexOffset = 0;
			uint32_t IndexCount = 0;
			uint32_t VertexOffset = 0;
			glm::mat4 World{1.0f};
			nvrhi::IBindingSet* Material = nullptr;
			bool Blend = false;
			bool DoubleSided = false;
			bool AlphaMask = false;
			bool CompactSurface = true;
			bool CastShadows = true;
			bool Visible = true;   // Inside the camera frustum (otherwise only drawn into shadow maps).
			bool Mirrored = false; // Negative-determinant transform (reversed winding).
			AABB Bounds;
			float ViewDepth = 0.0f;
		};

		struct MaterialEntry
		{
			nvrhi::BufferHandle Constants;
			nvrhi::BindingSetHandle BindingSet;
			uint32_t Version = 0;
			uint64_t TextureVersionHash = 0;
			bool Blend = false;
			bool DoubleSided = false;
			bool AlphaMask = false;
			bool CompactSurface = true;
			uint64_t LastValidatedFrame = 0;
		};

		bool CreatePipelines();
		bool EnsureTargets(uint32_t width, uint32_t height, bool supersampled = false);
		nvrhi::IGraphicsPipeline* GetTonemapPipeline(nvrhi::IFramebuffer* output);
		void RenderProbeMarkers(const CameraView& camera, nvrhi::IFramebuffer* output);
		MaterialEntry* GetMaterial(AssetHandle handle);
		nvrhi::ITexture* ResolveTexture(AssetHandle handle, nvrhi::ITexture* fallback);
		void CollectDraws(Scene& scene, const CameraView& camera);
		void UpdateEnvironment(Scene& scene, const glm::vec3& sunDirection, const glm::vec3& sunColor);
		void ComputeCascades(const CameraView& camera, const glm::vec3& sunDirection, GPU::ViewConstants& view);
		void RenderShadows(const GPU::ViewConstants& view);
		void CollectLights(Scene& scene, const CameraView& camera, LocalShadowHistory* history);
		void RenderLocalShadows();
		void RenderForward(const GPU::ViewConstants& view, ReflectionHistory* reflectionHistory);
		bool PrepareVisibilityDraws();
		void RenderVisibilityOpaque();
		void RenderTemporalGuides(TemporalAA& history);
		void InvalidateViewBindings();

	private:
		GraphicsDevice& m_GraphicsDevice;
		AssetManager& m_Assets;
		nvrhi::DeviceHandle m_Device;
		SceneRendererSettings m_Settings;
		SceneRenderStats m_Stats;
		GPUResourceCache m_Resources;
		EnvironmentLighting m_Environment;
		TextRenderer m_Text;
		ProbeGI m_ProbeGI;
		LocalShadowMaps m_LocalShadowMaps;
		RenderProfiler m_Profiler;
		PostEffectBudget m_EffectBudget;
		Scope<PostEffectTextures> m_SupersampleTargets;
		uint64_t m_SupersampleAllocationAttempts = 0;
		std::string m_TargetFailure;
		AmbientOcclusion m_AmbientOcclusion;
		Bloom m_Bloom;
		RenderScale m_RenderScale;
		TemporalAA m_TemporalAA;
		SpatialAA m_SpatialAA;
		FXAA m_FXAA;
		ClusteredLighting m_Clusters;
		std::string m_ReportedClusterFailure;
		ScreenSpaceReflections m_Reflections;
		ReflectionHistory m_ReflectionHistory;
		PostEffectResourceFactory* m_EffectFactory;
		uint64_t m_TemporalResetGeneration = 0;
		uint64_t m_ReflectionPlanKey = 0;
		bool m_ReflectionWasRequested = false;
		Ref<const void> m_ReflectionAAOwner;
		DeferredSurfaces m_Deferred;
		VisibilitySurfaces m_Visibility;
		LocalShadowResourceFactory* m_ShadowFactory;
		EnvironmentMaps m_EnvironmentMaps;
		bool m_Initialized = false;
		uint64_t m_FrameIndex = 0;
		bool m_ReportedMultipleGIVolumes = false;
		bool m_ReportedGIFailure = false;
		bool m_ReportedLocalPipelineFailure = false;
		bool m_ReportedLocalCapacity = false;
		bool m_ReportedSurfaceRange = false;
		uint32_t m_QueuedGIUpdates = 0;

		nvrhi::CommandListHandle m_CommandList;

		// Targets.
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		nvrhi::TextureHandle m_HDRTarget;
		nvrhi::TextureHandle m_DepthTarget;
		nvrhi::FramebufferHandle m_HDRFramebuffer;
		nvrhi::TextureHandle m_ShadowMap;
		uint32_t m_ShadowMapSize = 0;
		std::vector<nvrhi::FramebufferHandle> m_ShadowFramebuffers;

		// Shared state.
		nvrhi::BufferHandle m_ViewConstants;
		nvrhi::BufferHandle m_LightBuffer;
		nvrhi::BufferHandle m_LocalShadowBuffer;
		nvrhi::SamplerHandle m_LinearWrap;
		nvrhi::SamplerHandle m_LinearClamp;
		nvrhi::SamplerHandle m_PointClamp;
		nvrhi::SamplerHandle m_ShadowSampler;
		nvrhi::SamplerHandle m_LocalShadowSampler;
		nvrhi::BindingLayoutHandle m_ViewLayout;
		nvrhi::BindingLayoutHandle m_MaterialLayout;
		nvrhi::BindingLayoutHandle m_ShadowViewLayout;
		nvrhi::BindingLayoutHandle m_TonemapLayout;
		nvrhi::BindingLayoutHandle m_ProbeMarkerLayout;
		nvrhi::ShaderHandle m_ProbeMarkerVS, m_ProbeMarkerPS;
		std::array<nvrhi::BindingSetHandle, 2> m_ProbeMarkerBindings;
		uint32_t m_NextProbeMarkerBinding = 0;
		std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> m_ProbeMarkerPipelines;
		nvrhi::BindingSetHandle m_ViewBindings;
		nvrhi::IBindingSet* m_ActiveViewBindings = nullptr; // Owned by baseline or m_Clusters for this frame.
		nvrhi::BindingSetHandle m_ShadowViewBindings;
		nvrhi::BindingSetHandle m_TonemapBindings;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::InputLayoutHandle m_ShadowInputLayout;

		nvrhi::ShaderHandle m_ForwardVS, m_ForwardPS, m_ShadowVS, m_LocalShadowVS, m_ShadowPS, m_ShadowMaskPS, m_LocalShadowPS, m_LocalShadowMaskPS, m_SkyVS, m_SkyPS, m_TonemapVS, m_TonemapPS;
		nvrhi::GraphicsPipelineHandle m_OpaquePipeline, m_OpaqueMirroredPipeline, m_OpaqueDoubleSidedPipeline, m_BlendPipeline, m_SkyPipeline;
		nvrhi::GraphicsPipelineHandle m_ShadowPipeline, m_ShadowMaskPipeline;
		nvrhi::GraphicsPipelineHandle m_LocalShadowPipeline, m_LocalShadowMaskPipeline;
		std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> m_TonemapPipelines;

		std::unordered_map<uint64_t, MaterialEntry> m_Materials;
		std::vector<DrawItem> m_Draws;
		std::vector<GPU::Light> m_Lights;
		std::vector<GPU::LocalShadowSlot> m_LocalShadowSlots;

		// Environment cache: the environment is only regenerated when its inputs change.
		uint64_t m_EnvironmentKey = 0;
		// Set by EnsureTargets when it (re)allocates; the same frame regenerates the static lighting textures (Render
		// re-dispatches the LUT, UpdateEnvironment rebuilds the maps and clears the flag).
		bool m_StaticLightingDirty = false;
		float m_EnvironmentIntensity = 1.0f;
		float m_EnvironmentRotation = 0.0f;
		bool m_ShowSky = true;
	};

}
