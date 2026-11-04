#include "pxpch.h"
#include "Paralax/Renderer/DeferredSurfaces.h"

#include "Paralax/Renderer/ShaderLibrary.h"
#include "Paralax/Renderer/SceneRendererTypes.h"

#include <cstring>

namespace Paralax {

	DeferredSurfaces::DeferredSurfaces(GraphicsDevice& graphics, DeferredResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Device(graphics.GetDevice())
		, m_Factory(factory ? *factory : m_DefaultFactory)
	{
	}

	void DeferredSurfaces::ReleaseTargets()
	{
		m_Ready = false;
		m_SurfaceBindings = nullptr;
		m_Framebuffer = nullptr;
		m_ResolveFramebuffer = nullptr;
		m_Surfaces = {};
		m_HDR = nullptr;
		m_Depth = nullptr;
		m_OwnedBytes = 0;
	}

	bool DeferredSurfaces::Fail(const char* reason)
	{
		ReleaseTargets();
		if (std::strcmp(m_FallbackReason, reason) != 0)
			PX_CORE_ERROR("Deferred rendering unavailable ({}); using Forward", reason);
		m_FallbackReason = reason;
		return false;
	}

	bool DeferredSurfaces::Prepare(bool enabled, nvrhi::ITexture* hdr, nvrhi::ITexture* depth, nvrhi::IInputLayout* input,
								   nvrhi::IBindingLayout* view, nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe)
	{
		if (!enabled)
		{
			if (m_HDR)
			{
				m_Graphics.WaitForIdle();
				ReleaseTargets();
				++m_Generation;
			}
			m_FallbackReason = "";
			return false;
		}
		if (m_Ready && m_HDR == hdr && m_Depth == depth)
			return true;
		m_Graphics.WaitForIdle();
		ReleaseTargets();
		if (!hdr || !depth)
			return Fail("missing HDR or depth target");
		const auto& size = hdr->getDesc();
		// Bound the materialized surface footprint before any allocation; multiplication is widened first.
		if (!FitsCapacity(size.width, size.height))
			return Fail("surface capacity exceeded");
		constexpr std::array formats = {nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT};
		const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderLoad;
		for (auto format : formats)
			if ((m_Factory.QueryFormatSupport(m_Device, format) & required) != required)
				return Fail("surface format unsupported");
		if ((m_Factory.QueryFormatSupport(m_Device, nvrhi::Format::D32) & nvrhi::FormatSupport::ShaderLoad) == nvrhi::FormatSupport::None ||
			(m_Factory.QueryFormatSupport(m_Device, nvrhi::Format::RGBA16_FLOAT) & nvrhi::FormatSupport::Blendable) == nvrhi::FormatSupport::None)
			return Fail("depth sampling or HDR blending unsupported");

		++m_Generation;
		if (!m_SurfaceLayout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.registerSpace = 1;
			layout.registerSpaceIsDescriptorSet = true;
			for (uint32_t slot = 0; slot < 4; ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			m_SurfaceLayout = m_Device->createBindingLayout(layout);
		}
		if (!m_MaterialVS)
			m_MaterialVS = ShaderLibrary::CreateShader(m_Device, "DeferredMaterial", "MaterialVS", nvrhi::ShaderType::Vertex);
		if (!m_MaterialPS)
			m_MaterialPS = ShaderLibrary::CreateShader(m_Device, "DeferredMaterial", "MaterialPS", nvrhi::ShaderType::Pixel);
		if (!m_ResolveVS)
			m_ResolveVS = ShaderLibrary::CreateShader(m_Device, "DeferredLighting", "ResolveVS", nvrhi::ShaderType::Vertex);
		if (!m_ResolvePS)
			m_ResolvePS = ShaderLibrary::CreateShader(m_Device, "DeferredLighting", "ResolvePS", nvrhi::ShaderType::Pixel);
		if (!m_SurfaceLayout || !m_MaterialVS || !m_MaterialPS || !m_ResolveVS || !m_ResolvePS)
			return Fail("shader or layout creation failed");

		constexpr std::array names = {"SurfaceBaseMetal", "SurfaceMappedShadowNormals", "SurfaceReceiverRoughAO"};
		for (size_t i = 0; i < formats.size(); ++i)
		{
			nvrhi::TextureDesc desc;
			desc.width = size.width;
			desc.height = size.height;
			desc.format = formats[i];
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = names[i];
			++m_AllocationAttempts;
			m_Surfaces[i] = m_Factory.CreateTexture(m_Device, desc);
			if (!m_Surfaces[i])
				return Fail("surface allocation failed");
			m_OwnedBytes += m_Device->getTextureMemoryRequirements(m_Surfaces[i]).size;
		}
		nvrhi::FramebufferDesc framebuffer;
		for (auto& surface : m_Surfaces)
			framebuffer.addColorAttachment(surface);
		framebuffer.addColorAttachment(hdr).setDepthAttachment(depth);
		++m_AllocationAttempts;
		m_Framebuffer = m_Factory.CreateFramebuffer(m_Device, framebuffer);
		++m_AllocationAttempts;
		m_ResolveFramebuffer = m_Factory.CreateFramebuffer(m_Device, nvrhi::FramebufferDesc().addColorAttachment(hdr));
		if (!m_Framebuffer || !m_ResolveFramebuffer)
			return Fail("framebuffer allocation failed");
		nvrhi::BindingSetDesc bindings;
		for (uint32_t slot = 0; slot < 3; ++slot)
			bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(slot, m_Surfaces[slot]));
		bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(3, depth));
		++m_AllocationAttempts;
		m_SurfaceBindings = m_Factory.CreateBindingSet(m_Device, bindings, m_SurfaceLayout);
		if (!m_SurfaceBindings)
			return Fail("surface binding creation failed");
		for (size_t i = 0; i < m_MaterialPipelines.size(); ++i)
		{
			if (m_MaterialPipelines[i])
				continue;
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.inputLayout = input;
			pipeline.VS = m_MaterialVS;
			pipeline.PS = m_MaterialPS;
			pipeline.bindingLayouts = {view, material, probe};
			pipeline.renderState.rasterState.setFrontCounterClockwise(i != 1).setCullMode(i == 2 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back);
			pipeline.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
			++m_AllocationAttempts;
			m_MaterialPipelines[i] = m_Factory.CreatePipeline(m_Device, pipeline, m_Framebuffer->getFramebufferInfo());
			if (!m_MaterialPipelines[i])
				return Fail("material pipeline creation failed");
		}
		if (!m_ResolvePipeline)
		{
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = m_ResolveVS;
			pipeline.PS = m_ResolvePS;
			pipeline.bindingLayouts = {view, m_SurfaceLayout, probe};
			pipeline.renderState.rasterState.setCullNone();
			pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			pipeline.renderState.blendState.targets[0].enableBlend().setSrcBlend(nvrhi::BlendFactor::One).setDestBlend(nvrhi::BlendFactor::One);
			++m_AllocationAttempts;
			m_ResolvePipeline = m_Factory.CreatePipeline(m_Device, pipeline, m_ResolveFramebuffer->getFramebufferInfo());
			if (!m_ResolvePipeline)
				return Fail("lighting pipeline creation failed");
		}
		m_HDR = hdr;
		m_Depth = depth;
		m_Ready = true;
		m_FallbackReason = "";
		return true;
	}

	void DeferredSurfaces::ClearCoverage(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(m_Ready && commands, "Deferred coverage clear requires prepared targets and an open command list");
		commands->clearTextureFloat(m_Surfaces[2], nvrhi::AllSubresources, nvrhi::Color(0.0f));
	}

	void DeferredSurfaces::Resolve(nvrhi::ICommandList* commands, nvrhi::IBindingSet* view, nvrhi::IBindingSet* probe,
								   nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer)
	{
		PX_CORE_VERIFY(m_Ready && commands, "Deferred resolve requires prepared targets and an open command list");
		nvrhi::GraphicsState state;
		state.pipeline = pipeline ? pipeline : m_ResolvePipeline.Get();
		state.framebuffer = framebuffer ? framebuffer : m_ResolveFramebuffer.Get();
		state.bindings = {view, m_SurfaceBindings, probe};
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(m_HDR->getDesc().width), float(m_HDR->getDesc().height)));
		commands->setGraphicsState(state);
		const GPU::DrawConstants unused{};
		commands->setPushConstants(&unused, sizeof(unused));
		commands->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

}
