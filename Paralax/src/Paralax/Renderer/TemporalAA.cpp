#include "pxpch.h"
#include "Paralax/Renderer/TemporalAA.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/SceneRendererTypes.h"
#include "Paralax/Renderer/ShaderLibrary.h"

#include <bit>
#include <cstring>

namespace Paralax {

	namespace {

		struct ResolveConstants
		{
			glm::mat4 InverseProjection;
			glm::vec4 JitterHistory; // xy displacement, z history weight, w usable history
			glm::uvec4 Size;
		};
		static_assert(sizeof(ResolveConstants) == 96);

	}

	TemporalAA::TemporalAA(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
		, m_Buffers(graphics, budget, &m_Factory)
	{
	}

	TemporalAA::~TemporalAA()
	{
		Release();
	}

	void TemporalAA::Release()
	{
		m_Ready = false;
		m_State.Reset("temporal resources released");
		m_DrawBindings = nullptr;
		m_ResolveBindings = {};
		m_GuideFramebuffer = nullptr;
		m_ReactiveFramebuffer = nullptr;
		m_ResolveFramebuffers = {};
		m_GuidePipelines = {};
		m_ReactivePipeline = nullptr;
		m_ResolvePipeline = nullptr;
		m_HDR = nullptr;
		m_Depth = nullptr;
		m_DrawBuffer = nullptr;
		m_Guides = {};
		m_Reactive = nullptr;
		m_History = {};
		m_Textures.Clear();
		m_Buffers.Clear();
		m_BufferCapacity = 0;
		m_WriteIndex = 0;
		m_ClearHistory = true;
	}

	bool TemporalAA::Fail(const char* reason)
	{
		if (std::strcmp(reason, m_Reason))
			PX_CORE_ERROR("Temporal AA unavailable ({}); using a fallback without jitter", reason);
		m_Reason = reason;
		Release();
		m_State.Abort(reason);
		return false;
	}

	nvrhi::IGraphicsPipeline* TemporalAA::GetGuidePipeline(bool doubleSided, bool mirrored) const
	{
		return m_GuidePipelines[doubleSided ? 2 : mirrored ? 1
														   : 0];
	}

	bool TemporalAA::CreateBindings()
	{
		auto* device = m_Graphics.GetDevice();
		m_GuideFramebuffer = nullptr;
		m_ReactiveFramebuffer = nullptr;
		m_ResolveBindings = {};
		nvrhi::FramebufferDesc guide;
		for (auto* texture : m_Guides)
			guide.addColorAttachment(texture);
		guide.setDepthAttachment(m_Depth);
		// The pinned backend always stores depth attachments at pass end. Track that store
		// as DepthWrite even though the guide pipelines disable per-fragment depth writes.
		++m_ObjectAttempts;
		m_GuideFramebuffer = m_Factory.CreateFramebuffer(device, guide);
		nvrhi::FramebufferDesc reactive;
		reactive.addColorAttachment(m_Reactive).setDepthAttachment(m_Depth);

		++m_ObjectAttempts;
		m_ReactiveFramebuffer = m_Factory.CreateFramebuffer(device, reactive);
		if (!m_GuideFramebuffer || !m_ReactiveFramebuffer)
			return false;
		for (uint32_t index = 0; index < 2; ++index)
		{
			const auto& previous = m_History[1 - index];
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(ResolveConstants)),
								 nvrhi::BindingSetItem::Texture_SRV(0, m_HDR), nvrhi::BindingSetItem::Texture_SRV(1, m_Depth),
								 nvrhi::BindingSetItem::Texture_SRV(2, m_Guides[0]), nvrhi::BindingSetItem::Texture_SRV(3, m_Guides[1]),
								 nvrhi::BindingSetItem::Texture_SRV(4, m_Guides[2]), nvrhi::BindingSetItem::Texture_SRV(5, m_Guides[3]),
								 nvrhi::BindingSetItem::Texture_SRV(6, m_Reactive), nvrhi::BindingSetItem::Texture_SRV(7, previous[0]),
								 nvrhi::BindingSetItem::Texture_SRV(8, previous[1]), nvrhi::BindingSetItem::Texture_SRV(9, previous[2]),
								 nvrhi::BindingSetItem::Texture_SRV(10, previous[3])};
			++m_ObjectAttempts;
			m_ResolveBindings[index] = m_Factory.CreateBindingSet(device, bindings, m_ResolveLayout);
			if (!m_ResolveBindings[index])
				return false;
		}
		return true;
	}

	bool TemporalAA::Prepare(nvrhi::ITexture* hdr, nvrhi::ITexture* depth, uint32_t drawCount, nvrhi::IInputLayout* input,
							 nvrhi::IBindingLayout* view, nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe)
	{
		if (!hdr || !depth || !input || !view || !material || !probe || drawCount > TemporalState::MaxDraws ||
			hdr->getDesc().width != depth->getDesc().width || hdr->getDesc().height != depth->getDesc().height)
			return Fail("invalid temporal inputs");
		const auto& size = hdr->getDesc();
		if (!size.width || !size.height || size.sampleCount != 1 || depth->getDesc().sampleCount != 1 || depth->getDesc().format != nvrhi::Format::D32)
			return Fail("invalid temporal texture shape or depth format");
		auto* device = m_Graphics.GetDevice();
		if (!m_Factory.SupportsVirtualResources(device))
			return Fail("temporal memory preflight unsupported");
		const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderLoad | nvrhi::FormatSupport::RenderTarget;
		// Reject missing capabilities before any backing allocation, including the draw buffer.
		for (auto format : {nvrhi::Format::RG16_FLOAT, nvrhi::Format::R32_UINT, nvrhi::Format::RG32_FLOAT,
							nvrhi::Format::R32_FLOAT, nvrhi::Format::R8_UNORM, nvrhi::Format::RGBA16_FLOAT})
			if ((m_Factory.QueryFormatSupport(device, format) & required) != required)
				return Fail("temporal texture format unsupported");
		const bool sameSize = m_Ready && m_HDR->getDesc().width == size.width && m_HDR->getDesc().height == size.height;
		if (!sameSize)
			Release();
		if (!m_DrawLayout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Vertex;
			layout.registerSpace = 3;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)};
			++m_ObjectAttempts;
			m_DrawLayout = m_Factory.CreateBindingLayout(device, layout);
		}
		if (!m_ResolveLayout)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ResolveConstants))};
			for (uint32_t slot = 0; slot < 11; ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			++m_ObjectAttempts;
			m_ResolveLayout = m_Factory.CreateBindingLayout(device, layout);
		}
		if (!m_DrawLayout || !m_ResolveLayout)
			return Fail("temporal layout creation failed");
		if (std::max(1u, drawCount) > m_BufferCapacity)
		{
			m_DrawBindings = nullptr;
			m_Buffers.Clear();
			m_BufferCapacity = std::bit_ceil(std::max(1u, drawCount));
			m_DrawBuffer = m_Buffers.Create(static_cast<uint64_t>(m_BufferCapacity) * sizeof(TemporalGPURecord), sizeof(TemporalGPURecord), "TemporalPreviousDraws");
			if (!m_DrawBuffer)
				return Fail(m_Buffers.GetFailureReason());
			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_DrawBuffer)};
			++m_ObjectAttempts;
			m_DrawBindings = m_Factory.CreateBindingSet(device, bindings, m_DrawLayout);
			if (!m_DrawBindings)
				return Fail("temporal draw binding creation failed");
		}
		if (!sameSize)
		{
			auto create = [&](nvrhi::Format format, const char* name) {
				nvrhi::TextureDesc desc;
				desc.width = size.width;
				desc.height = size.height;
				desc.format = format;
				desc.isRenderTarget = true;
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				desc.debugName = name;
				return m_Textures.Create(desc);
			};
			const std::array guideFormats{nvrhi::Format::RG16_FLOAT, nvrhi::Format::R32_UINT, nvrhi::Format::RG32_FLOAT, nvrhi::Format::R32_FLOAT};
			for (size_t i = 0; i < guideFormats.size(); ++i)
			{
				m_Guides[i] = create(guideFormats[i], "TemporalMotionGuide");
				if (!m_Guides[i])
					return Fail(m_Textures.GetFailureReason());
			}
			m_Reactive = create(nvrhi::Format::R8_UNORM, "TemporalReactive");
			if (!m_Reactive)
				return Fail(m_Textures.GetFailureReason());
			const std::array historyFormats{nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::R32_UINT, nvrhi::Format::R32_FLOAT};
			for (uint32_t index = 0; index < 2; ++index)
			{
				nvrhi::FramebufferDesc framebuffer;
				for (size_t i = 0; i < historyFormats.size(); ++i)
				{
					m_History[index][i] = create(historyFormats[i], "TemporalHistory");
					if (!m_History[index][i])
						return Fail(m_Textures.GetFailureReason());
					framebuffer.addColorAttachment(m_History[index][i]);
				}
				++m_ObjectAttempts;
				m_ResolveFramebuffers[index] = m_Factory.CreateFramebuffer(device, framebuffer);
				if (!m_ResolveFramebuffers[index])
					return Fail("temporal history framebuffer creation failed");
			}
		}
		if (m_HDR != hdr || m_Depth != depth)
		{
			m_HDR = hdr;
			m_Depth = depth;
			if (!CreateBindings())
				return Fail("temporal input binding creation failed");
		}
		if (!sameSize)
		{
			auto vs = ShaderLibrary::CreateShader(device, "TemporalGuide", "MotionVS", nvrhi::ShaderType::Vertex);
			auto ps = ShaderLibrary::CreateShader(device, "TemporalGuide", "MotionPS", nvrhi::ShaderType::Pixel);
			auto reactivePS = ShaderLibrary::CreateShader(device, "TemporalGuide", "ReactivePS", nvrhi::ShaderType::Pixel);
			auto resolveVS = ShaderLibrary::CreateShader(device, "TemporalResolve", "ResolveVS", nvrhi::ShaderType::Vertex);
			auto resolvePS = ShaderLibrary::CreateShader(device, "TemporalResolve", "ResolvePS", nvrhi::ShaderType::Pixel);
			if (!vs || !ps || !reactivePS || !resolveVS || !resolvePS)
				return Fail("temporal shader creation failed");
			for (size_t i = 0; i < m_GuidePipelines.size(); ++i)
			{
				nvrhi::GraphicsPipelineDesc pipeline;
				pipeline.VS = vs;
				pipeline.PS = ps;
				pipeline.inputLayout = input;
				pipeline.bindingLayouts = {view, material, probe, m_DrawLayout};
				pipeline.renderState.rasterState.setFrontCounterClockwise(i != 1).setCullMode(i == 2 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back);
				pipeline.renderState.depthStencilState.enableDepthTest().disableDepthWrite().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				++m_ObjectAttempts;
				m_GuidePipelines[i] = m_Factory.CreatePipeline(device, pipeline, m_GuideFramebuffer->getFramebufferInfo());
				if (!m_GuidePipelines[i])
					return Fail("temporal guide pipeline creation failed");
				if (i == 2)
				{
					pipeline.PS = reactivePS;
					++m_ObjectAttempts;
					m_ReactivePipeline = m_Factory.CreatePipeline(device, pipeline, m_ReactiveFramebuffer->getFramebufferInfo());
					if (!m_ReactivePipeline)
						return Fail("temporal reactive pipeline creation failed");
				}
			}
			nvrhi::GraphicsPipelineDesc pipeline;
			pipeline.VS = resolveVS;
			pipeline.PS = resolvePS;
			pipeline.bindingLayouts = {m_ResolveLayout};
			pipeline.renderState.rasterState.setCullNone();
			pipeline.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			++m_ObjectAttempts;
			m_ResolvePipeline = m_Factory.CreatePipeline(device, pipeline, m_ResolveFramebuffers[0]->getFramebufferInfo());
			if (!m_ResolvePipeline)
				return Fail("temporal resolve pipeline creation failed");
		}
		m_Ready = true;
		m_Reason = "";
		return true;
	}

	void TemporalAA::UploadAndClear(nvrhi::ICommandList* commands, std::span<const TemporalGPURecord> records)
	{
		PX_CORE_VERIFY(m_Ready && commands && records.size() <= m_BufferCapacity, "Temporal upload requires prepared resources and bounded records");
		if (!records.empty())
			commands->writeBuffer(m_DrawBuffer, records.data(), records.size_bytes());
		auto clear = [&](nvrhi::ITexture* texture) {
			PostEffectTextures::TrackUsage(commands, texture, nvrhi::ResourceStates::CopyDest);
			if (texture->getDesc().format == nvrhi::Format::R32_UINT)
				commands->clearTextureUInt(texture, nvrhi::AllSubresources, 0);
			else
				commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0));
		};
		for (auto* texture : m_Guides)
			clear(texture);
		clear(m_Reactive);
		if (m_ClearHistory)
		{
			for (const auto& history : m_History)
				for (auto* texture : history)
					clear(texture);
		}
	}

	void TemporalAA::Resolve(nvrhi::ICommandList* commands, const glm::mat4& inverseProjection, float historyWeight)
	{
		PX_CORE_VERIFY(m_Ready && commands && m_State.IsStaged() && std::isfinite(historyWeight) && historyWeight >= 0 && historyWeight <= 0.95f,
					   "Temporal resolve requires staged history and a bounded history weight");
		const auto& size = m_HDR->getDesc();
		nvrhi::GraphicsState state;
		state.pipeline = m_ResolvePipeline;
		state.framebuffer = m_ResolveFramebuffers[m_WriteIndex];
		state.bindings = {m_ResolveBindings[m_WriteIndex]};
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(size.width), float(size.height)));
		commands->setGraphicsState(state);
		const auto jitter = m_State.GetJitter();
		const ResolveConstants constants{inverseProjection, {jitter.x, jitter.y, historyWeight, m_State.CanUseHistory() ? 1.0f : 0.0f}, {size.width, size.height, 0, 0}};
		commands->setPushConstants(&constants, sizeof(constants));
		commands->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

	void TemporalAA::Commit()
	{
		m_State.Commit();
		m_ClearHistory = false;
		m_WriteIndex = 1 - m_WriteIndex;
	}

}
