#include "pxpch.h"
#include "Paralax/Renderer/Bloom.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/ShaderLibrary.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstring>

namespace Paralax {

	namespace {
		struct BloomConstants
		{
			glm::vec4 SourceSize, Parameters, Weights;
		};
	}

	Bloom::Bloom(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
	{
	}

	Bloom::~Bloom()
	{
		Release();
	}

	void Bloom::Release()
	{
		m_Ready = false;
		m_DownBindings = {};
		m_UpBindings = {};
		m_DownFramebuffers = {};
		m_UpFramebuffers = {};
		m_DownPipeline = nullptr;
		m_UpPipeline = nullptr;
		m_Down = {};
		m_Up = {};
		m_HDR = nullptr;
		m_Levels = m_RequestedLevels = 0;
		m_Textures.Clear();
	}

	bool Bloom::Fail(const char* reason)
	{
		if (std::strcmp(m_Reason, reason) != 0)
			PX_CORE_ERROR("Bloom unavailable ({}); using zero bloom", reason);
		m_Reason = reason;
		Release();
		return false;
	}

	bool Bloom::Prepare(bool enabled, nvrhi::ITexture* hdr, nvrhi::ITexture* neutral, uint32_t levels)
	{
		if (!enabled)
		{
			Release();
			m_Reason = "";
			return false;
		}
		if (MatchesInputs(hdr, levels))
			return true;
		if (m_Ready && hdr && neutral && levels == m_RequestedLevels && hdr->getDesc().width == m_HDR->getDesc().width && hdr->getDesc().height == m_HDR->getDesc().height && hdr->getDesc().format == m_HDR->getDesc().format)
		{
			// Temporal ping-pong changes the source every frame, not the pyramid allocation.
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)), nvrhi::BindingSetItem::Texture_SRV(0, hdr),
								 nvrhi::BindingSetItem::Texture_SRV(1, neutral), nvrhi::BindingSetItem::Sampler(0, m_Linear)};
			++m_ObjectAttempts;
			auto replacement = m_Factory.CreateBindingSet(m_Graphics.GetDevice(), bindings, m_Layout);
			if (!replacement)
				return Fail("bloom source rebinding failed");
			m_DownBindings[0] = replacement;
			m_HDR = hdr;
			return true;
		}
		Release();
		if (!hdr || !neutral || levels < 1 || levels > MaxLevels)
			return Fail("invalid bloom inputs or level count");
		auto* device = m_Graphics.GetDevice();
		for (auto format : {hdr->getDesc().format, nvrhi::Format::RGBA16_FLOAT})
			if ((m_Factory.QueryFormatSupport(device, format) & nvrhi::FormatSupport::ShaderSample) == nvrhi::FormatSupport::None)
				return Fail("linear HDR filtering unsupported");
		m_HDR = hdr;
		m_RequestedLevels = levels;
		uint32_t width = hdr->getDesc().width, height = hdr->getDesc().height;
		for (uint32_t level = 0; level < levels; ++level)
		{
			width = (width + 1) / 2;
			height = (height + 1) / 2;
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = "BloomDownsample";
			m_Down[level] = m_Textures.Create(desc);
			if (!m_Down[level])
				return Fail(m_Textures.GetFailureReason());
			++m_Levels;
			if (width == 1 && height == 1)
				break;
		}
		for (uint32_t level = 0; level + 1 < m_Levels; ++level)
		{
			auto desc = m_Down[level]->getDesc();
			desc.debugName = "BloomUpsample";
			m_Up[level] = m_Textures.Create(desc);
			if (!m_Up[level])
				return Fail(m_Textures.GetFailureReason());
		}
		if (!m_Layout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BloomConstants)),
							   nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(0)};
			++m_ObjectAttempts;
			m_Layout = m_Factory.CreateBindingLayout(device, layout);
		}
		if (!m_Linear)
		{
			nvrhi::SamplerDesc sampler;
			sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
			++m_ObjectAttempts;
			m_Linear = m_Factory.CreateSampler(device, sampler);
		}
		if (!m_Layout || !m_Linear)
			return Fail("bloom layout or sampler creation failed");
		auto makePass = [&](nvrhi::ITexture* output, nvrhi::ITexture* high, nvrhi::ITexture* low, nvrhi::FramebufferHandle& framebuffer, nvrhi::BindingSetHandle& bindings) {
			++m_ObjectAttempts;
			framebuffer = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(output));
			if (!framebuffer)
				return false;
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)), nvrhi::BindingSetItem::Texture_SRV(0, high),
							nvrhi::BindingSetItem::Texture_SRV(1, low), nvrhi::BindingSetItem::Sampler(0, m_Linear)};
			++m_ObjectAttempts;
			bindings = m_Factory.CreateBindingSet(device, set, m_Layout);
			return bool(bindings);
		};
		for (uint32_t level = 0; level < m_Levels; ++level)
		{
			if (!makePass(m_Down[level], level ? m_Down[level - 1] : hdr, neutral, m_DownFramebuffers[level], m_DownBindings[level]))
				return Fail("bloom downsample framebuffer or binding creation failed");
			if (level + 1 < m_Levels && !makePass(m_Up[level], m_Down[level], level + 2 < m_Levels ? m_Up[level + 1] : m_Down[level + 1], m_UpFramebuffers[level], m_UpBindings[level]))
				return Fail("bloom upsample framebuffer or binding creation failed");
		}
		auto vs = ShaderLibrary::CreateShader(device, "Bloom", "BloomVS", nvrhi::ShaderType::Vertex);
		auto down = ShaderLibrary::CreateShader(device, "Bloom", "DownsamplePS", nvrhi::ShaderType::Pixel);
		auto up = ShaderLibrary::CreateShader(device, "Bloom", "UpsamplePS", nvrhi::ShaderType::Pixel);
		if (!vs || !down || !up)
			return Fail("bloom shader creation failed");
		nvrhi::GraphicsPipelineDesc pipeline;
		pipeline.VS = vs;
		pipeline.PS = down;
		pipeline.bindingLayouts = {m_Layout};
		pipeline.renderState.rasterState.setCullNone();
		pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
		++m_ObjectAttempts;
		m_DownPipeline = m_Factory.CreatePipeline(device, pipeline, m_DownFramebuffers[0]->getFramebufferInfo());
		pipeline.PS = up;
		++m_ObjectAttempts;
		m_UpPipeline = m_Factory.CreatePipeline(device, pipeline, m_DownFramebuffers[0]->getFramebufferInfo());
		if (!m_DownPipeline || !m_UpPipeline)
			return Fail("bloom pipeline creation failed");
		m_Ready = true;
		m_Reason = "";
		return true;
	}

	void Bloom::Render(nvrhi::ICommandList* commands, float threshold, float softKnee)
	{
		PX_CORE_VERIFY(m_Ready && commands && std::isfinite(threshold) && threshold >= 0 && threshold <= 64 &&
						   std::isfinite(softKnee) && softKnee >= 0 && softKnee <= 1,
					   "Bloom render requires prepared resources and bounded finite parameters");
		BloomConstants constants{};
		constants.Parameters = glm::vec4(threshold, softKnee, 0, 0);
		auto draw = [&](nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* bindings, nvrhi::IGraphicsPipeline* pipeline) {
			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.bindings = {bindings};
			const auto& info = framebuffer->getFramebufferInfo();
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(info.width), float(info.height)));
			commands->setGraphicsState(state);
			commands->setPushConstants(&constants, sizeof(constants));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		};
		for (uint32_t level = 0; level < m_Levels; ++level)
		{
			const auto& source = level ? m_Down[level - 1]->getDesc() : m_HDR->getDesc();
			const auto& target = m_Down[level]->getDesc();
			constants.SourceSize = glm::vec4(source.width, source.height, target.width, target.height);
			constants.Parameters.z = level == 0 ? 1.0f : 0.0f;
			draw(m_DownFramebuffers[level], m_DownBindings[level], m_DownPipeline);
		}
		for (uint32_t level = m_Levels - 1; level > 0; --level)
		{
			const auto& source = m_Down[level]->getDesc();
			const auto& target = m_Down[level - 1]->getDesc();
			constants.SourceSize = glm::vec4(source.width, source.height, target.width, target.height);
			const float count = static_cast<float>(m_Levels - level + 1);
			constants.Weights = glm::vec4(1.0f / count, (count - 1) / count, 0, 0);
			draw(m_UpFramebuffers[level - 1], m_UpBindings[level - 1], m_UpPipeline);
		}
	}

}
