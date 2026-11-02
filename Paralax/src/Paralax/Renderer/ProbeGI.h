#pragma once

#include "Paralax/Renderer/GraphicsDevice.h"
#include "Paralax/Renderer/ProbeTraceScene.h"
#include "Paralax/Renderer/SceneRendererTypes.h"

#include <nvrhi/nvrhi.h>

#include <optional>

namespace Paralax {

	// Resource-creation policy for the probe cache. Main-thread-only, uses the supplied device, returns owned
	// handles or nullptr on failure. Override to enforce an application resource budget or exercise recovery.
	class ProbeGIResourceFactory
	{
	public:
		virtual ~ProbeGIResourceFactory() = default;
		virtual nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc) { return device->createBuffer(desc); }
		virtual nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) { return device->createTexture(desc); }
		virtual nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout)
		{
			return device->createBindingSet(desc, layout);
		}
	};

	// Update accepts finite origins within +/-1e6 metres, spacing 0.1–100 metres, counts 2–12 per axis,
	// rays 32–256, update budget 1–1728 and hysteresis 0–0.99. Normal bias is 0–half the spacing in metres;
	// view bias is 0–0.5 of spacing. Adapter buffer and layer limits may impose smaller volumes.
	struct ProbeGISpecification
	{
		glm::vec3 Origin{0.0f};
		float Spacing = 1.0f;
		glm::uvec3 Counts{4};
		uint32_t RaysPerProbe = 64;
		uint32_t ProbesPerFrame = 32;
		float Hysteresis = 0.85f;
		float NormalBias = 0.05f;
		float ViewBias = 0.2f; // Fraction of probe spacing; bounded by the available view-ray distance.
		bool FreezeUpdates = false;
		bool VisibilityWeighting = true;
		bool BounceFeedback = true;
		int DebugProbe = -1;
	};

	namespace GPU {

		inline constexpr uint32_t ProbeDistanceTileSize = 32;

		struct ProbeGIConstants
		{
			glm::vec4 Origin{0.0f};
			glm::vec4 Spacing{1.0f};
			glm::uvec4 Grid{0};
			glm::uvec4 Update{0};
			glm::uvec4 Scene{0};
			glm::vec4 Tuning{0.0f};
			glm::vec4 Diagnostics{-1.0f, 1.0f, 1.0f, 0.0f};
		};
		static_assert(sizeof(ProbeGIConstants) == 112);

		struct ProbeVisibilityRay
		{
			glm::vec4 OriginLength{};        // offset start, tested segment length
			glm::vec4 EndOffset{};           // trimmed endpoint, endpoint epsilon in world units
			glm::vec4 HitPositionDistance{}; // first hit (or endpoint), distance from offset start
			glm::uvec4 Metadata{};           // triangle, material (UINT_MAX if clear), blocked, flags (bit 0 backface, bit 1 indeterminate)
		};
		struct ProbeNeighbourInspection
		{
			glm::vec4 PositionDistance{};
			glm::vec4 Moments{};          // mean, second moment, raw variance, clamped variance
			glm::vec4 Weights{};          // trilinear, facing, visibility, unnormalized weight
			glm::vec4 Sampling{};         // angular mass, expected mass, sampling confidence, reserved
			glm::vec4 IrradianceWeight{}; // directional irradiance / PI, normalized weight
			glm::uvec4 Metadata{};        // probe index, active, reserved, reserved
			ProbeVisibilityRay SurfaceRay, BiasedRay;
		};
		struct ProbeInspection
		{
			glm::vec4 Irradiance{}; // RGB irradiance / PI, volume coverage
			glm::vec4 Summary{};    // raw weight, active neighbours, active support, full support
			// RGB irradiance / PI with unchanged support normalization; W is the new raw weight sum.
			glm::vec4 SurfaceRejected{}, SurfaceExact{}, BiasedRejected{}, BiasedExact{};
			ProbeNeighbourInspection Neighbours[8];
		};
		static_assert(sizeof(ProbeVisibilityRay) == 64);
		static_assert(sizeof(ProbeNeighbourInspection) == 224);
		static_assert(sizeof(ProbeInspection) == 1888);
	}

	// Visibility-aware diffuse irradiance cache with portable compute tracing. Owns all GPU resources and its
	// CPU scene snapshot; main-thread-only. Commands are recorded into the caller's open command list. The first
	// update fills the volume, later updates respect ProbesPerFrame. Returns false on invalid input/allocation
	// failure; callers must Disable on failure. Radiance is independent of screen visibility and camera motion.
	class ProbeGI
	{
	public:
		// A supplied resource factory is non-owning and must outlive this cache.
		ProbeGI(nvrhi::IDevice* device, const GraphicsDeviceLimits& limits, ProbeGIResourceFactory* factory = nullptr);
		ProbeGI(const ProbeGI&) = delete;
		ProbeGI& operator=(const ProbeGI&) = delete;

		bool Initialize(nvrhi::ICommandList* commands);
		// Unsupported adapters initialize a valid disabled forward binding; Update returns false.
		bool IsSupported() const { return m_Supported; }
		// True when the last failed Update already reported a specific scene/buffer cause (possibly memoized).
		bool HasReportedUpdateFailure() const { return m_ReportedUpdateFailure; }
		bool Update(Scene& scene, AssetManager& assets, const ProbeGISpecification& specification, nvrhi::ICommandList* commands,
					nvrhi::IBuffer* viewConstants, nvrhi::IBuffer* lights, nvrhi::ITexture* environment, uint32_t lightCount);
		// Disables sampling and discards history. Bindings remain valid for the forward pipeline.
		void Disable(nvrhi::ICommandList* commands);
		nvrhi::IBindingLayout* GetLayout() const { return m_ForwardLayout; }
		nvrhi::IBindingSet* GetBindings() const { return m_ForwardBindings[m_Current]; }
		nvrhi::ITexture* GetIrradiance() const { return m_Irradiance[m_Current]; }
		nvrhi::ITexture* GetDistance() const { return m_Distance[m_Current]; }
		const GPU::ProbeGIConstants& GetConstants() const { return m_Parameters; }
		uint32_t GetUpdateCount() const { return m_Frame; }
		bool HasHistory() const { return m_HasHistory; }
		// The next update refills the volume, including when updates are frozen. No GPU work is recorded here.
		void ResetHistory()
		{
			m_HasHistory = false;
			m_Frame = 0;
			m_Cursor = 0;
		}
		// Synchronous GPU inspection using the shading reconstruction. Main-thread-only, between recording calls;
		// returns owned data or nullopt for unavailable history, invalid vectors or allocation/readback failure.
		std::optional<GPU::ProbeInspection> Inspect(glm::vec3 position, glm::vec3 normal, glm::vec3 view, float viewDistance = 1000000.0f);

	private:
		bool CreateVolumes(uint32_t count, nvrhi::ICommandList* commands);
		bool UploadScene(nvrhi::ICommandList* commands);

		nvrhi::DeviceHandle m_Device;
		GraphicsDeviceLimits m_Limits;
		bool m_Supported = false;
		ProbeGIResourceFactory m_DefaultFactory;
		ProbeGIResourceFactory& m_Factory;
		ProbeTraceScene m_Scene;
		GPU::ProbeGIConstants m_Parameters;
		glm::uvec3 m_Counts{0};
		glm::vec3 m_Origin{0.0f};
		float m_Spacing = 0.0f;
		bool m_HasHistory = false;
		Ref<const ProbeTraceSceneIdentity> m_SceneIdentity;
		bool m_GPUSceneDirty = true;
		bool m_ReportedUpdateFailure = false;
		uint32_t m_ReportedUploadFailures = 0;
		uint32_t m_Current = 0;
		uint32_t m_Cursor = 0;
		uint32_t m_Frame = 0;
		nvrhi::TextureHandle m_Irradiance[2], m_Distance[2];
		nvrhi::BufferHandle m_Constants, m_Nodes, m_Triangles, m_Materials, m_Texels, m_Rays;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::BindingLayoutHandle m_ComputeLayout, m_ForwardLayout;
		nvrhi::BindingSetHandle m_ForwardBindings[2], m_ComputeBindings[2];
		nvrhi::ComputePipelineHandle m_TracePipeline, m_FilterPipeline;
	};

}
