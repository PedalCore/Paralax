#include "pxpch.h"
#include "Paralax/Renderer/ReflectionHistory.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/ShaderLibrary.h"
#include "Paralax/Renderer/SceneRendererTypes.h"

#include <algorithm>
#include <cmath>

namespace Paralax {

	namespace {
		struct HistoryParameters
		{
			glm::mat4 PreviousRaster, PreviousView;
			glm::vec4 Settings;
			glm::uvec4 Dimensions;
		};
		static_assert(sizeof(HistoryParameters) == 160);
	}

	ReflectionHistory::ReflectionHistory(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Budget(budget)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
		, m_Buffers(graphics, budget, &m_Factory)
	{
	}

	ReflectionHistory::~ReflectionHistory()
	{
		Release();
	}

	void ReflectionHistory::DetachInputs()
	{
		m_Bindings = {};
		m_ComposeBindings = {};
		m_ComposeFramebuffer = nullptr;
		m_Inputs.reset();
	}

	void ReflectionHistory::Release()
	{
		m_Failure.reset();
		ReleaseResources();
	}

	void ReflectionHistory::ReleaseResources()
	{
		DetachInputs();
		m_Framebuffers = {};
		m_Pipeline = nullptr;
		m_ComposePipeline = nullptr;
		m_Layout = nullptr;
		m_ComposeLayout = nullptr;
		m_History = {};
		m_Parameters = nullptr;
		m_Textures.Clear();
		m_Buffers.Clear();
		m_Ready = false;
		m_Recorded = false;
		m_Size = {};
		m_Clear = true;
		m_WriteIndex = 0;
		m_State.Reset("reflection resources released");
	}

	bool ReflectionHistory::Fail(const char* reason)
	{
		m_Reason = reason;
		// Preserve the retry identity while retiring partial allocations; explicit Release clears both.
		ReleaseResources();
		m_State.Abort(m_Reason.c_str());
		return false;
	}

	bool ReflectionHistory::Prepare(const Inputs& inputs)
	{
		if (!inputs.Generation || !inputs.HDR || !inputs.ViewConstants || std::any_of(inputs.Textures.begin(), inputs.Textures.end(), [](const auto& input) { return !input; }))
			return Fail("reflection history requires complete inputs");
		const auto& output = inputs.HDR->getDesc();
		const glm::uvec2 size(output.width, output.height);
		auto colorFormat = [](nvrhi::Format format) { return format == nvrhi::Format::RGBA16_FLOAT || format == nvrhi::Format::RGBA32_FLOAT; };
		if (!size.x || !size.y || !output.isRenderTarget || !colorFormat(output.format) || output.sampleCount != 1 || output.dimension != nvrhi::TextureDimension::Texture2D ||
			!inputs.ViewConstants->getDesc().isConstantBuffer || !inputs.ViewConstants->getDesc().isVolatile || inputs.ViewConstants->getDesc().byteSize < sizeof(GPU::ViewConstants))
			return Fail("invalid reflection history output or view constants");
		for (size_t i = 0; i < inputs.Textures.size(); ++i)
		{
			const auto& desc = inputs.Textures[i]->getDesc();
			const bool format = i == 1 ? desc.format == nvrhi::Format::D32 || desc.format == nvrhi::Format::R32_FLOAT : colorFormat(desc.format);
			if (!format || !desc.isShaderResource || desc.width != size.x || desc.height != size.y || desc.sampleCount != 1 || desc.dimension != nvrhi::TextureDimension::Texture2D)
				return Fail("invalid reflection history sampled input");
		}
		if (m_Ready && m_Inputs && *m_Inputs == inputs)
			return true;
		std::array<uintptr_t, 9> inputIDs{};
		for (size_t i = 0; i < inputs.Textures.size(); ++i)
			inputIDs[i] = reinterpret_cast<uintptr_t>(inputs.Textures[i].Get());
		inputIDs[7] = reinterpret_cast<uintptr_t>(inputs.HDR.Get());
		inputIDs[8] = reinterpret_cast<uintptr_t>(inputs.ViewConstants.Get());
		const uint64_t otherBytes = m_Budget.GetUsedBytes() - GetOwnedBytes();
		if (m_Failure && m_Failure->Size == size && m_Failure->Generation == inputs.Generation && m_Failure->InputIDs == inputIDs && m_Failure->OtherBytes == otherBytes)
			return false;
		if (m_Size != size || m_OutputFormat != output.format)
			Release();
		m_Failure = Failure{inputIDs, inputs.Generation, size, m_Budget.GetUsedBytes() - GetOwnedBytes()};
		auto* device = m_Graphics.GetDevice();
		if (!m_Ready)
		{
			const std::array formats{nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::R32_FLOAT, nvrhi::Format::RGBA32_FLOAT};
			for (auto format : formats)
			{
				const auto support = m_Factory.QueryFormatSupport(device, format);
				const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderLoad;
				if ((support & required) != required)
					return Fail("reflection history format unsupported");
			}
			for (uint32_t index = 0; index < 2; ++index)
			{
				nvrhi::FramebufferDesc framebuffer;
				for (uint32_t channel = 0; channel < 4; ++channel)
				{
					nvrhi::TextureDesc desc;
					desc.width = size.x;
					desc.height = size.y;
					desc.format = formats[channel];
					desc.isRenderTarget = true;
					desc.initialState = nvrhi::ResourceStates::ShaderResource;
					desc.keepInitialState = true;
					desc.debugName = "ReflectionHistory";
					m_History[index][channel] = m_Textures.Create(desc);
					if (!m_History[index][channel])
						return Fail(m_Textures.GetFailureReason());
					framebuffer.addColorAttachment(m_History[index][channel]);
				}
				++m_ObjectAttempts;
				m_Framebuffers[index] = m_Factory.CreateFramebuffer(device, framebuffer);
				if (!m_Framebuffers[index])
					return Fail("reflection history framebuffer failed");
			}
			m_Parameters = m_Buffers.Create(sizeof(HistoryParameters), sizeof(HistoryParameters), "ReflectionHistoryParameters");
			if (!m_Parameters)
				return Fail(m_Buffers.GetFailureReason());
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::AllGraphics;
			layout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)};
			for (uint32_t slot = 1; slot <= 9; ++slot)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			++m_ObjectAttempts;
			m_Layout = m_Factory.CreateBindingLayout(device, layout);
			layout.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(10), nvrhi::BindingLayoutItem::Texture_SRV(11), nvrhi::BindingLayoutItem::Texture_SRV(12), nvrhi::BindingLayoutItem::PushConstants(15, 16)};
			++m_ObjectAttempts;
			m_ComposeLayout = m_Factory.CreateBindingLayout(device, layout);
			if (!m_Layout || !m_ComposeLayout)
				return Fail("reflection history layout failed");
			m_Size = size;
			m_OutputFormat = output.format;
		}
		DetachInputs();
		++m_ObjectAttempts;
		m_ComposeFramebuffer = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(inputs.HDR));
		if (!m_ComposeFramebuffer)
			return Fail("reflection history composition framebuffer failed");
		for (uint32_t index = 0; index < 2; ++index)
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, inputs.ViewConstants), nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_Parameters)};
			for (uint32_t i = 0; i < 5; ++i)
				set.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(i + 1, inputs.Textures[i]));
			for (uint32_t i = 0; i < 4; ++i)
				set.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(i + 6, m_History[1 - index][i]));
			++m_ObjectAttempts;
			m_Bindings[index] = m_Factory.CreateBindingSet(device, set, m_Layout);
			set.bindings = {nvrhi::BindingSetItem::Texture_SRV(10, inputs.Textures[5]), nvrhi::BindingSetItem::Texture_SRV(11, inputs.Textures[6]),
							nvrhi::BindingSetItem::Texture_SRV(12, m_History[index][0]), nvrhi::BindingSetItem::PushConstants(15, 16)};
			++m_ObjectAttempts;
			m_ComposeBindings[index] = m_Factory.CreateBindingSet(device, set, m_ComposeLayout);
			if (!m_Bindings[index] || !m_ComposeBindings[index])
				return Fail("reflection history bindings failed");
		}
		if (!m_Ready)
		{
			for (bool compose : {false, true})
			{
				nvrhi::GraphicsPipelineDesc desc;
				desc.VS = ShaderLibrary::CreateShader(device, "ReflectionHistory", "HistoryVS", nvrhi::ShaderType::Vertex);
				desc.PS = ShaderLibrary::CreateShader(device, "ReflectionHistory", compose ? "ComposePS" : "HistoryPS", nvrhi::ShaderType::Pixel);
				if (!desc.VS || !desc.PS)
					return Fail("reflection history shader unavailable");
				desc.bindingLayouts = {compose ? m_ComposeLayout : m_Layout};
				desc.renderState.depthStencilState.depthTestEnable = false;
				desc.renderState.depthStencilState.depthWriteEnable = false;
				desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
				++m_ObjectAttempts;
				auto pipeline = m_Factory.CreatePipeline(device, desc, (compose ? m_ComposeFramebuffer : m_Framebuffers[0])->getFramebufferInfo());
				if (!pipeline)
					return Fail("reflection history pipeline failed");
				(compose ? m_ComposePipeline : m_Pipeline) = pipeline;
			}
		}
		m_Inputs = inputs;
		m_Failure.reset();
		m_Reason.clear();
		m_Ready = true;
		return true;
	}

	void ReflectionHistory::ResolveAndCompose(nvrhi::ICommandList* commands, const glm::mat4& rasterViewProjection, float weight, float thickness, bool reflectionOnly)
	{
		PX_CORE_VERIFY(commands && m_Ready && m_Inputs && m_State.IsStaged(), "Reflection resolve requires prepared resources and staged metadata");
		if (m_Clear)
		{
			for (const auto& history : m_History)
				for (auto* texture : history)
				{
					PostEffectTextures::TrackUsage(commands, texture, nvrhi::ResourceStates::CopyDest);
					commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0));
				}
			m_Clear = false;
		}
		const HistoryParameters parameters{m_PreviousRaster, m_State.GetPreviousFrame().View, {std::isfinite(weight) ? std::clamp(weight, 0.0f, 0.95f) : 0.85f, std::isfinite(thickness) ? std::clamp(thickness, 0.001f, 1.0f) : 0.1f, m_State.CanUseHistory() ? 1.0f : 0.0f, 0}, {m_Size.x, m_Size.y, 0, 0}};
		commands->writeBuffer(m_Parameters, &parameters, sizeof(parameters));
		for (bool compose : {false, true})
		{
			nvrhi::GraphicsState state;
			state.pipeline = compose ? m_ComposePipeline : m_Pipeline;
			state.framebuffer = compose ? m_ComposeFramebuffer : m_Framebuffers[m_WriteIndex];
			state.bindings = {compose ? m_ComposeBindings[m_WriteIndex] : m_Bindings[m_WriteIndex]};
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(m_Size.x), float(m_Size.y)));
			commands->setGraphicsState(state);
			if (compose)
			{
				const glm::uvec4 constants(reflectionOnly ? 1u : 0u, 0, 0, 0);
				commands->setPushConstants(&constants, sizeof(constants));
			}
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		}
		m_PendingRaster = rasterViewProjection;
		m_Recorded = true;
	}

	void ReflectionHistory::Commit()
	{
		PX_CORE_VERIFY(m_Ready && m_Recorded, "Reflection history commit requires a recorded resolve");
		m_State.Commit();
		m_Recorded = false;
		m_PreviousRaster = m_PendingRaster;
		m_WriteIndex = 1 - m_WriteIndex;
	}

}
