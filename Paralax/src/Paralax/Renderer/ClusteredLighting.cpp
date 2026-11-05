#include "pxpch.h"
#include "ClusteredLighting.h"

#include "Paralax/Renderer/ShaderLibrary.h"

#include <cmath>

namespace Paralax {

	namespace {
		bool Supported(float value)
		{
			return std::isfinite(value) && (value == 0 || std::fpclassify(value) == FP_NORMAL) && std::abs(value) <= 1e10f;
		}
	}

	std::optional<ClusteredLightingGrid> ClusteredLightingGrid::Create(uint32_t width, uint32_t height, uint32_t lightCount,
																	   float nearDepth, float farDepth, const glm::mat4& view, const glm::mat4& projection, uint64_t maxBufferBytes)
	{
		if (!width || !height || width > 8192 || height > 8192 || lightCount > GPU::MaxLights ||
			!Supported(nearDepth) || !Supported(farDepth) || nearDepth >= farDepth)
			return std::nullopt;
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (!Supported(view[column][row]) || !Supported(projection[column][row]))
					return std::nullopt;
		if (view[0][3] != 0 || view[1][3] != 0 || view[2][3] != 0 || view[3][3] != 1 || projection[0][3] != 0 || projection[1][3] != 0)
			return std::nullopt;
		const bool perspective = projection[2][3] == -1 && projection[3][3] == 0;
		const bool orthographic = projection[2][3] == 0 && projection[3][3] == 1;
		if ((!perspective && !orthographic) || (perspective && nearDepth <= 0))
			return std::nullopt;
		const glm::mat4 viewProjection = projection * view;
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (!Supported(viewProjection[column][row]))
					return std::nullopt;
		ClusteredLightingGrid grid;
		grid.m_Constants.Dimensions = {width, height, (width + TileSize - 1) / TileSize, (height + TileSize - 1) / TileSize};
		grid.m_Constants.State = {lightCount, 1, 0, 0};
		float previous = nearDepth;
		for (uint32_t i = 0; i <= DepthSlices; ++i)
		{
			const double t = double(i) / DepthSlices;
			const float boundary = i == 0 ? nearDepth : i == DepthSlices ? farDepth
																		 : static_cast<float>(perspective ? double(nearDepth) * std::pow(double(farDepth) / double(nearDepth), t) : double(nearDepth) + (double(farDepth) - double(nearDepth)) * t);
			if (!Supported(boundary) || (i && boundary <= previous))
				return std::nullopt;
			grid.m_Constants.Depth[i / 4][i % 4] = boundary;
			previous = boundary;
		}
		if (grid.GetBufferBytes() > maxBufferBytes)
			return std::nullopt;
		return grid;
	}
	ClusteredLighting::ClusteredLighting(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Budget(budget)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Buffers(graphics, budget, &m_Factory)
	{
	}

	ClusteredLighting::~ClusteredLighting()
	{
		Release();
	}

	void ClusteredLighting::InvalidateView()
	{
		m_ClusterView = nullptr;
		m_BaseView = nullptr;
		m_ViewLayout = nullptr;
	}

	void ClusteredLighting::ReleaseResources()
	{
		InvalidateView();
		m_Bindings = nullptr;
		m_Pipeline = nullptr;
		m_Layout = nullptr;
		m_ViewBuffer = nullptr;
		m_Data = nullptr;
		m_Buffers.Clear();
	}

	void ClusteredLighting::Release()
	{
		ReleaseResources();
		m_FailedViewBuffer = nullptr;
		m_FailedBytes = 0;
		m_Grid = {};
		m_Reason = "";
	}

	bool ClusteredLighting::Fail(const char* reason)
	{
		m_Reason = reason;
		ReleaseResources();
		m_FailedAvailable = m_Budget.GetCapacity() - m_Budget.GetUsedBytes();
		return false;
	}

	bool ClusteredLighting::Prepare(const ClusteredLightingGrid& grid, nvrhi::IBuffer* viewConstants)
	{
		m_Budget.CollectRetired();
		m_Grid = grid.GetConstants();
		if (!m_Grid.State.x)
		{
			Release();
			return false;
		}
		const uint64_t bytes = grid.GetBufferBytes();
		if (IsReady() && m_Data->getDesc().byteSize == bytes && m_ViewBuffer == viewConstants)
			return true;
		ReleaseResources();
		if (m_FailedBytes == bytes && m_FailedViewBuffer == viewConstants && m_FailedAvailable == m_Budget.GetCapacity() - m_Budget.GetUsedBytes())
			return false;
		m_FailedBytes = bytes;
		m_FailedViewBuffer = viewConstants;
		if (!viewConstants || !viewConstants->getDesc().isConstantBuffer || viewConstants->getDesc().byteSize < sizeof(GPU::ViewConstants) || bytes > m_Graphics.GetInfo().Limits.MaxStorageBufferBytes)
			return Fail("invalid clustered lighting buffer inputs");
		m_Data = m_Buffers.CreateRaw(bytes, "ClusterLightPrefixAndMasks");
		if (!m_Data)
			return Fail(m_Buffers.GetFailureReason());
		auto* device = m_Graphics.GetDevice();
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Compute;
		layout.bindings = {viewConstants->getDesc().isVolatile ? nvrhi::BindingLayoutItem::VolatileConstantBuffer(0) : nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::RawBuffer_UAV(0)};
		++m_ObjectAttempts;
		m_Layout = m_Factory.CreateBindingLayout(device, layout);
		if (!m_Layout)
			return Fail("cluster build layout creation failed");
		auto shader = ShaderLibrary::CreateShader(device, "ClusteredLighting", "BuildMasks", nvrhi::ShaderType::Compute);
		if (!shader)
			return Fail("cluster build shader creation failed");
		++m_ObjectAttempts;
		m_Pipeline = m_Factory.CreateComputePipeline(device, nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(m_Layout));
		if (!m_Pipeline)
			return Fail("cluster build pipeline creation failed");
		nvrhi::BindingSetDesc bindings;
		bindings.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, viewConstants), nvrhi::BindingSetItem::RawBuffer_UAV(0, m_Data)};
		++m_ObjectAttempts;
		m_Bindings = m_Factory.CreateBindingSet(device, bindings, m_Layout);
		if (!m_Bindings)
			return Fail("cluster build binding creation failed");
		m_ViewBuffer = viewConstants;
		m_FailedBytes = 0;
		m_FailedViewBuffer = nullptr;
		m_Reason = "";
		return true;
	}

	nvrhi::IBindingSet* ClusteredLighting::FinalizeView(nvrhi::IBindingSet* baseline, nvrhi::IBindingLayout* layout, GPU::ClusterGridConstants& publishedConstants)
	{
		m_Budget.CollectRetired();
		publishedConstants = m_Grid;
		publishedConstants.State.y = 0;
		if (!IsReady() || !baseline || !layout)
			return nullptr;
		if (m_BaseView == baseline && m_ViewLayout == layout && (m_ClusterView || m_FailedCloneAvailable == m_Budget.GetCapacity() - m_Budget.GetUsedBytes()))
		{
			publishedConstants.State.y = m_ClusterView ? 1u : 0u;
			return m_ClusterView;
		}
		InvalidateView();
		m_BaseView = baseline;
		m_ViewLayout = layout;
		m_FailedCloneAvailable = m_Budget.GetCapacity() - m_Budget.GetUsedBytes();
		auto desc = *baseline->getDesc();
		bool found = false;
		for (auto& binding : desc.bindings)
			if (binding.slot == 0 && binding.type == nvrhi::ResourceType::RawBuffer_SRV)
			{
				binding = nvrhi::BindingSetItem::RawBuffer_SRV(0, m_Data);
				found = true;
			}
		if (!found)
		{
			m_Reason = "cluster view requires a raw light prefix at t0";
			return nullptr;
		}
		++m_ObjectAttempts;
		m_ClusterView = m_Factory.CreateBindingSet(m_Graphics.GetDevice(), desc, layout);
		m_Reason = m_ClusterView ? "" : "cluster view binding creation failed";
		// Keep complete prepared backing on a descriptor failure: retrying it after environment
		// regeneration could invalidate static lighting. This stable base identity caches the failure.
		publishedConstants.State.y = m_ClusterView ? 1u : 0u;
		return m_ClusterView;
	}

	bool ClusteredLighting::Dispatch(nvrhi::ICommandList* commands, std::span<const GPU::Light> lights)
	{
		if (!commands || !IsReady() || lights.size() != m_Grid.State.x || lights.size() > GPU::MaxLights)
			return false;
		commands->writeBuffer(m_Data, lights.data(), lights.size_bytes());
		commands->setBufferState(m_Data, nvrhi::ResourceStates::UnorderedAccess);
		commands->commitBarriers();
		nvrhi::ComputeState state;
		state.pipeline = m_Pipeline;
		state.bindings = {m_Bindings};
		commands->setComputeState(state);
		commands->dispatch(m_Grid.Dimensions.z, m_Grid.Dimensions.w, ClusteredLightingGrid::DepthSlices);
		commands->setBufferState(m_Data, nvrhi::ResourceStates::ShaderResource);
		commands->commitBarriers();
		return true;
	}

}
