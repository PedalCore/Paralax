#include "pxpch.h"
#include "Paralax/Renderer/SpatialAA.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/ShaderLibrary.h"

#include <glm/glm.hpp>

#include <cstring>

namespace Paralax {

	SpatialAA::SpatialAA(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
	{
	}

	SpatialAA::~SpatialAA()
	{
		Release();
	}

	void SpatialAA::Release()
	{
		m_Ready = false;
		m_Framebuffer = nullptr;
		m_Bindings = nullptr;
		m_Pipeline = nullptr;
		m_LDR = nullptr;
		m_Textures.Clear();
	}

	bool SpatialAA::Fail(const char* reason)
	{
		if (std::strcmp(m_Reason, reason))
			PX_CORE_ERROR("Spatial AA unavailable ({}); using unfiltered output", reason);
		m_Reason = reason;
		Release();
		return false;
	}

	bool SpatialAA::Prepare(bool needed, nvrhi::IFramebuffer* output)
	{
		if (!needed)
		{
			Release();
			m_Reason = "";
			return false;
		}
		if (!output || output->getFramebufferInfo().colorFormats.size() != 1 || output->getFramebufferInfo().sampleCount != 1)
			return Fail("invalid spatial AA output");
		const auto& info = output->getFramebufferInfo();
		if (m_Ready && m_LDR->getDesc().width == info.width && m_LDR->getDesc().height == info.height && m_OutputInfo == static_cast<const nvrhi::FramebufferInfo&>(info))
			return true;
		Release();
		auto* device = m_Graphics.GetDevice();
		if ((m_Factory.QueryFormatSupport(device, nvrhi::Format::RGBA16_FLOAT) & nvrhi::FormatSupport::ShaderSample) == nvrhi::FormatSupport::None)
			return Fail("spatial AA linear filtering unsupported");
		nvrhi::TextureDesc desc;
		desc.width = info.width;
		desc.height = info.height;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "NativeSpatialAAInput";
		m_LDR = m_Textures.Create(desc);
		if (!m_LDR)
			return Fail(m_Textures.GetFailureReason());
		if (!m_Layout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::vec4)), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0)};
			++m_ObjectAttempts;
			m_Layout = m_Factory.CreateBindingLayout(device, layout);
		}
		if (!m_Sampler)
		{
			++m_ObjectAttempts;
			m_Sampler = m_Factory.CreateSampler(device, nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		}
		if (!m_Layout || !m_Sampler)
			return Fail("spatial AA layout or sampler creation failed");
		nvrhi::BindingSetDesc bindings;
		bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec4)), nvrhi::BindingSetItem::Texture_SRV(0, m_LDR), nvrhi::BindingSetItem::Sampler(0, m_Sampler)};
		++m_ObjectAttempts;
		m_Bindings = m_Factory.CreateBindingSet(device, bindings, m_Layout);
		++m_ObjectAttempts;
		m_Framebuffer = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(m_LDR));
		if (!m_Bindings || !m_Framebuffer)
			return Fail("spatial AA binding or framebuffer creation failed");
		nvrhi::GraphicsPipelineDesc pipeline;
		pipeline.VS = ShaderLibrary::CreateShader(device, "SpatialAA", "SpatialVS", nvrhi::ShaderType::Vertex);
		pipeline.PS = ShaderLibrary::CreateShader(device, "SpatialAA", "SpatialPS", nvrhi::ShaderType::Pixel);
		if (!pipeline.VS || !pipeline.PS)
			return Fail("spatial AA shader creation failed");
		pipeline.bindingLayouts = {m_Layout};
		pipeline.renderState.rasterState.setCullNone();
		pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
		++m_ObjectAttempts;
		m_Pipeline = m_Factory.CreatePipeline(device, pipeline, info);
		if (!m_Pipeline)
			return Fail("spatial AA pipeline creation failed");
		m_OutputInfo = info;
		m_Ready = true;
		m_Reason = "";
		return true;
	}

	void SpatialAA::Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output)
	{
		PX_CORE_VERIFY(m_Ready && commands && output, "Spatial AA rendering requires prepared resources and an output");
		const auto& info = output->getFramebufferInfo();
		PX_CORE_VERIFY(info.width == m_LDR->getDesc().width && info.height == m_LDR->getDesc().height && static_cast<const nvrhi::FramebufferInfo&>(info) == m_OutputInfo,
					   "Spatial AA output changed after preparation");
		nvrhi::GraphicsState state;
		state.pipeline = m_Pipeline;
		state.framebuffer = output;
		state.bindings = {m_Bindings};
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(info.width), float(info.height)));
		commands->setGraphicsState(state);
		const glm::vec4 size(static_cast<float>(info.width), static_cast<float>(info.height), 0, 0);
		commands->setPushConstants(&size, sizeof(size));
		commands->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

}
