#include "pxpch.h"
#include "Paralax/Renderer/RenderScale.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/ShaderLibrary.h"

#include <cmath>
#include <cstring>

namespace Paralax {

	RenderScale::RenderScale(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
	{
	}

	RenderScale::~RenderScale()
	{
		Release();
	}

	glm::uvec2 RenderScale::CalculateDimensions(uint32_t outputWidth, uint32_t outputHeight, float scale)
	{
		const double factor = std::isfinite(scale) ? static_cast<double>(std::clamp(scale, 0.5f, 1.0f)) : 1.0;
		auto dimension = [&](uint32_t output) {
			return output == 0 ? 0u : static_cast<uint32_t>(std::clamp(std::floor(static_cast<double>(output) * factor + 0.5), 1.0, static_cast<double>(output)));
		};
		return {dimension(outputWidth), dimension(outputHeight)};
	}

	void RenderScale::Release()
	{
		m_Ready = false;
		m_Bindings = nullptr;
		m_Framebuffer = nullptr;
		m_Pipeline = nullptr;
		m_Source = nullptr;
		m_NativeDepth = nullptr;
		m_Textures.Clear();
	}

	bool RenderScale::MatchesInputs(nvrhi::ITexture* sceneDepth, uint32_t outputWidth, uint32_t outputHeight) const
	{
		return m_Ready && m_Source == sceneDepth && m_NativeDepth->getDesc().width == outputWidth && m_NativeDepth->getDesc().height == outputHeight;
	}

	bool RenderScale::Fail(const char* reason)
	{
		if (std::strcmp(m_Reason, reason) != 0)
			PX_CORE_ERROR("Render scale unavailable ({}); using native scene resolution", reason);
		m_Reason = reason;
		Release();
		return false;
	}

	bool RenderScale::Prepare(bool needed, nvrhi::ITexture* sceneDepth, uint32_t outputWidth, uint32_t outputHeight)
	{
		if (!needed)
		{
			Release();
			m_Reason = "";
			return false;
		}
		if (MatchesInputs(sceneDepth, outputWidth, outputHeight))
			return true;
		Release();
		if (!sceneDepth || !outputWidth || !outputHeight || sceneDepth->getDesc().format != nvrhi::Format::D32)
			return Fail("invalid scene depth or output dimensions");
		auto* device = m_Graphics.GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = outputWidth;
		desc.height = outputHeight;
		desc.format = nvrhi::Format::D32;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::DepthWrite;
		desc.keepInitialState = true;
		desc.debugName = "NativeTextDepth";
		m_NativeDepth = m_Textures.Create(desc);
		if (!m_NativeDepth)
			return Fail(m_Textures.GetFailureReason());
		m_Source = sceneDepth;
		if (!m_Layout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::uvec4)), nvrhi::BindingLayoutItem::Texture_SRV(0)};
			++m_ObjectAttempts;
			m_Layout = m_Factory.CreateBindingLayout(device, layout);
		}
		if (!m_Layout)
			return Fail("native depth layout creation failed");
		nvrhi::BindingSetDesc bindings;
		bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::uvec4)), nvrhi::BindingSetItem::Texture_SRV(0, sceneDepth)};
		++m_ObjectAttempts;
		m_Bindings = m_Factory.CreateBindingSet(device, bindings, m_Layout);
		++m_ObjectAttempts;
		m_Framebuffer = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().setDepthAttachment(m_NativeDepth));
		if (!m_Bindings || !m_Framebuffer)
			return Fail("native depth framebuffer or binding creation failed");
		auto vs = ShaderLibrary::CreateShader(device, "NativeDepth", "DepthVS", nvrhi::ShaderType::Vertex);
		auto ps = ShaderLibrary::CreateShader(device, "NativeDepth", "DepthPS", nvrhi::ShaderType::Pixel);
		if (!vs || !ps)
			return Fail("native depth shader creation failed");
		nvrhi::GraphicsPipelineDesc pipeline;
		pipeline.VS = vs;
		pipeline.PS = ps;
		pipeline.bindingLayouts = {m_Layout};
		pipeline.renderState.rasterState.setCullNone();
		pipeline.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::Always);
		++m_ObjectAttempts;
		m_Pipeline = m_Factory.CreatePipeline(device, pipeline, m_Framebuffer->getFramebufferInfo());
		if (!m_Pipeline)
			return Fail("native depth pipeline creation failed");
		m_Reason = "";
		m_Ready = true;
		return true;
	}

	void RenderScale::RenderDepth(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(m_Ready && commands, "Native depth reconstruction requires prepared resources and an open command list");
		nvrhi::GraphicsState state;
		state.pipeline = m_Pipeline;
		state.framebuffer = m_Framebuffer;
		state.bindings = {m_Bindings};
		const auto& output = m_NativeDepth->getDesc();
		const auto& input = m_Source->getDesc();
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(output.width), float(output.height)));
		commands->setGraphicsState(state);
		const glm::uvec4 dimensions(input.width, input.height, output.width, output.height);
		commands->setPushConstants(&dimensions, sizeof(dimensions));
		commands->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

}
