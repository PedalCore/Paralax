#include "pxpch.h"
#include "Paralax/Renderer/TextRenderer.h"

#include "Paralax/Renderer/ShaderLibrary.h"
#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Paralax {

	namespace {

		struct TextConstants
		{
			glm::mat4 Transform;
		};

		uint64_t HashCombine(uint64_t seed, uint64_t value)
		{
			return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
		}

		// Depth framebuffers pair a swapchain image (a few, rotating) with the scene depth; one unused for this many
		// frames belongs to a resized or replaced target and is dropped.
		constexpr uint64_t s_EvictAfterFrames = 8;

		glm::vec4 SafeColor(const glm::vec4& color)
		{
			glm::vec4 safe = color;
			for (int i = 0; i < 4; i++)
				safe[i] = std::isfinite(safe[i]) ? std::clamp(safe[i], 0.0f, i == 3 ? 1.0f : 1000.0f) : 0.0f;
			return safe;
		}

	}

	TextRenderer::TextRenderer(nvrhi::IDevice* device)
		: m_Device(device)
	{
		PX_CORE_VERIFY(device, "TextRenderer requires a device");
	}

	TextRenderer::~TextRenderer() = default;

	bool TextRenderer::Initialize(nvrhi::ICommandList* commandList)
	{
		PX_CORE_VERIFY(commandList, "TextRenderer::Initialize needs an open command list");
		m_Atlas = FontAtlas::CreateDefault();
		if (!m_Atlas)
			return false;

		nvrhi::TextureDesc atlas;
		atlas.width = FontAtlas::Width;
		atlas.height = FontAtlas::Height;
		atlas.format = nvrhi::Format::R8_UNORM;
		atlas.debugName = "TextAtlas";
		atlas.initialState = nvrhi::ResourceStates::ShaderResource;
		atlas.keepInitialState = true;
		m_AtlasTexture = m_Device->createTexture(atlas);
		commandList->writeTexture(m_AtlasTexture, 0, 0, m_Atlas->GetPixels().data(), FontAtlas::Width);

		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_Sampler = m_Device->createSampler(sampler);

		m_VertexShader = ShaderLibrary::CreateShader(m_Device, "Text", "TextVS", nvrhi::ShaderType::Vertex);
		m_PixelShader = ShaderLibrary::CreateShader(m_Device, "Text", "TextPS", nvrhi::ShaderType::Pixel);
		if (!m_VertexShader || !m_PixelShader)
			return false;

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(Vertex, Position)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(Vertex, UV)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(Vertex, Color)).setElementStride(sizeof(Vertex)),
		};
		m_InputLayout = m_Device->createInputLayout(attributes, 3, m_VertexShader);

		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::All;
		layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(TextConstants)), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0)};
		m_BindingLayout = m_Device->createBindingLayout(layout);
		nvrhi::BindingSetDesc set;
		set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(TextConstants)), nvrhi::BindingSetItem::Texture_SRV(0, m_AtlasTexture), nvrhi::BindingSetItem::Sampler(0, m_Sampler)};
		m_BindingSet = m_Device->createBindingSet(set, m_BindingLayout);
		return m_InputLayout && m_BindingLayout && m_BindingSet;
	}

	nvrhi::IGraphicsPipeline* TextRenderer::GetPipeline(nvrhi::IFramebuffer* framebuffer, bool depthTest)
	{
		const nvrhi::FramebufferInfo& info = framebuffer->getFramebufferInfo();
		uint64_t key = HashCombine(static_cast<uint64_t>(info.depthFormat), (static_cast<uint64_t>(info.sampleCount) << 8) | info.sampleQuality);
		for (nvrhi::Format format : info.colorFormats)
			key = HashCombine(key, static_cast<uint64_t>(format));
		key = HashCombine(key, depthTest ? 1 : 0);
		if (auto it = m_Pipelines.find(key); it != m_Pipelines.end())
			return it->second;

		nvrhi::GraphicsPipelineDesc desc;
		desc.VS = m_VertexShader;
		desc.PS = m_PixelShader;
		desc.inputLayout = m_InputLayout;
		desc.bindingLayouts = {m_BindingLayout};
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.renderState.rasterState.setCullNone();
		desc.renderState.depthStencilState.disableDepthWrite();
		if (depthTest)
			desc.renderState.depthStencilState.enableDepthTest().setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual); // Reversed Z.
		else
			desc.renderState.depthStencilState.disableDepthTest();
		desc.renderState.blendState.targets[0]
			.setBlendEnable(true)
			.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
			.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
			.setSrcBlendAlpha(nvrhi::BlendFactor::One)
			.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, info);
		m_Pipelines[key] = pipeline;
		return pipeline;
	}

	nvrhi::IFramebuffer* TextRenderer::GetDepthFramebuffer(nvrhi::IFramebuffer* output, nvrhi::ITexture* depth)
	{
		const nvrhi::FramebufferDesc& outputDesc = output->getDesc();
		if (outputDesc.colorAttachments.empty() || !outputDesc.colorAttachments[0].texture)
			return output;
		nvrhi::ITexture* color = outputDesc.colorAttachments[0].texture;
		const nvrhi::TextureDesc& colorDesc = color->getDesc();
		const nvrhi::TextureDesc& depthDesc = depth->getDesc();
		if (colorDesc.width != depthDesc.width || colorDesc.height != depthDesc.height)
			return output; // No matching depth (not rendered this frame): drawn on top.

		for (DepthFramebuffer& entry : m_DepthFramebuffers)
		{
			if (entry.Color == color && entry.Depth == depth)
			{
				entry.LastUsedFrame = m_Frame;
				return entry.Framebuffer;
			}
		}
		DepthFramebuffer entry;
		entry.Color = color;
		entry.Depth = depth;
		entry.LastUsedFrame = m_Frame;
		entry.Framebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(outputDesc.colorAttachments[0]).setDepthAttachment(depth));
		m_DepthFramebuffers.push_back(entry);
		return entry.Framebuffer;
	}

	void TextRenderer::InvalidateDepthFramebuffers(nvrhi::ITexture* depth)
	{
		std::erase_if(m_DepthFramebuffers, [&](const DepthFramebuffer& entry) { return entry.Depth == depth; });
	}

	uint32_t TextRenderer::Render(nvrhi::ICommandList* commandList, Scene& scene, const glm::mat4& viewProjection, nvrhi::IFramebuffer* output, nvrhi::ITexture* depth)
	{
		PX_CORE_VERIFY(m_Atlas && commandList && output, "TextRenderer::Render needs Initialize, an open command list and an output");
		m_Frame++;
		// Every frame, not only frames with world-space text, so a resized target's framebuffers are let go promptly.
		std::erase_if(m_DepthFramebuffers, [&](const DepthFramebuffer& entry) { return m_Frame - entry.LastUsedFrame > s_EvictAfterFrames; });
		const nvrhi::FramebufferInfoEx& info = output->getFramebufferInfo();
		if (info.width == 0 || info.height == 0)
			return 0;
		const float width = static_cast<float>(info.width);
		const float height = static_cast<float>(info.height);
		const float screenScale = height / ReferenceViewportHeight;

		// World-space text first (depth-tested), then screen-space text over everything; hierarchy order within each.
		std::vector<Vertex> world;
		std::vector<Vertex> screen;
		uint32_t glyphs = 0;
		auto appendQuad = [](std::vector<Vertex>& vertices, const glm::vec3 corners[4], const GlyphQuad& quad, const glm::vec4& color) {
			const glm::vec2 uv[4] = {quad.UVMin, glm::vec2(quad.UVMax.x, quad.UVMin.y), quad.UVMax, glm::vec2(quad.UVMin.x, quad.UVMax.y)};
			const int order[6] = {0, 1, 2, 0, 2, 3};
			for (int index : order)
				vertices.push_back(Vertex{corners[index], uv[index], color});
		};
		scene.ForEachEntity([&](Entity entity) {
			const auto* text = entity.TryGetComponent<TextComponent>();
			if (!text || text->Text.empty() || glyphs >= MaxGlyphsPerFrame)
				return;
			const TextLayout layout = m_Atlas->Layout(text->Text, text->Alignment);
			if (layout.Quads.empty() || glyphs + layout.Quads.size() > MaxGlyphsPerFrame)
				return;
			glyphs += static_cast<uint32_t>(layout.Quads.size());
			const glm::vec4 color = SafeColor(text->Color);
			const float size = std::isfinite(text->Size) ? std::max(text->Size, 0.0f) : 0.0f;
			// Layout puts the widest line at x 0; the anchor moves with the alignment.
			const float anchorShift = text->Alignment == TextAlignment::Center ? -layout.Size.x * 0.5f : text->Alignment == TextAlignment::Right ? -layout.Size.x
																																				 : 0.0f;
			const glm::mat4 worldMatrix = scene.GetWorldTransform(entity);

			if (text->ScreenSpace)
			{
				const float lineHeight = size * screenScale; // Pixels.
				const glm::vec2 origin(worldMatrix[3].x * width, worldMatrix[3].y * height);
				for (const GlyphQuad& quad : layout.Quads)
				{
					const glm::vec2 min = origin + (quad.Min + glm::vec2(anchorShift, 0.0f)) * lineHeight;
					const glm::vec2 max = origin + (quad.Max + glm::vec2(anchorShift, 0.0f)) * lineHeight;
					auto clip = [&](float x, float y) { return glm::vec3(x / width * 2.0f - 1.0f, 1.0f - y / height * 2.0f, 0.0f); };
					const glm::vec3 corners[4] = {clip(min.x, min.y), clip(max.x, min.y), clip(max.x, max.y), clip(min.x, max.y)};
					appendQuad(screen, corners, quad, color);
				}
			}
			else
			{
				// Local XY plane, y up: the first line's top is at the entity's origin.
				for (const GlyphQuad& quad : layout.Quads)
				{
					const glm::vec2 min = (quad.Min + glm::vec2(anchorShift, 0.0f)) * size;
					const glm::vec2 max = (quad.Max + glm::vec2(anchorShift, 0.0f)) * size;
					auto place = [&](float x, float y) { return glm::vec3(worldMatrix * glm::vec4(x, -y, 0.0f, 1.0f)); };
					const glm::vec3 corners[4] = {place(min.x, min.y), place(max.x, min.y), place(max.x, max.y), place(min.x, max.y)};
					appendQuad(world, corners, quad, color);
				}
			}
		});
		if (world.empty() && screen.empty())
			return 0;

		m_Vertices.clear();
		m_Vertices.insert(m_Vertices.end(), world.begin(), world.end());
		m_Vertices.insert(m_Vertices.end(), screen.begin(), screen.end());
		const size_t bytes = m_Vertices.size() * sizeof(Vertex);
		if (!m_VertexBuffer || m_VertexBuffer->getDesc().byteSize < bytes)
		{
			m_VertexBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
														.setByteSize(std::max<size_t>(bytes * 2, 64 * 1024))
														.setIsVertexBuffer(true)
														.setInitialState(nvrhi::ResourceStates::VertexBuffer)
														.setKeepInitialState(true)
														.setDebugName("TextVertices"));
		}
		commandList->writeBuffer(m_VertexBuffer, m_Vertices.data(), bytes);

		auto draw = [&](nvrhi::IFramebuffer* framebuffer, bool depthTest, const glm::mat4& transform, uint32_t firstVertex, uint32_t vertexCount) {
			nvrhi::GraphicsState state;
			state.pipeline = GetPipeline(framebuffer, depthTest);
			state.framebuffer = framebuffer;
			state.bindings = {m_BindingSet};
			state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(m_VertexBuffer)};
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(width, height));
			commandList->setGraphicsState(state);
			TextConstants constants{transform};
			commandList->setPushConstants(&constants, sizeof(constants));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(vertexCount).setStartVertexLocation(firstVertex));
		};
		if (!world.empty())
		{
			nvrhi::IFramebuffer* framebuffer = depth ? GetDepthFramebuffer(output, depth) : output;
			draw(framebuffer, framebuffer != output, viewProjection, 0, static_cast<uint32_t>(world.size()));
		}
		if (!screen.empty())
			draw(output, false, glm::mat4(1.0f), static_cast<uint32_t>(world.size()), static_cast<uint32_t>(screen.size()));
		return glyphs;
	}

}
