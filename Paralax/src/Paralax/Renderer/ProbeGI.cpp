#include "pxpch.h"
#include "Paralax/Renderer/ProbeGI.h"

#include "Paralax/Renderer/ShaderLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
		// Pinned NVRHI Vulkan clearTexture does not retain referencedResources (unlike a bound descriptor set).
		// Retire initial, unbound clears before releasing their textures. Same-size resets reuse storage.
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
		nvrhi::BindingSetHandle bindings[2] = {m_ForwardBindings[0], m_ForwardBindings[1]};
		for (uint32_t i = 0; i < 2; ++i)
		{
			desc.width = desc.height = 8;
			desc.debugName = "ProbeIrradiance";
			if (recreate)
				irradiance[i] = m_Factory.CreateTexture(m_Device, desc);
			desc.width = desc.height = GPU::ProbeDistanceTileSize;
			desc.debugName = "ProbeDistanceMoments";
			if (recreate)
				distance[i] = m_Factory.CreateTexture(m_Device, desc);
			if (!irradiance[i] || !distance[i])
				return false;
		}
		if (recreate)
			for (uint32_t i = 0; i < 2; ++i)
			{
				nvrhi::BindingSetDesc set;
				set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants), nvrhi::BindingSetItem::Texture_SRV(0, irradiance[i]),
								nvrhi::BindingSetItem::Texture_SRV(1, distance[i])};
				bindings[i] = m_Factory.CreateBindingSet(m_Device, set, m_ForwardLayout);
				if (!bindings[i])
					return false;
			}
		for (uint32_t i = 0; i < 2; ++i)
		{
			m_Irradiance[i] = irradiance[i];
			m_Distance[i] = distance[i];
			m_ForwardBindings[i] = bindings[i];
			commands->clearTextureFloat(m_Irradiance[i], nvrhi::AllSubresources, nvrhi::Color(0.0f));
			commands->clearTextureFloat(m_Distance[i], nvrhi::AllSubresources, nvrhi::Color(0.0f));
		}
		m_Current = 0;
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

	void ProbeGI::Disable(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(m_Constants && commands, "ProbeGI::Disable requires initialization and an open command list");
		m_Parameters.Origin.w = 0.0f;
		m_HasHistory = false;
		commands->writeBuffer(m_Constants, &m_Parameters, sizeof(m_Parameters));
	}

	std::optional<GPU::ProbeInspection> ProbeGI::Inspect(glm::vec3 position, glm::vec3 normal, glm::vec3 view, float viewDistance)
	{
		auto finite = [](glm::vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
		if (!m_HasHistory || !std::isfinite(viewDistance) || viewDistance < 0.0f || viewDistance > 1000000.0f || !finite(position) || !finite(normal) || !finite(view) ||
			glm::any(glm::greaterThan(glm::abs(position), glm::vec3(ProbeTraceScene::MaxCoordinate))) ||
			!std::isfinite(glm::length(normal)) || !std::isfinite(glm::length(view)) || glm::length(normal) < 1e-6f || glm::length(view) < 1e-6f)
			return std::nullopt;
		const glm::vec3 gridPosition = (position - glm::vec3(m_Parameters.Origin)) / glm::vec3(m_Parameters.Spacing);
		if (glm::any(glm::lessThan(gridPosition, glm::vec3(0.0f))) || glm::any(glm::greaterThan(gridPosition, glm::vec3(m_Parameters.Grid) - 1.0f)))
			return std::nullopt;
		struct InspectionConstants
		{
			GPU::ProbeGIConstants Probe;
			glm::vec4 Position, Normal, View;
		};
		static_assert(sizeof(InspectionConstants) == 160);
		const InspectionConstants parameters{m_Parameters, glm::vec4(position, 0.0f), glm::vec4(glm::normalize(normal), 0.0f), glm::vec4(glm::normalize(view), viewDistance)};
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::ConstantBuffer(2),
							   nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0), nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
							   nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2), nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
							   nvrhi::BindingLayoutItem::Texture_SRV(6), nvrhi::BindingLayoutItem::Texture_SRV(7), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3)};
		auto layout = m_Device->createBindingLayout(layoutDesc);
		auto shader = ShaderLibrary::CreateShader(m_Device, "ProbeGI", "InspectProbeGI", nvrhi::ShaderType::Compute);
		if (!layout || !shader)
			return std::nullopt;
		auto pipeline = m_Device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
		auto constants = m_Factory.CreateBuffer(m_Device, nvrhi::BufferDesc().setByteSize(sizeof(parameters)).setIsConstantBuffer(true).setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
		auto result = m_Factory.CreateBuffer(m_Device, nvrhi::BufferDesc().setByteSize(sizeof(GPU::ProbeInspection)).setStructStride(sizeof(GPU::ProbeInspection)).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true));
		auto staging = m_Factory.CreateBuffer(m_Device, nvrhi::BufferDesc().setByteSize(sizeof(GPU::ProbeInspection)).setCpuAccess(nvrhi::CpuAccessMode::Read).setInitialState(nvrhi::ResourceStates::CopyDest).setKeepInitialState(true));
		auto commands = m_Device->createCommandList();
		if (!pipeline || !constants || !result || !staging || !commands)
			return std::nullopt;
		nvrhi::BindingSetDesc bindingsDesc;
		bindingsDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constants), nvrhi::BindingSetItem::ConstantBuffer(2, constants),
								 nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_Nodes), nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_Triangles),
								 nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_Materials), nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_Texels),
								 nvrhi::BindingSetItem::Texture_SRV(6, GetIrradiance()), nvrhi::BindingSetItem::Texture_SRV(7, GetDistance()),
								 nvrhi::BindingSetItem::StructuredBuffer_UAV(3, result)};
		auto bindings = m_Factory.CreateBindingSet(m_Device, bindingsDesc, layout);
		if (!bindings)
			return std::nullopt;
		commands->open();
		commands->writeBuffer(constants, &parameters, sizeof(parameters));
		nvrhi::ComputeState state;
		state.pipeline = pipeline;
		state.bindings = {bindings};
		commands->setComputeState(state);
		commands->dispatch(1, 1, 1);
		commands->copyBuffer(staging, 0, result, 0, sizeof(GPU::ProbeInspection));
		commands->close();
		m_Device->executeCommandList(commands);
		m_Device->waitForIdle();
		const void* mapped = m_Device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
		if (!mapped)
			return std::nullopt;
		GPU::ProbeInspection inspection;
		std::memcpy(&inspection, mapped, sizeof(inspection));
		m_Device->unmapBuffer(staging);
		return inspection;
	}

	bool ProbeGI::Update(Scene& scene, AssetManager& assets, const ProbeGISpecification& spec, nvrhi::ICommandList* commands,
						 nvrhi::IBuffer* viewConstants, nvrhi::IBuffer* lights, nvrhi::ITexture* environment, uint32_t lightCount)
	{
		PX_CORE_VERIFY(GetBindings() && (!m_Supported || (m_TracePipeline && m_FilterPipeline)) && commands && viewConstants && lights,
					   "ProbeGI::Update requires successful initialization and valid recording resources");
		if (!m_Supported || !environment || lightCount > GPU::MaxLights)
			return false;
		if (!std::isfinite(spec.Spacing) || spec.Spacing < 0.1f || spec.Spacing > 100.0f ||
			!std::isfinite(spec.Origin.x) || !std::isfinite(spec.Origin.y) || !std::isfinite(spec.Origin.z) ||
			glm::any(glm::greaterThan(glm::abs(spec.Origin), glm::vec3(ProbeTraceScene::MaxCoordinate))) ||
			glm::any(glm::lessThan(spec.Counts, glm::uvec3(2))) || glm::any(glm::greaterThan(spec.Counts, glm::uvec3(12))) ||
			spec.RaysPerProbe < 32 || spec.RaysPerProbe > 256 || spec.ProbesPerFrame == 0 || spec.ProbesPerFrame > 1728 ||
			!std::isfinite(spec.Hysteresis) || spec.Hysteresis < 0.0f || spec.Hysteresis > 0.99f ||
			!std::isfinite(spec.NormalBias) || spec.NormalBias < 0.0f || spec.NormalBias > spec.Spacing * 0.5f ||
			!std::isfinite(spec.ViewBias) || spec.ViewBias < 0.0f || spec.ViewBias > 0.5f)
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
		const auto& sceneIdentity = m_Scene.GetSceneIdentity();
		const bool reset = m_SceneIdentity != sceneIdentity || !m_HasHistory || spec.Counts != m_Counts || spec.Origin != m_Origin || spec.Spacing != m_Spacing;
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
		const uint32_t updates = reset ? count : spec.FreezeUpdates ? 0
																	: std::min(spec.ProbesPerFrame, count);
		const uint64_t rayBytes = uint64_t(updates) * spec.RaysPerProbe * sizeof(glm::vec4);
		if (!m_Rays || m_Rays->getDesc().byteSize < rayBytes)
		{
			auto rays = m_Factory.CreateBuffer(m_Device, nvrhi::BufferDesc().setByteSize(rayBytes).setStructStride(sizeof(glm::vec4)).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true).setDebugName("ProbeRays"));
			if (!rays)
				return false;
			m_Rays = rays;
		}
		m_Parameters.Origin = glm::vec4(spec.Origin, 1.0f);
		m_Parameters.Spacing = glm::vec4(glm::vec3(spec.Spacing), 100000.0f);
		m_Parameters.Grid = glm::uvec4(spec.Counts, count);
		m_Parameters.Update = {m_Cursor, updates, spec.RaysPerProbe, m_Frame};
		m_Parameters.Scene = {static_cast<uint32_t>(m_Scene.GetNodes().size()), lightCount, GPU::ProbeDistanceTileSize, 0};
		m_Parameters.Tuning = {spec.NormalBias, spec.ViewBias * spec.Spacing, reset ? 0.0f : spec.Hysteresis, spec.Spacing * 3.0f};
		m_Parameters.Diagnostics = {spec.DebugProbe >= 0 && static_cast<uint32_t>(spec.DebugProbe) < count ? static_cast<float>(spec.DebugProbe) : -1.0f,
									spec.VisibilityWeighting ? 1.0f : 0.0f, spec.BounceFeedback ? 1.0f : 0.0f, 0.0f};
		commands->writeBuffer(m_Constants, &m_Parameters, sizeof(m_Parameters));
		if (updates == 0)
			return true;
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
		// Slot, type and subresource ranges are fixed above. Rebuild only when a resource changes; each
		// ping-pong direction owns a set so stable frames do not allocate Vulkan descriptor pools.
		auto& bindings = m_ComputeBindings[m_Current];
		bool matches = bindings && bindings->getDesc()->bindings.size() == set.bindings.size();
		for (size_t i = 0; matches && i < set.bindings.size(); ++i)
			matches = bindings->getDesc()->bindings[i].resourceHandle == set.bindings[i].resourceHandle;
		if (!matches)
		{
			auto replacement = m_Factory.CreateBindingSet(m_Device, set, m_ComputeLayout);
			if (!replacement)
				return false;
			bindings = replacement;
		}
		nvrhi::ComputeState state;
		state.pipeline = m_TracePipeline;
		state.bindings = {bindings};
		commands->setComputeState(state);
		commands->dispatch((spec.RaysPerProbe + 63) / 64, updates, 1);
		// Both dispatches reuse the same bindings, so NVRHI does not infer the ray-buffer UAV
		// dependency. The next setComputeState commits this barrier before filtering reads the rays.
		commands->setBufferState(m_Rays, nvrhi::ResourceStates::UnorderedAccess);
		state.pipeline = m_FilterPipeline;
		commands->setComputeState(state);
		commands->dispatch(GPU::ProbeDistanceTileSize / 8, GPU::ProbeDistanceTileSize / 8, count);
		m_Current = next;
		m_Cursor = (m_Cursor + updates) % count;
		++m_Frame;
		m_HasHistory = true;
		m_SceneIdentity = sceneIdentity;
		return true;
	}

}
