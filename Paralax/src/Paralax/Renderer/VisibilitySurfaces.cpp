#include "pxpch.h"
#include "Paralax/Renderer/VisibilitySurfaces.h"

#include "Paralax/Renderer/ShaderLibrary.h"

#include <cstring>

namespace Paralax {

	VisibilitySurfaces::VisibilitySurfaces(GraphicsDevice& graphics, DeferredResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Device(graphics.GetDevice())
		, m_Factory(factory ? *factory : m_DefaultFactory)
	{
	}

	void VisibilitySurfaces::ReleaseTargets()
	{
		m_MeshBindings.clear();
		m_Framebuffer = nullptr;
		m_Surfaces = nullptr;
		m_Depth = nullptr;
		m_IDs = nullptr;
		m_OwnedBytes = 0;
	}

	bool VisibilitySurfaces::Fail(const char* reason)
	{
		if (std::strcmp(m_FallbackReason, reason) != 0)
			PX_CORE_ERROR("Visibility rendering unavailable ({}); using Forward", reason);
		m_FallbackReason = reason;
		return false;
	}

	bool VisibilitySurfaces::Prepare(bool enabled, nvrhi::ITexture* depth, nvrhi::IFramebuffer* surfaces, nvrhi::IBindingLayout* view,
									 nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe)
	{
		++m_Frame;
		if (!enabled)
		{
			if (m_IDs)
			{
				m_Graphics.WaitForIdle();
				ReleaseTargets();
				++m_Generation;
			}
			m_FallbackReason = "";
			return false;
		}
		if (m_Framebuffer && m_Depth == depth && m_Surfaces == surfaces && m_ResolvePipeline && m_Pipelines[0] && m_Pipelines[1] && m_Pipelines[2])
			return true;
		m_Graphics.WaitForIdle();
		ReleaseTargets();
		if (!depth || !surfaces)
			return Fail("missing surface targets");
		const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderLoad;
		if ((m_Factory.QueryFormatSupport(m_Device, nvrhi::Format::R32_UINT) & required) != required)
			return Fail("ID format unsupported");
		if (m_Graphics.GetInfo().Limits.MaxStorageBuffersPerStage < 4 || m_Graphics.GetInfo().Limits.MaxStorageBuffersPerSet < 4)
			return Fail("raw geometry descriptors unsupported");
		++m_Generation;
		if (!m_VisibilityLayout || !m_ResolveLayout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 3;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {nvrhi::BindingLayoutItem::RawBuffer_SRV(0), nvrhi::BindingLayoutItem::RawBuffer_SRV(1)};
			m_VisibilityLayout = m_Device->createBindingLayout(layout);
			layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(2));
			m_ResolveLayout = m_Device->createBindingLayout(layout);
		}
		if (!m_VS)
			m_VS = ShaderLibrary::CreateShader(m_Device, "Visibility", "VisibilityVS", nvrhi::ShaderType::Vertex);
		if (!m_PS)
			m_PS = ShaderLibrary::CreateShader(m_Device, "Visibility", "VisibilityPS", nvrhi::ShaderType::Pixel);
		if (!m_ResolveVS)
			m_ResolveVS = ShaderLibrary::CreateShader(m_Device, "Visibility", "MaterialResolveVS", nvrhi::ShaderType::Vertex);
		if (!m_ResolvePS)
			m_ResolvePS = ShaderLibrary::CreateShader(m_Device, "Visibility", "MaterialResolvePS", nvrhi::ShaderType::Pixel);
		if (!m_VisibilityLayout || !m_ResolveLayout || !m_VS || !m_PS || !m_ResolveVS || !m_ResolvePS)
			return Fail("shader or layout creation failed");
		nvrhi::TextureDesc desc;
		desc.width = depth->getDesc().width;
		desc.height = depth->getDesc().height;
		desc.format = nvrhi::Format::R32_UINT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.debugName = "VisibilityTriangleIDs";
		++m_AllocationAttempts;
		m_IDs = m_Factory.CreateTexture(m_Device, desc);
		if (!m_IDs)
			return Fail("ID allocation failed");
		++m_AllocationAttempts;
		m_Framebuffer = m_Factory.CreateFramebuffer(m_Device, nvrhi::FramebufferDesc().addColorAttachment(m_IDs).setDepthAttachment(depth));
		if (!m_Framebuffer)
		{
			ReleaseTargets();
			return Fail("ID framebuffer creation failed");
		}
		m_OwnedBytes = m_Device->getTextureMemoryRequirements(m_IDs).size;
		for (size_t i = 0; i < m_Pipelines.size(); ++i)
		{
			if (m_Pipelines[i])
				continue;
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = m_VS;
			pipeline.PS = m_PS;
			pipeline.bindingLayouts = {view, material, probe, m_VisibilityLayout};
			pipeline.renderState.rasterState.setFrontCounterClockwise(i != 1).setCullMode(i == 2 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back);
			pipeline.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
			++m_AllocationAttempts;
			m_Pipelines[i] = m_Factory.CreatePipeline(m_Device, pipeline, m_Framebuffer->getFramebufferInfo());
			if (!m_Pipelines[i])
			{
				ReleaseTargets();
				return Fail("ID pipeline creation failed");
			}
		}
		if (!m_ResolvePipeline)
		{
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = m_ResolveVS;
			pipeline.PS = m_ResolvePS;
			pipeline.bindingLayouts = {view, material, probe, m_ResolveLayout};
			pipeline.renderState.rasterState.setCullNone();
			pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			++m_AllocationAttempts;
			m_ResolvePipeline = m_Factory.CreatePipeline(m_Device, pipeline, surfaces->getFramebufferInfo());
			if (!m_ResolvePipeline)
			{
				ReleaseTargets();
				return Fail("material resolve pipeline creation failed");
			}
		}
		m_Depth = depth;
		m_Surfaces = surfaces;
		return true;
	}

	bool VisibilitySurfaces::PrepareMesh(const GPUMesh& mesh)
	{
		PX_CORE_VERIFY(m_Framebuffer, "Visibility mesh preparation requires prepared targets");
		const uint64_t limit = m_Graphics.GetInfo().Limits.MaxStorageBufferBytes;
		if (!mesh.VertexBuffer || !mesh.IndexBuffer || !FitsGeometry(mesh.VertexBuffer->getDesc().byteSize, mesh.IndexBuffer->getDesc().byteSize, limit))
			return Fail("geometry buffer capacity exceeded");
		auto& entry = m_MeshBindings[mesh.VertexBuffer];
		entry.LastFrame = m_Frame;
		if (entry.Visibility && entry.Resolve && entry.Indices == mesh.IndexBuffer)
			return true;
		entry.Vertices = mesh.VertexBuffer;
		entry.Indices = mesh.IndexBuffer;
		nvrhi::BindingSetDesc bindings;
		bindings.bindings = {nvrhi::BindingSetItem::RawBuffer_SRV(0, mesh.VertexBuffer), nvrhi::BindingSetItem::RawBuffer_SRV(1, mesh.IndexBuffer)};
		++m_AllocationAttempts;
		++m_Generation;
		entry.Visibility = m_Factory.CreateBindingSet(m_Device, bindings, m_VisibilityLayout);
		bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(2, m_IDs));
		++m_AllocationAttempts;
		entry.Resolve = m_Factory.CreateBindingSet(m_Device, bindings, m_ResolveLayout);
		if (!entry.Visibility || !entry.Resolve)
			return Fail("geometry binding creation failed");
		return true;
	}

	void VisibilitySurfaces::EndPreparation(bool success)
	{
		std::erase_if(m_MeshBindings, [this](const auto& entry) { return entry.second.LastFrame != m_Frame; });
		if (success)
			m_FallbackReason = "";
	}

	nvrhi::IBindingSet* VisibilitySurfaces::GetBindings(const GPUMesh& mesh, bool resolve) const
	{
		const auto found = m_MeshBindings.find(mesh.VertexBuffer);
		return found == m_MeshBindings.end() ? nullptr : resolve ? found->second.Resolve.Get()
																 : found->second.Visibility.Get();
	}

	uint64_t VisibilitySurfaces::GetGeometryBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& [key, entry] : m_MeshBindings)
		{
			(void)key;
			bytes += m_Device->getBufferMemoryRequirements(entry.Vertices).size + m_Device->getBufferMemoryRequirements(entry.Indices).size;
		}
		return bytes;
	}

	void VisibilitySurfaces::Clear(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(commands && m_Framebuffer, "Visibility clear requires prepared targets and an open command list");
		commands->clearTextureUInt(m_IDs, nvrhi::AllSubresources, 0);
	}

}
