#include "pxpch.h"
#include "Paralax/Renderer/FXAA.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/ShaderLibrary.h"

#include <glm/glm.hpp>

#include <cstring>

namespace Paralax {

	FXAA::FXAA(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Budget(budget)
		, m_Graphics(graphics)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
	{
	}

	FXAA::~FXAA()
	{
		Release();
	}

	void FXAA::Release()
	{
		ReleaseResources();
		m_Failed = false;
		m_Reason = "";
	}

	void FXAA::ReleaseResources()
	{
		m_Ready = false;
		m_Framebuffer = nullptr;
		m_Bindings = nullptr;
		m_Pipeline = nullptr;
		m_Layout = nullptr;
		m_Sampler = nullptr;
		m_LDR = nullptr;
		m_Textures.Clear();
	}

	bool FXAA::Fail(const char* reason)
	{
		if (std::strcmp(m_Reason, reason))
			PX_CORE_ERROR("FXAA unavailable ({}); using unfiltered output", reason);
		ReleaseResources();
		m_Reason = reason;
		m_Failed = true;
		m_FailedAvailableBytes = m_Budget.GetCapacity() - m_Budget.GetUsedBytes();
		return false;
	}

	bool FXAA::Prepare(bool needed, nvrhi::IFramebuffer* output)
	{
		if (!needed)
		{
			Release();
			m_Reason = "";
			return false;
		}
		if (MatchesOutput(output))
			return true;
		ReleaseResources();
		const nvrhi::FramebufferInfo empty;
		const auto& compatibility = output ? static_cast<const nvrhi::FramebufferInfo&>(output->getFramebufferInfo()) : empty;
		const uint32_t width = output ? output->getFramebufferInfo().width : 0;
		const uint32_t height = output ? output->getFramebufferInfo().height : 0;
		const uint32_t arraySize = output ? output->getFramebufferInfo().arraySize : 0;
		m_Budget.CollectRetired();
		const auto available = m_Budget.GetCapacity() - m_Budget.GetUsedBytes();
		if (m_Failed && m_FailedInfo == compatibility && m_FailedWidth == width && m_FailedHeight == height && m_FailedArraySize == arraySize && m_FailedAvailableBytes == available)
			return false;
		m_FailedInfo = compatibility;
		m_FailedWidth = width;
		m_FailedHeight = height;
		m_FailedArraySize = arraySize;
		if (!output || arraySize != 1 || !width || !height || width > 8192 || height > 8192 || compatibility.colorFormats.size() != 1 || compatibility.sampleCount != 1)
			return Fail("invalid FXAA output");
		const auto& info = output->getFramebufferInfo();
		const auto format = info.colorFormats[0];
		if (format != nvrhi::Format::RGBA8_UNORM && format != nvrhi::Format::SRGBA8_UNORM &&
			format != nvrhi::Format::BGRA8_UNORM && format != nvrhi::Format::SBGRA8_UNORM &&
			format != nvrhi::Format::RGBA16_FLOAT && format != nvrhi::Format::RGBA32_FLOAT)
			return Fail("unsupported FXAA output format");
		auto* device = m_Graphics.GetDevice();
		const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderLoad | nvrhi::FormatSupport::ShaderSample | nvrhi::FormatSupport::RenderTarget;
		if ((m_Factory.QueryFormatSupport(device, nvrhi::Format::RGBA16_FLOAT) & required) != required ||
			(m_Factory.QueryFormatSupport(device, format) & nvrhi::FormatSupport::RenderTarget) == nvrhi::FormatSupport::None)
			return Fail("FXAA texture or linear filtering unsupported");
		nvrhi::TextureDesc desc;
		desc.width = info.width;
		desc.height = info.height;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "NativeFXAAInput";
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
			return Fail("FXAA layout or sampler creation failed");
		nvrhi::BindingSetDesc bindings;
		bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec4)), nvrhi::BindingSetItem::Texture_SRV(0, m_LDR), nvrhi::BindingSetItem::Sampler(0, m_Sampler)};
		++m_ObjectAttempts;
		m_Bindings = m_Factory.CreateBindingSet(device, bindings, m_Layout);
		++m_ObjectAttempts;
		m_Framebuffer = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(m_LDR));
		if (!m_Bindings || !m_Framebuffer)
			return Fail("FXAA binding or framebuffer creation failed");
		nvrhi::GraphicsPipelineDesc pipeline;
		pipeline.VS = ShaderLibrary::CreateShader(device, "FXAA", "FXAAVS", nvrhi::ShaderType::Vertex);
		pipeline.PS = ShaderLibrary::CreateShader(device, "FXAA", "FXAAPS", nvrhi::ShaderType::Pixel);
		if (!pipeline.VS || !pipeline.PS)
			return Fail("FXAA shader creation failed");
		pipeline.bindingLayouts = {m_Layout};
		pipeline.renderState.rasterState.setCullNone();
		pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
		++m_ObjectAttempts;
		m_Pipeline = m_Factory.CreatePipeline(device, pipeline, info);
		if (!m_Pipeline)
			return Fail("FXAA pipeline creation failed");
		m_OutputInfo = info;
		m_Ready = true;
		m_Failed = false;
		m_Reason = "";
		return true;
	}

	bool FXAA::MatchesOutput(nvrhi::IFramebuffer* output) const
	{
		return m_Ready && output && output->getFramebufferInfo().arraySize == 1 && m_LDR->getDesc().width == output->getFramebufferInfo().width &&
			   m_LDR->getDesc().height == output->getFramebufferInfo().height &&
			   m_OutputInfo == static_cast<const nvrhi::FramebufferInfo&>(output->getFramebufferInfo());
	}

	void FXAA::Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output)
	{
		PX_CORE_VERIFY(m_Ready && commands && output, "FXAA rendering requires prepared resources and an output");
		const auto& info = output->getFramebufferInfo();
		PX_CORE_VERIFY(MatchesOutput(output), "FXAA output changed after preparation");
		nvrhi::GraphicsState state;
		state.pipeline = m_Pipeline;
		state.framebuffer = output;
		state.bindings = {m_Bindings};
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(info.width), float(info.height)));
		commands->setTextureState(m_LDR, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		commands->setGraphicsState(state);
		const glm::vec4 size(static_cast<float>(info.width), static_cast<float>(info.height), 0, 0);
		commands->setPushConstants(&size, sizeof(size));
		commands->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

}
