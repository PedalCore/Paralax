#include "pxpch.h"
#include "Paralax/Renderer/ScreenSpaceReflections.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Renderer/ShaderLibrary.h"
#include "Paralax/Renderer/RenderProfiler.h"

#include <cmath>
#include <cstring>

namespace Paralax {

	ScreenSpaceReflections::ScreenSpaceReflections(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Budget(budget)
		, m_Factory(factory ? *factory : m_DefaultFactory)
		, m_Textures(graphics, budget, &m_Factory)
		, m_Hierarchy(graphics, budget, &m_Factory)
	{
	}

	ScreenSpaceReflections::~ScreenSpaceReflections()
	{
		Release();
	}

	void ScreenSpaceReflections::Release()
	{
		m_Failed.reset();
		m_Inputs.reset();
		m_HistoryInputToken.reset();
		m_Ready = false;
		m_InspectionReady = m_PendingInspectionFrame = false;
		m_TraceBindings = nullptr;
		m_ComposeBindings = nullptr;
		m_OpaqueFramebuffer = nullptr;
		m_GuideFramebuffer = nullptr;
		m_TraceFramebuffer = nullptr;
		m_ComposeFramebuffer = nullptr;
		m_OpaquePipelines = {};
		m_GuidePipelines = {};
		m_TracePipeline = nullptr;
		m_ComposePipeline = nullptr;
		m_Layout = nullptr;
		m_Linear = nullptr;
		m_Hierarchy.Release();
		m_HDR = nullptr;
		m_Depth = nullptr;
		m_Guides = {};
		m_Environment = m_Source = m_Color = m_Hit = nullptr;
		m_Textures.Clear();
	}

	bool ScreenSpaceReflections::Fail(const char* reason)
	{
		auto failed = std::move(m_Failed);
		m_Reason = reason;
		Release();
		m_Failed = std::move(failed);
		return false;
	}

	bool ScreenSpaceReflections::Prepare(bool enabled, nvrhi::ITexture* hdr, nvrhi::ITexture* depth, const std::array<nvrhi::ITexture*, 3>& deferredGuides,
										 nvrhi::IBuffer* viewConstants, nvrhi::ITexture* brdf, nvrhi::IInputLayout* input, nvrhi::IBindingLayout* view,
										 nvrhi::IBindingLayout* material, nvrhi::IBindingLayout* probe, nvrhi::IBindingLayout* deferredLayout)
	{
		if (!enabled)
		{
			Release();
			m_Failed.reset();
			m_Reason.clear();
			return false;
		}
		const bool forward = !deferredLayout;
		const InputKey inputs{hdr, depth, {deferredGuides[0], deferredGuides[1], deferredGuides[2]}, viewConstants, brdf, input, view, material, probe, deferredLayout};
		if (m_Ready && m_Inputs && *m_Inputs == inputs)
			return true;
		if (m_Failed && m_Failed->Inputs == inputs && m_Failed->OtherBytes == m_Budget.GetUsedBytes())
			return false;
		Release();
		m_Failed = FailedRequest{inputs, m_Budget.GetUsedBytes()};
		if (!hdr || !depth || !viewConstants || !brdf || !input || !view || !material || !probe)
			return Fail("missing reflection inputs");
		const auto& size = hdr->getDesc();
		if (size.width != depth->getDesc().width || size.height != depth->getDesc().height || size.sampleCount != 1 || size.format != nvrhi::Format::RGBA16_FLOAT)
			return Fail("unsupported reflection input shape or format");
		const auto& limits = m_Graphics.GetInfo().Limits;
		if (limits.MaxSampledTexturesPerStage < SampledTextureCount || limits.MaxSampledTexturesPerSet < SampledTextureCount)
			return Fail("reflection sampled texture binding limit");
		if (!forward && !m_Factory.SupportsIndependentBlend(m_Graphics))
			return Fail("deferred reflections require independent attachment blending");
		m_Forward = forward;
		m_HDR = hdr;
		m_Depth = depth;
		auto* device = m_Graphics.GetDevice();
		auto texture = [&](nvrhi::Format format, const char* name) {
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
		for (size_t i = 0; i < m_Guides.size(); ++i)
		{
			m_Guides[i] = forward ? texture(nvrhi::Format::RGBA16_FLOAT, "ReflectionMaterialGuide") : deferredGuides[i];
			if (!m_Guides[i])
				return Fail(forward ? m_Textures.GetFailureReason() : "missing deferred reflection guide");
		}
		m_Environment = texture(nvrhi::Format::RGBA32_FLOAT, "ReflectionExactEnvironmentSpecular");
		if (!m_Environment)
			return Fail(m_Textures.GetFailureReason());
		m_Source = texture(nvrhi::Format::RGBA16_FLOAT, "ReflectionOpaqueRadiance");
		if (!m_Source)
			return Fail(m_Textures.GetFailureReason());
		m_Color = texture(nvrhi::Format::RGBA16_FLOAT, "ReflectionCandidate");
		if (!m_Color)
			return Fail(m_Textures.GetFailureReason());
		m_Hit = texture(nvrhi::Format::RGBA32_FLOAT, "ReflectionHitWorld");
		if (!m_Hit)
			return Fail(m_Textures.GetFailureReason());
		if (!m_Hierarchy.Prepare(depth, m_Guides[2]))
			return Fail(m_Hierarchy.GetFallbackReason());
		auto framebuffer = [&](const nvrhi::FramebufferDesc& desc) {
			++m_ObjectAttempts;
			return m_Factory.CreateFramebuffer(device, desc);
		};
		nvrhi::FramebufferDesc opaque;
		opaque.addColorAttachment(hdr).addColorAttachment(m_Environment);
		if (forward)
			opaque.setDepthAttachment(depth);
		m_OpaqueFramebuffer = framebuffer(opaque);
		m_TraceFramebuffer = framebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Color).addColorAttachment(m_Hit));
		m_ComposeFramebuffer = framebuffer(nvrhi::FramebufferDesc().addColorAttachment(hdr));
		if (!m_OpaqueFramebuffer || !m_TraceFramebuffer || !m_ComposeFramebuffer)
			return Fail("reflection framebuffer creation failed");
		auto pipeline = [&](const char* module, const char* vertex, const char* fragment, nvrhi::IFramebuffer* target,
							const nvrhi::BindingLayoutVector& layouts, int raster, bool depthWrite, bool additive) {
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = ShaderLibrary::CreateShader(device, module, vertex, nvrhi::ShaderType::Vertex);
			desc.PS = ShaderLibrary::CreateShader(device, module, fragment, nvrhi::ShaderType::Pixel);
			if (!desc.VS || !desc.PS)
				return nvrhi::GraphicsPipelineHandle{};
			desc.bindingLayouts = layouts;
			if (raster < 0)
			{
				desc.renderState.rasterState.setCullNone();
				desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
			}
			else
			{
				desc.inputLayout = input;
				desc.renderState.rasterState.setFrontCounterClockwise(raster != 1).setCullMode(raster == 2 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back);
				desc.renderState.depthStencilState.enableDepthTest().setDepthFunc(depthWrite ? nvrhi::ComparisonFunc::GreaterOrEqual : nvrhi::ComparisonFunc::Equal);
				desc.renderState.depthStencilState.depthWriteEnable = depthWrite;
			}
			if (additive)
				desc.renderState.blendState.targets[0].enableBlend().setSrcBlend(nvrhi::BlendFactor::One).setDestBlend(nvrhi::BlendFactor::One);
			++m_ObjectAttempts;
			return m_Factory.CreatePipeline(device, desc, target->getFramebufferInfo());
		};
		if (forward)
		{
			m_GuideFramebuffer = framebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Guides[0]).addColorAttachment(m_Guides[1]).addColorAttachment(m_Guides[2]).setDepthAttachment(depth));
			if (!m_GuideFramebuffer)
				return Fail("reflection guide framebuffer creation failed");
			for (int i = 0; i < 3; ++i)
			{
				m_OpaquePipelines[i] = pipeline("ForwardPBR", "MainVS", "ReflectionPS", m_OpaqueFramebuffer, {view, material, probe}, i, true, false);
				m_GuidePipelines[i] = pipeline("SurfaceGuide", "GuideVS", "ReflectionGuidePS", m_GuideFramebuffer, {view, material, probe}, i, false, false);
				if (!m_OpaquePipelines[i] || !m_GuidePipelines[i])
					return Fail("reflection material pipeline creation failed");
			}
		}
		else
		{
			m_OpaquePipelines[0] = pipeline("DeferredLighting", "ResolveVS", "ReflectionPS", m_OpaqueFramebuffer, {view, deferredLayout, probe}, -1, false, true);
			if (!m_OpaquePipelines[0])
				return Fail("reflection deferred pipeline creation failed");
		}
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::AllGraphics;
		layout.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::PushConstants(15, sizeof(ReflectionConstants)), nvrhi::BindingLayoutItem::Sampler(0)};
		for (uint32_t i = 0; i < SampledTextureCount; ++i)
			layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(i));
		++m_ObjectAttempts;
		m_Layout = m_Factory.CreateBindingLayout(device, layout);
		if (!m_Layout)
			return Fail("reflection layout creation failed");
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		++m_ObjectAttempts;
		m_Linear = m_Factory.CreateSampler(device, sampler);
		if (!m_Linear)
			return Fail("reflection sampler creation failed");
		for (bool compose : {false, true})
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, viewConstants), nvrhi::BindingSetItem::PushConstants(15, sizeof(ReflectionConstants)), nvrhi::BindingSetItem::Sampler(0, m_Linear)};
			for (uint32_t i = 0; i < DepthHierarchy::MaxLevels; ++i)
				set.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(i, m_Hierarchy.GetLevel(std::min(i, m_Hierarchy.GetLevelCount() - 1))));
			std::array<nvrhi::ITexture*, 8> textures{depth, m_Guides[1], m_Guides[2], m_Source, m_Guides[0], m_Environment, brdf, compose ? m_Color : m_Source};
			for (uint32_t i = 0; i < textures.size(); ++i)
				set.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(16 + i, textures[i]));
			++m_ObjectAttempts;
			auto bindings = m_Factory.CreateBindingSet(device, set, m_Layout);
			if (!bindings)
				return Fail("reflection binding creation failed");
			(compose ? m_ComposeBindings : m_TraceBindings) = bindings;
		}
		m_TracePipeline = pipeline("ScreenSpaceReflections", "ReflectionVS", "TracePS", m_TraceFramebuffer, {m_Layout}, -1, false, false);
		m_ComposePipeline = pipeline("ScreenSpaceReflections", "ReflectionVS", "ComposePS", m_ComposeFramebuffer, {m_Layout}, -1, false, false);
		if (!m_TracePipeline || !m_ComposePipeline)
			return Fail("reflection trace/compose pipeline creation failed");
		m_Failed.reset();
		m_Reason.clear();
		m_Inputs = inputs;
		m_HistoryInputToken = CreateRef<uint8_t>(0);
		++m_Generation;
		m_Ready = true;
		return true;
	}

	void ScreenSpaceReflections::ClearGuides(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(m_Ready && commands, "Reflection guide clear requires prepared resources");
		PostEffectTextures::TrackUsage(commands, m_Environment, nvrhi::ResourceStates::CopyDest);
		commands->clearTextureFloat(m_Environment, nvrhi::AllSubresources, nvrhi::Color(0));
		if (m_Forward)
			for (const auto& guide : m_Guides)
			{
				PostEffectTextures::TrackUsage(commands, guide, nvrhi::ResourceStates::CopyDest);
				commands->clearTextureFloat(guide, nvrhi::AllSubresources, nvrhi::Color(0));
			}
	}

	ReflectionHistory::Inputs ScreenSpaceReflections::GetHistoryInputs() const
	{
		return {{{m_Color, m_Depth, m_Guides[1], m_Guides[2], m_Hit, m_Source, m_Environment}}, m_HDR, m_Inputs ? m_Inputs->ViewConstants : nullptr, m_HistoryInputToken};
	}

	void ScreenSpaceReflections::CommitInspectionFrame(const GPU::ViewConstants& view, uint64_t frame, uint32_t path)
	{
		if (!m_Ready || !m_PendingInspectionFrame)
			return;
		m_RecordedView = view;
		m_RecordedFrame = frame;
		m_RecordedPath = path;
		m_PendingInspectionFrame = false;
		m_InspectionReady = true;
	}

	std::optional<ReflectionPixelInspection> ScreenSpaceReflections::InspectPixel(glm::uvec2 pixel, uint64_t expectedFrame)
	{
		if (!m_Ready || !m_InspectionReady || expectedFrame != m_RecordedFrame ||
			pixel.x >= m_RecordedConstants.Dimensions.x || pixel.y >= m_RecordedConstants.Dimensions.y)
			return std::nullopt;
		auto* device = m_Graphics.GetDevice();
		// Declare the owner first: bindings and command lists must release their references before it retires memory.
		PostEffectTextures textures(m_Graphics, m_Budget, &m_Factory);
		nvrhi::TextureDesc desc;
		desc.width = desc.height = 1;
		desc.format = nvrhi::Format::RGBA32_FLOAT;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "ReflectionPixelInspection";
		const auto createTarget = [&] {
			const auto before = textures.GetBackingAttempts();
			auto* target = textures.Create(desc);
			// Include failed committed attempts; a partial diagnostic still changes allocation order.
			m_InspectionBackingAttempts += textures.GetBackingAttempts() - before;
			return target;
		};
		auto* color = createTarget();
		if (!color)
			return std::nullopt;
		auto* details = createTarget();
		if (!details)
			return std::nullopt;
		auto framebuffer = m_Factory.CreateFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(color).addColorAttachment(details));
		if (!framebuffer)
			return std::nullopt;
		nvrhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.VS = ShaderLibrary::CreateShader(device, "ScreenSpaceReflections", "ReflectionVS", nvrhi::ShaderType::Vertex);
		pipelineDesc.PS = ShaderLibrary::CreateShader(device, "ScreenSpaceReflections", "InspectionPS", nvrhi::ShaderType::Pixel);
		pipelineDesc.bindingLayouts = {m_Layout};
		pipelineDesc.renderState.rasterState.setCullNone();
		pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
		if (!pipelineDesc.VS || !pipelineDesc.PS)
			return std::nullopt;
		auto pipeline = m_Factory.CreatePipeline(device, pipelineDesc, framebuffer->getFramebufferInfo());
		if (!pipeline)
			return std::nullopt;
		desc.isRenderTarget = false;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		std::array<nvrhi::StagingTextureHandle, 6> staging;
		for (auto& target : staging)
		{
			target = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
			if (!target)
				return std::nullopt;
		}
		auto commands = device->createCommandList();
		if (!commands)
			return std::nullopt;
		commands->open();
		commands->writeBuffer(m_Inputs->ViewConstants, &m_RecordedView, sizeof(m_RecordedView));
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = framebuffer;
		state.bindings = {m_TraceBindings};
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(1.0f, 1.0f));
		auto constants = m_RecordedConstants;
		for (uint32_t part = 0; part < 3; ++part)
		{
			// The preceding copies force NVRHI to restore render-target barriers for the reused tiny outputs.
			commands->setGraphicsState(state);
			constants.Probe = {pixel.x, pixel.y, part, 0};
			commands->setPushConstants(&constants, sizeof(constants));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
			commands->copyTexture(staging[part * 2], {}, color, {});
			commands->copyTexture(staging[part * 2 + 1], {}, details, {});
		}
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		std::array<glm::vec4, 6> values;
		for (size_t i = 0; i < staging.size(); ++i)
		{
			size_t pitch = 0;
			const void* mapped = device->mapStagingTexture(staging[i], {}, nvrhi::CpuAccessMode::Read, &pitch);
			if (!mapped)
				return std::nullopt;
			const bool valid = pitch >= sizeof(glm::vec4);
			if (valid)
				std::memcpy(&values[i], mapped, sizeof(glm::vec4));
			device->unmapStagingTexture(staging[i]);
			if (!valid)
				return std::nullopt;
		}
		ReflectionPixelInspection result;
		result.Frame = m_RecordedFrame;
		result.Generation = m_Generation;
		result.Path = m_RecordedPath;
		result.Size = glm::uvec2(m_RecordedConstants.Dimensions);
		result.Pixel = pixel;
		result.CandidateTexel = glm::vec2(values[0]);
		result.Distance = values[0].z;
		result.Confidence = values[0].w;
		result.Residual = values[1].x;
		result.Facing = values[1].y;
		result.Status = static_cast<uint32_t>(values[1].z);
		result.FilterWeight = values[1].w;
		result.PlanePixel = glm::vec2(values[2]);
		result.PlaneDistance = values[2].z;
		result.PlaneFlags = static_cast<uint32_t>(values[2].w);
		result.Termination = static_cast<uint32_t>(values[3].x);
		result.RefinementCells = static_cast<uint32_t>(values[3].y);
		result.TerminationCell = glm::ivec2(values[3].z, values[3].w);
		result.CandidateNormal = glm::vec3(values[4]);
		result.CandidateNormalDot = values[4].w;
		result.TerminationNormal = glm::vec3(values[5]);
		result.CandidatePlaneOffset = values[5].w;
		return result;
	}

	void ScreenSpaceReflections::Render(nvrhi::ICommandList* commands, const ReflectionSettings& settings, bool reflectionOnly, ReflectionHistory* history, const glm::mat4& rasterViewProjection, float historyWeight, RenderProfiler* profiler)
	{
		PX_CORE_VERIFY(m_Ready && commands, "Reflections require prepared resources and an open command list");
		if (profiler)
			profiler->BeginSection(commands, RenderTimingSection::DepthHierarchy);
		m_Hierarchy.Render(commands);
		if (profiler)
			profiler->EndSection(commands, RenderTimingSection::DepthHierarchy);
		PostEffectTextures::TrackUsage(commands, m_HDR, nvrhi::ResourceStates::CopySource);
		PostEffectTextures::TrackUsage(commands, m_Source, nvrhi::ResourceStates::CopyDest);
		commands->copyTexture(m_Source, {}, m_HDR, {});
		auto finite = [](float value, float fallback, float low, float high) { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; };
		const auto& size = m_HDR->getDesc();
		const ReflectionConstants constants{{finite(settings.MaxDistance, 30, 0.1f, 200), finite(settings.Thickness, 0.1f, 0.001f, 1),
											 finite(settings.MaxRoughness, 0.8f, 0.05f, 1), float(std::clamp(settings.MaxSteps, 16u, 256u))},
											{size.width, size.height, m_Hierarchy.GetLevelCount(), reflectionOnly ? 1u : 0u},
											{0, 0, 0, 0}};
		m_RecordedConstants = constants;
		m_InspectionReady = false;
		m_PendingInspectionFrame = true;
		for (bool compose : {false, true})
		{
			if (compose && history)
			{
				history->ResolveAndCompose(commands, rasterViewProjection, historyWeight, settings.Thickness, reflectionOnly);
				break;
			}
			nvrhi::GraphicsState state;
			state.pipeline = compose ? m_ComposePipeline : m_TracePipeline;
			state.framebuffer = compose ? m_ComposeFramebuffer : m_TraceFramebuffer;
			state.bindings = {compose ? m_ComposeBindings : m_TraceBindings};
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(size.width), float(size.height)));
			commands->setGraphicsState(state);
			commands->setPushConstants(&constants, sizeof(constants));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		}
	}

}
