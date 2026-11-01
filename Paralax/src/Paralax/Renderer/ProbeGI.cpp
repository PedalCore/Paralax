#include "pxpch.h"
#include "Paralax/Renderer/ProbeGI.h"

#include "Paralax/Renderer/ShaderLibrary.h"

#include <algorithm>
#include <cmath>

namespace Paralax {

	ProbeGI::ProbeGI(nvrhi::IDevice* device, const GraphicsDeviceLimits& limits, ProbeGIResourceFactory* factory)
		: m_Device(device)
		, m_Limits(limits)
		, m_Supported(limits.MaxStorageBuffersPerStage >= 6 && limits.MaxStorageBuffersPerSet >= 6)
		, m_Factory(factory ? *factory : m_DefaultFactory)
	{
		PX_CORE_VERIFY(device, "ProbeGI requires a device");
	}

	bool ProbeGI::Initialize(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(commands, "ProbeGI initialization requires an open command list");
		m_Constants = m_Factory.CreateBuffer(m_Device, nvrhi::BufferDesc().setByteSize(sizeof(GPU::ProbeGIConstants)).setIsConstantBuffer(true).setIsVolatile(true).setMaxVersions(16).setDebugName("ProbeGIConstants"));
		m_Sampler = m_Device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		if (!m_Constants || !m_Sampler)
			return false;
		if (m_Supported)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
							   nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
							   nvrhi::BindingLayoutItem::Texture_UAV(1), nvrhi::BindingLayoutItem::Texture_UAV(2)};
			for (uint32_t slot = 0; slot < 5; ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
			for (uint32_t slot = 5; slot < 8; ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			m_ComputeLayout = m_Device->createBindingLayout(layout);
			auto trace = ShaderLibrary::CreateShader(m_Device, "ProbeGI", "TraceProbes", nvrhi::ShaderType::Compute);
			auto filter = ShaderLibrary::CreateShader(m_Device, "ProbeGI", "FilterProbes", nvrhi::ShaderType::Compute);
			if (!m_ComputeLayout || !trace || !filter)
				return false;
			m_TracePipeline = m_Device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(trace).addBindingLayout(m_ComputeLayout));
			m_FilterPipeline = m_Device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(filter).addBindingLayout(m_ComputeLayout));
			if (!m_TracePipeline || !m_FilterPipeline)
				return false;
		}
		nvrhi::BindingLayoutDesc forwardLayout;
		forwardLayout.visibility = nvrhi::ShaderType::Pixel;
		forwardLayout.registerSpace = 2;
		forwardLayout.registerSpaceIsDescriptorSet = true;
		forwardLayout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0),
								  nvrhi::BindingLayoutItem::Texture_SRV(1)};
		m_ForwardLayout = m_Device->createBindingLayout(forwardLayout);
		if (!m_ForwardLayout || !CreateVolumes(1, commands))
			return false;
		Disable(commands);
		if (!m_Supported)
		{
			PX_CORE_WARN("Probe GI is unavailable: the adapter needs six storage-buffer descriptors per compute stage and set");
		}
		return true;
	}

	bool ProbeGI::CreateVolumes(uint32_t count, nvrhi::ICommandList* commands)
	{
		if (count > m_Limits.MaxTextureArrayLayers)
			return false;
		const bool recreate = !m_Irradiance[0] || m_Irradiance[0]->getDesc().arraySize != count;
		// Initial clears do not retain textures through binding sets. Complete those submissions before replacing
		// a volume; geometry/history resets at the same dimensions reuse storage and require no idle wait.
		if (recreate && m_Irradiance[0])
			m_Device->waitForIdle();
		nvrhi::TextureDesc desc;
		desc.width = 8;
		desc.height = 8;
		desc.arraySize = count;
		desc.dimension = nvrhi::TextureDimension::Texture2DArray;
		desc.format = nvrhi::Format::RGBA32_FLOAT;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle irradiance[2] = {m_Irradiance[0], m_Irradiance[1]};
		nvrhi::TextureHandle distance[2] = {m_Distance[0], m_Distance[1]};
		for (uint32_t i = 0; i < 2; ++i)
		{
			desc.debugName = "ProbeIrradiance";
			if (recreate)
				irradiance[i] = m_Factory.CreateTexture(m_Device, desc);
			desc.debugName = "ProbeDistanceMoments";
			if (recreate)
				distance[i] = m_Factory.CreateTexture(m_Device, desc);
			if (!irradiance[i] || !distance[i])
				return false;
		}
		nvrhi::BindingSetDesc set;
		set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants), nvrhi::BindingSetItem::Texture_SRV(0, irradiance[0]),
						nvrhi::BindingSetItem::Texture_SRV(1, distance[0])};
		auto bindings = m_Factory.CreateBindingSet(m_Device, set, m_ForwardLayout);
		if (!bindings)
			return false;
		for (uint32_t i = 0; i < 2; ++i)
		{
			m_Irradiance[i] = irradiance[i];
			m_Distance[i] = distance[i];
			commands->clearTextureFloat(m_Irradiance[i], nvrhi::AllSubresources, nvrhi::Color(0.0f));
			commands->clearTextureFloat(m_Distance[i], nvrhi::AllSubresources, nvrhi::Color(0.0f));
		}
		m_Current = 0;
		m_ForwardBindings = bindings;
		return true;
	}

	bool ProbeGI::UploadScene(nvrhi::ICommandList* commands)
	{
		auto fits = [&](const auto& data) {
			using Element = typename std::remove_cvref_t<decltype(data)>::value_type;
			return std::max(data.size(), size_t(1)) * sizeof(Element) <= m_Limits.MaxStorageBufferBytes;
		};
		// Hardware limits cannot recover on a later frame. Reject before allocating any replacement resources.
		if (!fits(m_Scene.GetNodes()) || !fits(m_Scene.GetTriangles()) || !fits(m_Scene.GetMaterials()) || !fits(m_Scene.GetTexels()))
			return false;
		auto allocate = [&](const auto& data, const char* name) {
			using Element = typename std::remove_cvref_t<decltype(data)>::value_type;
			const size_t bytes = std::max(data.size(), size_t(1)) * sizeof(Element);
			return m_Factory.CreateBuffer(m_Device, nvrhi::BufferDesc().setByteSize(bytes).setStructStride(sizeof(Element)).setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true).setDebugName(name));
		};
		auto nodes = allocate(m_Scene.GetNodes(), "ProbeTraceNodes");
		auto triangles = allocate(m_Scene.GetTriangles(), "ProbeTraceTriangles");
		auto materials = allocate(m_Scene.GetMaterials(), "ProbeTraceMaterials");
		auto texels = allocate(m_Scene.GetTexels(), "ProbeTraceTexels");
		if (!nodes || !triangles || !materials || !texels)
			return false;
		// Do not record any transfer or replace the live snapshot until every allocation succeeded.
		auto upload = [&](const auto& data, nvrhi::IBuffer* buffer) {
			using Element = typename std::remove_cvref_t<decltype(data)>::value_type;
			if (!data.empty())
				commands->writeBuffer(buffer, data.data(), data.size() * sizeof(Element));
			else
			{
				const Element empty{};
				commands->writeBuffer(buffer, &empty, sizeof(Element));
			}
		};
		upload(m_Scene.GetNodes(), nodes);
		upload(m_Scene.GetTriangles(), triangles);
		upload(m_Scene.GetMaterials(), materials);
		upload(m_Scene.GetTexels(), texels);
		m_Nodes = nodes;
		m_Triangles = triangles;
		m_Materials = materials;
		m_Texels = texels;
		return true;
	}

	bool ProbeGI::BindForward()
	{
		if (!m_ForwardLayout)
			return false;
		nvrhi::BindingSetDesc set;
		set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants), nvrhi::BindingSetItem::Texture_SRV(0, m_Irradiance[m_Current]),
						nvrhi::BindingSetItem::Texture_SRV(1, m_Distance[m_Current])};
		auto bindings = m_Factory.CreateBindingSet(m_Device, set, m_ForwardLayout);
		if (!bindings)
			return false;
		m_ForwardBindings = bindings;
		return true;
	}

	void ProbeGI::Disable(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(m_Constants && commands, "ProbeGI::Disable requires initialization and an open command list");
		m_Parameters.Origin.w = 0.0f;
		m_HasHistory = false;
		commands->writeBuffer(m_Constants, &m_Parameters, sizeof(m_Parameters));
	}

	bool ProbeGI::Update(Scene& scene, AssetManager& assets, const ProbeGISpecification& spec, nvrhi::ICommandList* commands,
						 nvrhi::IBuffer* viewConstants, nvrhi::IBuffer* lights, nvrhi::ITexture* environment, uint32_t lightCount)
	{
		PX_CORE_VERIFY(m_ForwardBindings && (!m_Supported || (m_TracePipeline && m_FilterPipeline)) && commands && viewConstants && lights && environment,
					   "ProbeGI::Update requires successful initialization and valid recording resources");
		if (!m_Supported || lightCount > GPU::MaxLights)
			return false;
		if (!std::isfinite(spec.Spacing) || spec.Spacing < 0.1f || spec.Spacing > 100.0f ||
			!std::isfinite(spec.Origin.x) || !std::isfinite(spec.Origin.y) || !std::isfinite(spec.Origin.z) ||
			glm::any(glm::greaterThan(glm::abs(spec.Origin), glm::vec3(ProbeTraceScene::MaxCoordinate))) ||
			glm::any(glm::lessThan(spec.Counts, glm::uvec3(2))) || glm::any(glm::greaterThan(spec.Counts, glm::uvec3(12))) ||
			spec.RaysPerProbe < 32 || spec.RaysPerProbe > 256 || spec.ProbesPerFrame == 0 || spec.ProbesPerFrame > 1728 ||
			!std::isfinite(spec.Hysteresis) || spec.Hysteresis < 0.0f || spec.Hysteresis > 0.99f ||
			!std::isfinite(spec.NormalBias) || spec.NormalBias < 0.0f || spec.NormalBias > spec.Spacing * 0.5f ||
			!std::isfinite(spec.ViewBias) || spec.ViewBias < 0.0f || spec.ViewBias > spec.Spacing * 0.5f)
			return false;
		const uint32_t count = spec.Counts.x * spec.Counts.y * spec.Counts.z;
		if (count > m_Limits.MaxTextureArrayLayers || uint64_t(count) * spec.RaysPerProbe * sizeof(glm::vec4) > m_Limits.MaxStorageBufferBytes)
			return false;
		bool changed = false;
		if (!m_Scene.Update(scene, assets, changed))
			return false;
		m_GPUSceneDirty |= changed;
		if (m_GPUSceneDirty)
		{
			if (!UploadScene(commands))
				return false;
			m_GPUSceneDirty = false;
		}
		const bool reset = changed || !m_HasHistory || spec.Counts != m_Counts || spec.Origin != m_Origin || spec.Spacing != m_Spacing;
		if (reset)
		{
			if (!CreateVolumes(count, commands))
				return false;
			m_Cursor = 0;
			m_Frame = 0;
			m_Counts = spec.Counts;
			m_Origin = spec.Origin;
			m_Spacing = spec.Spacing;
		}
		const uint32_t updates = reset ? count : std::min(spec.ProbesPerFrame, count);
		const uint64_t rayBytes = uint64_t(updates) * spec.RaysPerProbe * sizeof(glm::vec4);
		if (!m_Rays || m_Rays->getDesc().byteSize < rayBytes)
		{
			m_Rays = m_Factory.CreateBuffer(m_Device, nvrhi::BufferDesc().setByteSize(rayBytes).setStructStride(sizeof(glm::vec4)).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true).setDebugName("ProbeRays"));
			if (!m_Rays)
				return false;
		}
		m_Parameters.Origin = glm::vec4(spec.Origin, 1.0f);
		m_Parameters.Spacing = glm::vec4(glm::vec3(spec.Spacing), 100000.0f);
		m_Parameters.Grid = glm::uvec4(spec.Counts, count);
		m_Parameters.Update = {m_Cursor, updates, spec.RaysPerProbe, m_Frame};
		m_Parameters.Scene = {static_cast<uint32_t>(m_Scene.GetNodes().size()), lightCount, 0, 0};
		m_Parameters.Tuning = {spec.NormalBias, spec.ViewBias, reset ? 0.0f : spec.Hysteresis, spec.Spacing * 3.0f};
		commands->writeBuffer(m_Constants, &m_Parameters, sizeof(m_Parameters));
		const uint32_t next = 1 - m_Current;
		// Filtering copies unchanged probes. Separate read/write volumes prevent bounce-order dependence.
		nvrhi::BindingSetDesc set;
		set.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants), nvrhi::BindingSetItem::ConstantBuffer(1, viewConstants),
			nvrhi::BindingSetItem::Sampler(0, m_Sampler), nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_Nodes),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_Triangles), nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_Materials),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_Texels), nvrhi::BindingSetItem::StructuredBuffer_SRV(4, lights),
			nvrhi::BindingSetItem::Texture_SRV(5, environment), nvrhi::BindingSetItem::Texture_SRV(6, m_Irradiance[m_Current]),
			nvrhi::BindingSetItem::Texture_SRV(7, m_Distance[m_Current]), nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_Rays),
			nvrhi::BindingSetItem::Texture_UAV(1, m_Irradiance[next]), nvrhi::BindingSetItem::Texture_UAV(2, m_Distance[next])};
		auto bindings = m_Factory.CreateBindingSet(m_Device, set, m_ComputeLayout);
		if (!bindings)
			return false;
		nvrhi::ComputeState state;
		state.pipeline = m_TracePipeline;
		state.bindings = {bindings};
		commands->setComputeState(state);
		commands->dispatch((spec.RaysPerProbe + 63) / 64, updates, 1);
		state.pipeline = m_FilterPipeline;
		commands->setComputeState(state);
		commands->dispatch(count, 1, 1);
		m_Current = next;
		m_Cursor = (m_Cursor + updates) % count;
		++m_Frame;
		m_HasHistory = true;
		return BindForward();
	}

}
