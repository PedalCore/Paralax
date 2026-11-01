#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Core/Reflection.h"
#include "Paralax/Math/AABB.h"
#include "Paralax/Renderer/EnvironmentLighting.h"
#include "Paralax/Renderer/GPUResourceCache.h"
#include "Paralax/Renderer/SceneRendererTypes.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <algorithm>
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
		BaseColor = 2
	};

	enum class ShadowQuality
	{
		Low = 0,    // 1024 texels per cascade
		Medium = 1, // 2048
		High = 2,   // 4096
	};

	// Renderer settings. Reflected (GetFields) so the editor UI, scene tooling and agents share one description.
	struct SceneRendererSettings
	{
		float Exposure = 1.0f;
		TonemapOperator Tonemapper = TonemapOperator::AgX;
		bool Shadows = true;
		ShadowQuality ShadowResolution = ShadowQuality::Medium;
		float ShadowDistance = 80.0f;
		// Penumbra size in world units (larger = softer shadows).
		float ShadowSoftness = 0.06f;
		// Fraction of each cascade blended into the next (0 disables blending).
		float ShadowCascadeBlend = 0.1f;
		// Environment intensity when the scene has no SkyLight (procedural sky lit by the sun).
		float DefaultEnvironmentIntensity = 0.65f;
		RenderDebugView DebugView = RenderDebugView::None;

		uint32_t GetShadowMapSize() const { return 1024u << std::clamp(static_cast<int>(ShadowResolution), 0, 2); }
		static std::span<const FieldInfo> GetFields();
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

		// Builds the view of a camera entity (CameraComponent + world transform) for the given aspect ratio.
		static std::optional<CameraView> FromEntity(Scene& scene, Entity entity, float aspectRatio);
		static CameraView LookAt(const glm::vec3& position, const glm::vec3& target, float verticalFOV, float aspectRatio, float nearPlane = 0.1f, float farPlane = 1000.0f);
	};

	struct SceneRenderStats
	{
		uint32_t DrawCalls = 0;
		uint32_t ShadowDrawCalls = 0;
		uint32_t CulledSubmeshes = 0;
		uint64_t Triangles = 0;
		uint32_t Lights = 0;
	};

	// Renders a Scene: cascaded soft shadows for the sun, forward PBR with point/spot lights and image-based lighting
	// (procedural sky or an HDR environment), sky background, alpha-blended surfaces and tone mapping into an 8-bit
	// sRGB framebuffer. Main-thread-only.
	class SceneRenderer
	{
	public:
		SceneRenderer(GraphicsDevice& device, AssetManager& assets);
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Creates pipelines, the BRDF LUT and default resources. Returns false (with an error logged) on failure.
		bool Initialize();

		// Renders the scene into output (a framebuffer with one 8-bit color attachment, e.g. a swapchain image).
		// Internal targets are resized to the output's size as needed.
		void Render(Scene& scene, const CameraView& camera, nvrhi::IFramebuffer* output);

		SceneRendererSettings& GetSettings() { return m_Settings; }
		const SceneRenderStats& GetStats() const { return m_Stats; }
		nvrhi::ITexture* GetHDRTarget() const { return m_HDRTarget; }
		// Temporary main-thread diagnostic views, owned by the renderer; empty before the first render.
		const EnvironmentMaps& GetEnvironmentMaps() const { return m_EnvironmentMaps; }
		// Temporary main-thread diagnostic view, owned by the renderer; null before initialization succeeds.
		nvrhi::ITexture* GetBRDFLut() const { return m_Environment.GetBRDFLut(); }
		GPUResourceCache& GetResourceCache() { return m_Resources; }

	private:
		struct DrawItem
		{
			const GPUMesh* Mesh = nullptr;
			uint32_t IndexOffset = 0;
			uint32_t IndexCount = 0;
			uint32_t VertexOffset = 0;
			glm::mat4 World{1.0f};
			nvrhi::IBindingSet* Material = nullptr;
			bool Blend = false;
			bool DoubleSided = false;
			bool AlphaMask = false;
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
			uint64_t LastValidatedFrame = 0;
		};

		bool CreatePipelines();
		void EnsureTargets(uint32_t width, uint32_t height);
		nvrhi::IGraphicsPipeline* GetTonemapPipeline(nvrhi::IFramebuffer* output);
		MaterialEntry* GetMaterial(AssetHandle handle);
		nvrhi::ITexture* ResolveTexture(AssetHandle handle, nvrhi::ITexture* fallback);
		void CollectDraws(Scene& scene, const CameraView& camera);
		void UpdateEnvironment(Scene& scene, const glm::vec3& sunDirection, const glm::vec3& sunColor);
		void ComputeCascades(const CameraView& camera, const glm::vec3& sunDirection, GPU::ViewConstants& view);
		void RenderShadows(const GPU::ViewConstants& view);
		void RenderForward(const GPU::ViewConstants& view);

	private:
		GraphicsDevice& m_GraphicsDevice;
		AssetManager& m_Assets;
		nvrhi::DeviceHandle m_Device;
		SceneRendererSettings m_Settings;
		SceneRenderStats m_Stats;
		GPUResourceCache m_Resources;
		EnvironmentLighting m_Environment;
		EnvironmentMaps m_EnvironmentMaps;
		bool m_Initialized = false;
		uint64_t m_FrameIndex = 0;

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
		nvrhi::SamplerHandle m_LinearWrap;
		nvrhi::SamplerHandle m_LinearClamp;
		nvrhi::SamplerHandle m_PointClamp;
		nvrhi::SamplerHandle m_ShadowSampler;
		nvrhi::BindingLayoutHandle m_ViewLayout;
		nvrhi::BindingLayoutHandle m_MaterialLayout;
		nvrhi::BindingLayoutHandle m_ShadowViewLayout;
		nvrhi::BindingLayoutHandle m_TonemapLayout;
		nvrhi::BindingSetHandle m_ViewBindings;
		nvrhi::BindingSetHandle m_ShadowViewBindings;
		nvrhi::BindingSetHandle m_TonemapBindings;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::InputLayoutHandle m_ShadowInputLayout;

		nvrhi::ShaderHandle m_ForwardVS, m_ForwardPS, m_ShadowVS, m_ShadowMaskPS, m_SkyVS, m_SkyPS, m_TonemapVS, m_TonemapPS;
		nvrhi::GraphicsPipelineHandle m_OpaquePipeline, m_OpaqueMirroredPipeline, m_OpaqueDoubleSidedPipeline, m_BlendPipeline, m_SkyPipeline;
		nvrhi::GraphicsPipelineHandle m_ShadowPipeline, m_ShadowMaskPipeline;
		std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> m_TonemapPipelines;

		std::unordered_map<uint64_t, MaterialEntry> m_Materials;
		std::vector<DrawItem> m_Draws;
		std::vector<GPU::Light> m_Lights;

		// Environment cache: the environment is only regenerated when its inputs change.
		uint64_t m_EnvironmentKey = 0;
		float m_EnvironmentIntensity = 1.0f;
		float m_EnvironmentRotation = 0.0f;
		bool m_ShowSky = true;
	};

}
