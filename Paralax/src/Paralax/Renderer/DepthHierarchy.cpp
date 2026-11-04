#include "pxpch.h"
#include "Paralax/Renderer/DepthHierarchy.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/ShaderLibrary.h"

namespace Paralax {

	uint32_t DepthHierarchy::LevelCount(glm::uvec2 size)
	{
		if (!size.x || !size.y || size.x > (1u << (MaxLevels - 1)) || size.y > (1u << (MaxLevels - 1)))
			return 0;
		uint32_t count = 1;
		while (size.x > 1 || size.y > 1)
		{
			size = (size + 1u) / 2u;
			++count;
		}
		return count;
	}

	glm::uvec2 DepthHierarchy::LevelSize(glm::uvec2 size, uint32_t level)
	{
		if (level >= LevelCount(size))
			return {};
		const uint32_t footprint = 1u << level;
		return (size + footprint - 1u) / footprint;
	}

	DepthHierarchy::DepthHierarchy(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
	{
	}

	DepthHierarchy::~DepthHierarchy()
	{
		Release();
	}

	void DepthHierarchy::Release()
	{
		m_Count = 0;
		m_Bindings = {};
		m_Framebuffers = {};
		m_Pipelines = {};
		m_Layout = nullptr;
		m_Levels = {};
		m_Depth = nullptr;
		m_Coverage = nullptr;
		m_Textures.Clear();
	}

	bool DepthHierarchy::Fail(const char* reason)
	{
		m_Reason = reason;
		Release();
		return false;
	}

	bool DepthHierarchy::Prepare(nvrhi::ITexture* depth, nvrhi::ITexture* coverage, uint32_t coverageComponent)
	{
		if (IsReady() && depth == m_Depth && coverage == m_Coverage && coverageComponent == m_Component)
			return true;
		Release();
		if (!depth || !coverage || coverageComponent > 3)
			return Fail("missing depth hierarchy inputs or invalid coverage component");
		const auto& desc = depth->getDesc();
		const auto& guide = coverage->getDesc();
		// Typed float SRVs cannot read integer formats; missing channels are not stored coverage.
		const uint32_t channels = guide.format == nvrhi::Format::RGBA16_FLOAT || guide.format == nvrhi::Format::RGBA32_FLOAT ? 4u
								  : guide.format == nvrhi::Format::R32_FLOAT || guide.format == nvrhi::Format::R8_UNORM      ? 1u
																															 : 0u;
		if ((desc.format != nvrhi::Format::D32 && desc.format != nvrhi::Format::R32_FLOAT) || coverageComponent >= channels)
			return Fail("unsupported depth hierarchy numeric format or coverage channel");
		// NVRHI samples depth images in a depth read-only layout, which requires attachment usage in Vulkan.
		if (desc.format == nvrhi::Format::D32 && !desc.isRenderTarget)
			return Fail("depth hierarchy D32 input requires depth attachment usage");
		const uint32_t count = LevelCount({desc.width, desc.height});
		if (!count || desc.width > m_Graphics.GetInfo().Limits.MaxTextureDimension2D || desc.height > m_Graphics.GetInfo().Limits.MaxTextureDimension2D ||
			desc.width != guide.width || desc.height != guide.height || desc.dimension != nvrhi::TextureDimension::Texture2D ||
			guide.dimension != nvrhi::TextureDimension::Texture2D || desc.sampleCount != 1 || guide.sampleCount != 1 || !desc.isShaderResource || !guide.isShaderResource)
			return Fail("unsupported depth hierarchy dimensions or inputs");
		auto* device = m_Graphics.GetDevice();
		for (auto format : {desc.format, guide.format})
			if ((m_Factory.QueryFormatSupport(device, format) & nvrhi::FormatSupport::ShaderLoad) == nvrhi::FormatSupport::None)
				return Fail("depth hierarchy input sampling unsupported");
		m_Depth = depth;
		m_Coverage = coverage;
		m_Component = coverageComponent;
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::AllGraphics;
		layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::uvec4)), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
		++m_ObjectAttempts;
		m_Layout = m_Factory.CreateBindingLayout(device, layout);
		if (!m_Layout)
			return Fail("depth hierarchy layout creation failed");
		for (uint32_t level = 0; level < count; ++level)
		{
			const auto size = LevelSize({desc.width, desc.height}, level);
			nvrhi::TextureDesc target;
			target.width = size.x;
			target.height = size.y;
			target.format = nvrhi::Format::RG32_FLOAT;
			target.isRenderTarget = true;
			target.initialState = nvrhi::ResourceStates::ShaderResource;
			target.keepInitialState = true;
			target.debugName = "OpaqueDepthInterval";
			m_Levels[level] = m_Textures.Create(target);
			if (!m_Levels[level])
				return Fail(m_Textures.GetFailureReason());
			++m_ObjectAttempts;
			m_Framebuffers[level] = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(m_Levels[level]));
			if (!m_Framebuffers[level])
				return Fail("depth hierarchy framebuffer creation failed");
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::uvec4)),
							nvrhi::BindingSetItem::Texture_SRV(0, level ? m_Levels[level - 1] : depth), nvrhi::BindingSetItem::Texture_SRV(1, coverage)};
			++m_ObjectAttempts;
			m_Bindings[level] = m_Factory.CreateBindingSet(device, set, m_Layout);
			if (!m_Bindings[level])
				return Fail("depth hierarchy binding creation failed");
		}
		auto vs = ShaderLibrary::CreateShader(device, "DepthHierarchy", "HierarchyVS", nvrhi::ShaderType::Vertex);
		for (uint32_t pass = 0; pass < m_Pipelines.size(); ++pass)
		{
			auto ps = ShaderLibrary::CreateShader(device, "DepthHierarchy", pass ? "ReducePS" : "CopyPS", nvrhi::ShaderType::Pixel);
			if (!vs || !ps)
				return Fail("depth hierarchy shader creation failed");
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = vs;
			pipeline.PS = ps;
			pipeline.bindingLayouts = {m_Layout};
			pipeline.renderState.rasterState.setCullNone();
			pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			++m_ObjectAttempts;
			m_Pipelines[pass] = m_Factory.CreatePipeline(device, pipeline, m_Framebuffers[0]->getFramebufferInfo());
			if (!m_Pipelines[pass])
				return Fail("depth hierarchy pipeline creation failed");
		}
		m_Count = count;
		m_Reason = "";
		return true;
	}

	void DepthHierarchy::Render(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(IsReady() && commands, "Depth hierarchy render requires prepared resources and an open command list");
		for (uint32_t level = 0; level < m_Count; ++level)
		{
			const auto& input = level ? m_Levels[level - 1]->getDesc() : m_Depth->getDesc();
			const auto& output = m_Levels[level]->getDesc();
			nvrhi::GraphicsState state;
			state.pipeline = m_Pipelines[level ? 1 : 0];
			state.framebuffer = m_Framebuffers[level];
			state.bindings = {m_Bindings[level]};
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(output.width), float(output.height)));
			commands->setGraphicsState(state);
			const glm::uvec4 constants(input.width, input.height, m_Component, 0);
			commands->setPushConstants(&constants, sizeof(constants));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		}
	}

}
