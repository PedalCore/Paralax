#include "pxpch.h"
#include "Paralax/Renderer/AmbientOcclusion.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/SceneRendererTypes.h"
#include "Paralax/Renderer/ShaderLibrary.h"

#include <cstring>

namespace Paralax {

	namespace {
		struct AOConstants
		{
			glm::mat4 InverseProjection;
			glm::vec4 Size, Settings, Pass, Padding;
		};
		static_assert(sizeof(AOConstants) == 128);
	}

	AmbientOcclusion::AmbientOcclusion(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
	{
	}

	AmbientOcclusion::~AmbientOcclusion()
	{
		Release();
	}

	void AmbientOcclusion::Release()
	{
		m_Ready = false;
		m_Bindings = {};
		m_Framebuffers = {};
		m_GuideFramebuffer = nullptr;
		m_ViewBindings = nullptr;
		m_Pipelines = {};
		m_GuidePipelines = {};
		m_Targets = {};
		m_Guides = {};
		m_Depth = nullptr;
		m_DeferredGuide = nullptr;
		m_Textures.Clear();
	}

	bool AmbientOcclusion::Fail(const char* reason)
	{
		if (std::strcmp(m_Reason, reason) != 0)
			PX_CORE_ERROR("SSAO unavailable ({}); using neutral occlusion", reason);
		m_Reason = reason;
		Release();
		return false;
	}

	bool AmbientOcclusion::Prepare(bool enabled, nvrhi::ITexture* depth, nvrhi::ITexture* deferredGuide, nvrhi::ITexture* neutral,
								   nvrhi::IBuffer* viewConstants, nvrhi::IInputLayout* input, nvrhi::IBindingLayout* view,
								   nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe)
	{
		if (!enabled)
		{
			if (m_Ready || m_Depth)
			{
				Release();
				++m_Generation;
			}
			m_Reason = "";
			return false;
		}
		if (m_Ready && m_Depth == depth && m_DeferredGuide == deferredGuide)
			return true;
		Release();
		++m_Generation;
		if (!depth || !neutral || !viewConstants || !input || !view || !material || !probe)
			return Fail("missing guide inputs");
		auto* device = m_Graphics.GetDevice();
		if ((device->queryFormatSupport(depth->getDesc().format) & nvrhi::FormatSupport::ShaderLoad) == nvrhi::FormatSupport::None)
			return Fail("depth sampling unsupported");
		m_Depth = depth;
		m_DeferredGuide = deferredGuide;
		const auto& size = depth->getDesc();
		auto createTarget = [&](uint32_t width, uint32_t height, nvrhi::Format format, const char* name) {
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = format;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = name;
			return m_Textures.Create(desc);
		};
		constexpr std::array formats = {nvrhi::Format::R32_FLOAT, nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::R16_FLOAT, nvrhi::Format::R16_FLOAT, nvrhi::Format::R16_FLOAT};
		constexpr std::array names = {"AOHalfDepth", "AOHalfMetadata", "AORaw", "AOFiltered", "AOFull"};
		for (size_t i = 0; i < m_Targets.size(); ++i)
		{
			m_Targets[i] = createTarget(i == 4 ? size.width : (size.width + 1) / 2, i == 4 ? size.height : (size.height + 1) / 2, formats[i], names[i]);
			if (!m_Targets[i])
				return Fail(m_Textures.GetFailureReason());
		}
		if (!deferredGuide)
		{
			m_Guides[0] = createTarget(size.width, size.height, nvrhi::Format::RG16_FLOAT, "AOReceiverNormal");
			m_Guides[1] = createTarget(size.width, size.height, nvrhi::Format::R32_FLOAT, "AOCoverage");
			if (!m_Guides[0] || !m_Guides[1])
				return Fail(m_Textures.GetFailureReason());
			++m_ObjectAttempts;
			m_GuideFramebuffer = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(m_Guides[0]).addColorAttachment(m_Guides[1]).setDepthAttachment(depth));
			if (!m_GuideFramebuffer)
				return Fail("guide framebuffer creation failed");
			auto vs = ShaderLibrary::CreateShader(device, "SurfaceGuide", "GuideVS", nvrhi::ShaderType::Vertex);
			auto ps = ShaderLibrary::CreateShader(device, "SurfaceGuide", "GuidePS", nvrhi::ShaderType::Pixel);
			if (!vs || !ps)
				return Fail("guide shader creation failed");
			for (size_t i = 0; i < m_GuidePipelines.size(); ++i)
			{
				nvrhi::GraphicsPipelineDesc pipeline;
				pipeline.VS = vs;
				pipeline.PS = ps;
				pipeline.inputLayout = input;
				pipeline.bindingLayouts = {view, material, probe};
				pipeline.renderState.rasterState.setFrontCounterClockwise(i != 1).setCullMode(i == 2 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back);
				pipeline.renderState.depthStencilState.enableDepthTest().enableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				++m_ObjectAttempts;
				m_GuidePipelines[i] = m_Factory.CreatePipeline(device, pipeline, m_GuideFramebuffer->getFramebufferInfo());
				if (!m_GuidePipelines[i])
					return Fail("guide pipeline creation failed");
			}
		}
		if (!m_ViewLayout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::AllGraphics;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::PushConstants(15, sizeof(AOConstants))};
			++m_ObjectAttempts;
			m_ViewLayout = m_Factory.CreateBindingLayout(device, layout);
		}
		if (!m_InputLayout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::AllGraphics;
			layout.registerSpaceIsDescriptorSet = true;
			layout.registerSpace = 1;
			layout.bindings.clear();
			for (uint32_t slot = 0; slot < 6; ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			++m_ObjectAttempts;
			m_InputLayout = m_Factory.CreateBindingLayout(device, layout);
		}
		if (!m_ViewLayout || !m_InputLayout)
			return Fail("AO layout creation failed");
		nvrhi::BindingSetDesc viewSet;
		viewSet.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, viewConstants), nvrhi::BindingSetItem::PushConstants(15, sizeof(AOConstants))};
		++m_ObjectAttempts;
		m_ViewBindings = m_Factory.CreateBindingSet(device, viewSet, m_ViewLayout);
		if (!m_ViewBindings)
			return Fail("AO view binding creation failed");
		auto vs = ShaderLibrary::CreateShader(device, "AmbientOcclusion", "FullscreenVS", nvrhi::ShaderType::Vertex);
		constexpr std::array entries = {"GuidePS", "OcclusionPS", "FilterPS", "FilterPS", "UpsamplePS"};
		constexpr std::array outputIndices = {0u, 2u, 3u, 2u, 4u};
		for (size_t pass = 0; pass < m_Pipelines.size(); ++pass)
		{
			nvrhi::FramebufferDesc framebuffer;
			framebuffer.addColorAttachment(m_Targets[outputIndices[pass]]);
			if (pass == 0)
				framebuffer.addColorAttachment(m_Targets[1]);
			++m_ObjectAttempts;
			m_Framebuffers[pass] = m_Factory.CreateFramebuffer(device, framebuffer);
			if (!m_Framebuffers[pass])
				return Fail("AO framebuffer creation failed");
			const bool fullGuide = pass == 0 || pass == 1 || pass == 4;
			std::array<nvrhi::ITexture*, 6> resources = {fullGuide ? depth : neutral,
														 fullGuide ? (deferredGuide ? deferredGuide : m_Guides[0]) : neutral,
														 fullGuide ? (deferredGuide ? deferredGuide : m_Guides[1]) : neutral,
														 pass > 0 ? m_Targets[0] : neutral, pass > 0 ? m_Targets[1] : neutral,
														 pass >= 2 ? m_Targets[pass == 3 ? 3 : 2] : neutral};
			nvrhi::BindingSetDesc bindings;
			for (uint32_t slot = 0; slot < resources.size(); ++slot)
				bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(slot, resources[slot]));
			++m_ObjectAttempts;
			m_Bindings[pass] = m_Factory.CreateBindingSet(device, bindings, m_InputLayout);
			auto ps = ShaderLibrary::CreateShader(device, "AmbientOcclusion", entries[pass], nvrhi::ShaderType::Pixel);
			if (!vs || !ps || !m_Bindings[pass])
				return Fail("AO shader or binding creation failed");
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = vs;
			pipeline.PS = ps;
			pipeline.bindingLayouts = {m_ViewLayout, m_InputLayout};
			pipeline.renderState.rasterState.setCullNone();
			pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			++m_ObjectAttempts;
			m_Pipelines[pass] = m_Factory.CreatePipeline(device, pipeline, m_Framebuffers[pass]->getFramebufferInfo());
			if (!m_Pipelines[pass])
				return Fail("AO pipeline creation failed");
		}
		m_Reason = "";
		m_Ready = true;
		return true;
	}

	void AmbientOcclusion::ClearGuide(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(NeedsPrepass() && commands, "Guide clear requires prepared Forward AO and an open command list");
		for (auto* texture : m_Guides)
		{
			PostEffectTextures::TrackUsage(commands, texture, nvrhi::ResourceStates::CopyDest);
			commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0));
		}
	}

	void AmbientOcclusion::Render(nvrhi::ICommandList* commands, const glm::mat4& projection, bool orthographic, float radius, float bias, float strength, uint32_t samples)
	{
		PX_CORE_VERIFY(m_Ready && commands && radius > 0 && strength > 0 && samples > 0, "AO render requires prepared resources and positive parameters");
		const auto& size = m_Depth->getDesc();
		AOConstants constants{};
		constants.InverseProjection = glm::inverse(projection);
		constants.Size = glm::vec4(size.width, size.height, (size.width + 1) / 2, (size.height + 1) / 2);
		constants.Settings = glm::vec4(radius, bias, strength, samples);
		constants.Pass = glm::vec4(m_DeferredGuide ? 1 : 0, 0, orthographic ? 1 : 0, 0);
		for (size_t pass = 0; pass < m_Pipelines.size(); ++pass)
		{
			nvrhi::GraphicsState state;
			state.pipeline = m_Pipelines[pass];
			state.framebuffer = m_Framebuffers[pass];
			state.bindings = {m_ViewBindings, m_Bindings[pass]};
			const auto& info = m_Framebuffers[pass]->getFramebufferInfo();
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(info.width), float(info.height)));
			commands->setGraphicsState(state);
			constants.Pass.y = pass == 3 ? 1.0f : 0.0f;
			commands->setPushConstants(&constants, sizeof(constants));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		}
	}

}
