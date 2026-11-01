#pragma once

#include "Paralax/Renderer/GraphicsDevice.h"
#include "Paralax/Renderer/ProbeTraceScene.h"
#include "Paralax/Renderer/SceneRendererTypes.h"

#include <nvrhi/nvrhi.h>

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

	struct ProbeGISpecification
	{
		glm::vec3 Origin{0.0f};
		float Spacing = 1.0f;
		glm::uvec3 Counts{4};
		uint32_t RaysPerProbe = 64;
		uint32_t ProbesPerFrame = 32;
		float Hysteresis = 0.85f;
		float NormalBias = 0.05f;
		float ViewBias = 0.02f;
	};

	namespace GPU {

		struct ProbeGIConstants
		{
			glm::vec4 Origin{0.0f};
			glm::vec4 Spacing{1.0f};
			glm::uvec4 Grid{0};
			glm::uvec4 Update{0};
			glm::uvec4 Scene{0};
			glm::vec4 Tuning{0.0f};
		};
		static_assert(sizeof(ProbeGIConstants) == 96);

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
		bool Update(Scene& scene, AssetManager& assets, const ProbeGISpecification& specification, nvrhi::ICommandList* commands,
					nvrhi::IBuffer* viewConstants, nvrhi::IBuffer* lights, nvrhi::ITexture* environment, uint32_t lightCount);
		// Disables sampling and discards history. Bindings remain valid for the forward pipeline.
		void Disable(nvrhi::ICommandList* commands);
		nvrhi::IBindingLayout* GetLayout() const { return m_ForwardLayout; }
		nvrhi::IBindingSet* GetBindings() const { return m_ForwardBindings; }
		nvrhi::ITexture* GetIrradiance() const { return m_Irradiance[m_Current]; }
		nvrhi::ITexture* GetDistance() const { return m_Distance[m_Current]; }
		const GPU::ProbeGIConstants& GetConstants() const { return m_Parameters; }

	private:
		bool CreateVolumes(uint32_t count, nvrhi::ICommandList* commands);
		bool UploadScene(nvrhi::ICommandList* commands);
		bool BindForward();

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
		bool m_GPUSceneDirty = true;
		uint32_t m_Current = 0;
		uint32_t m_Cursor = 0;
		uint32_t m_Frame = 0;
		nvrhi::TextureHandle m_Irradiance[2], m_Distance[2];
		nvrhi::BufferHandle m_Constants, m_Nodes, m_Triangles, m_Materials, m_Texels, m_Rays;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::BindingLayoutHandle m_ComputeLayout, m_ForwardLayout;
		nvrhi::BindingSetHandle m_ForwardBindings;
		nvrhi::ComputePipelineHandle m_TracePipeline, m_FilterPipeline;
	};

}
