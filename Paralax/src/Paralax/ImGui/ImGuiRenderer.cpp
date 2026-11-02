#include "pxpch.h"
#include "Paralax/ImGui/ImGuiRenderer.h"

#include "Paralax/Renderer/ShaderLibrary.h"

#include <imgui.h>

#include <glm/glm.hpp>

namespace Paralax {

	namespace {

		struct ImGuiConstants
		{
			glm::vec2 Scale;
			glm::vec2 Translate;
		};

		// Command lists keep resources they use alive until the GPU is done, so a binding set only needs to survive a
		// few frames of disuse. Keeping it longer would pin resized viewport textures (each a full-size target).
		constexpr uint64_t s_EvictAfterFrames = 4;

	}

	ImGuiRenderer::ImGuiRenderer(nvrhi::IDevice* device)
		: m_Device(device)
	{
		PX_CORE_VERIFY(device, "ImGuiRenderer requires a device");
	}

	ImGuiRenderer::~ImGuiRenderer()
	{
		Shutdown();
	}

	bool ImGuiRenderer::Initialize()
	{
		m_CommandList = m_Device->createCommandList();
		m_VertexShader = ShaderLibrary::CreateShader(m_Device, "ImGui", "ImGuiVS", nvrhi::ShaderType::Vertex);
		m_PixelShader = ShaderLibrary::CreateShader(m_Device, "ImGui", "ImGuiPS", nvrhi::ShaderType::Pixel);
		if (!m_VertexShader || !m_PixelShader)
			return false;

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, pos)).setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, uv)).setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(ImDrawVert, col)).setElementStride(sizeof(ImDrawVert)),
		};
		m_InputLayout = m_Device->createInputLayout(attributes, 3, m_VertexShader);

		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::All;
		layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(15, sizeof(ImGuiConstants)), nvrhi::BindingLayoutItem::Texture_SRV(0),
						   nvrhi::BindingLayoutItem::Sampler(0)};
		m_BindingLayout = m_Device->createBindingLayout(layout);

		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_Sampler = m_Device->createSampler(sampler);

		return m_InputLayout && m_BindingLayout && m_Sampler;
	}

	void ImGuiRenderer::Shutdown()
	{
		if (!m_Device)
			return;
		m_Device->waitForIdle();
		if (ImGui::GetCurrentContext())
		{
			for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
			{
				if (m_Textures.contains(texture->UniqueID))
				{
					texture->SetTexID(ImTextureID_Invalid);
					texture->SetStatus(ImTextureStatus_Destroyed);
				}
			}
		}
		m_Textures.clear();
		m_ReportedTextures.clear();
		m_BindingSets.clear();
	}

	void ImGuiRenderer::UpdateTexture(ImTextureData* texture, nvrhi::ICommandList* commandList)
	{
		if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
		{
			PX_CORE_VERIFY(texture->Format == ImTextureFormat_RGBA32, "ImGuiRenderer expects RGBA32 textures");
			nvrhi::TextureHandle& gpuTexture = m_Textures[texture->UniqueID];
			if (!gpuTexture)
			{
				nvrhi::TextureDesc desc;
				desc.width = static_cast<uint32_t>(texture->Width);
				desc.height = static_cast<uint32_t>(texture->Height);
				desc.format = nvrhi::Format::RGBA8_UNORM;
				desc.debugName = "ImGuiTexture";
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				gpuTexture = m_Device->createTexture(desc);
				if (!gpuTexture)
				{
					// Leave the request pending; ImGui keeps asking and draws nothing with this texture meanwhile.
					m_Textures.erase(texture->UniqueID);
					if (m_ReportedTextures.insert(texture->UniqueID).second)
						PX_CORE_ERROR("Could not create a {}x{} ImGui texture", texture->Width, texture->Height);
					return;
				}
				m_ReportedTextures.erase(texture->UniqueID);
				texture->SetTexID(ToImTextureID(gpuTexture));
			}
			// NVRHI uploads whole subresources; ImGui textures are small, so full re-uploads are cheap.
			commandList->writeTexture(gpuTexture, 0, 0, texture->GetPixels(), static_cast<size_t>(texture->GetPitch()));
			texture->SetStatus(ImTextureStatus_OK);
		}
		else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 2)
		{
			m_ReportedTextures.erase(texture->UniqueID);
			if (auto it = m_Textures.find(texture->UniqueID); it != m_Textures.end())
			{
				// The cached binding set holds its own reference, so the texture outlives frames still in flight.
				m_BindingSets.erase(it->second.Get());
				m_Textures.erase(it);
			}
			texture->SetTexID(ImTextureID_Invalid);
			texture->SetStatus(ImTextureStatus_Destroyed);
		}
	}

	nvrhi::IBindingSet* ImGuiRenderer::GetBindingSet(nvrhi::ITexture* texture)
	{
		CachedBindingSet& entry = m_BindingSets[texture];
		entry.LastUsedFrame = m_Frame;
		if (!entry.Set)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {nvrhi::BindingSetItem::PushConstants(15, sizeof(ImGuiConstants)), nvrhi::BindingSetItem::Texture_SRV(0, texture),
							 nvrhi::BindingSetItem::Sampler(0, m_Sampler)};
			entry.Set = m_Device->createBindingSet(desc, m_BindingLayout);
			entry.Texture = texture;
		}
		return entry.Set;
	}

	nvrhi::IGraphicsPipeline* ImGuiRenderer::GetPipeline(nvrhi::IFramebuffer* framebuffer)
	{
		const nvrhi::FramebufferInfo& info = framebuffer->getFramebufferInfo();
		const uint64_t key = info.colorFormats.empty() ? 0 : static_cast<uint64_t>(info.colorFormats[0]);
		if (auto it = m_Pipelines.find(key); it != m_Pipelines.end())
			return it->second;

		nvrhi::GraphicsPipelineDesc desc;
		desc.inputLayout = m_InputLayout;
		desc.VS = m_VertexShader;
		desc.PS = m_PixelShader;
		desc.bindingLayouts = {m_BindingLayout};
		desc.renderState.rasterState.setCullNone().enableScissor();
		desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
		desc.renderState.blendState.targets[0]
			.enableBlend()
			.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
			.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
			.setSrcBlendAlpha(nvrhi::BlendFactor::One)
			.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, info);
		m_Pipelines[key] = pipeline;
		return pipeline;
	}

	void ImGuiRenderer::Render(ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer)
	{
		PX_CORE_VERIFY(drawData && framebuffer, "ImGuiRenderer::Render requires draw data and a framebuffer");
		m_Frame++;
		m_CommandList->open();

		if (drawData->Textures)
		{
			for (ImTextureData* texture : *drawData->Textures)
			{
				if (texture->Status != ImTextureStatus_OK)
					UpdateTexture(texture, m_CommandList);
			}
		}

		const float width = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		const float height = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (width > 0.0f && height > 0.0f && drawData->TotalVtxCount > 0)
		{
			std::vector<ImDrawVert> vertices;
			std::vector<ImDrawIdx> indices;
			vertices.reserve(static_cast<size_t>(drawData->TotalVtxCount));
			indices.reserve(static_cast<size_t>(drawData->TotalIdxCount) + 1);
			for (const ImDrawList* list : drawData->CmdLists)
			{
				vertices.insert(vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
				indices.insert(indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
			}
			// Small uploads go through vkCmdUpdateBuffer, which NVRHI rounds up to 4 bytes: pad so the copy never
			// reads past the vector (an odd number of 16-bit indices would otherwise overrun by two bytes).
			while ((indices.size() * sizeof(ImDrawIdx)) % 4 != 0)
				indices.push_back(0);
			const size_t vertexBytes = vertices.size() * sizeof(ImDrawVert);
			const size_t indexBytes = indices.size() * sizeof(ImDrawIdx);
			if (!m_VertexBuffer || m_VertexBuffer->getDesc().byteSize < vertexBytes)
			{
				m_VertexBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
															.setByteSize(std::max<size_t>(vertexBytes * 2, 64 * 1024))
															.setIsVertexBuffer(true)
															.setInitialState(nvrhi::ResourceStates::VertexBuffer)
															.setKeepInitialState(true)
															.setDebugName("ImGuiVertices"));
			}
			if (!m_IndexBuffer || m_IndexBuffer->getDesc().byteSize < indexBytes)
			{
				m_IndexBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
														   .setByteSize(std::max<size_t>(indexBytes * 2, 64 * 1024))
														   .setIsIndexBuffer(true)
														   .setInitialState(nvrhi::ResourceStates::IndexBuffer)
														   .setKeepInitialState(true)
														   .setDebugName("ImGuiIndices"));
			}

			m_CommandList->writeBuffer(m_VertexBuffer, vertices.data(), vertexBytes);
			m_CommandList->writeBuffer(m_IndexBuffer, indices.data(), indexBytes);

			// Maps ImGui pixel coordinates (origin top-left, +Y down) to D3D-style clip space.
			ImGuiConstants constants;
			constants.Scale = glm::vec2(2.0f / drawData->DisplaySize.x, -2.0f / drawData->DisplaySize.y);
			constants.Translate = glm::vec2(-1.0f - drawData->DisplayPos.x * constants.Scale.x, 1.0f - drawData->DisplayPos.y * constants.Scale.y);

			nvrhi::IGraphicsPipeline* pipeline = GetPipeline(framebuffer);
			const ImVec2 clipOffset = drawData->DisplayPos;
			const ImVec2 clipScale = drawData->FramebufferScale;
			uint32_t vertexOffset = 0;
			uint32_t indexOffset = 0;
			for (const ImDrawList* list : drawData->CmdLists)
			{
				for (const ImDrawCmd& command : list->CmdBuffer)
				{
					if (command.UserCallback)
					{
						// Render state is set for every command anyway, so the reset callback needs no action.
						if (command.UserCallback != ImGui::GetPlatformIO().DrawCallback_ResetRenderState)
							command.UserCallback(list, &command);
						continue;
					}

					const float minX = std::max((command.ClipRect.x - clipOffset.x) * clipScale.x, 0.0f);
					const float minY = std::max((command.ClipRect.y - clipOffset.y) * clipScale.y, 0.0f);
					const float maxX = std::min((command.ClipRect.z - clipOffset.x) * clipScale.x, width);
					const float maxY = std::min((command.ClipRect.w - clipOffset.y) * clipScale.y, height);
					if (maxX <= minX || maxY <= minY)
						continue;

					auto* texture = reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(command.GetTexID()));
					if (!texture)
						continue;

					nvrhi::GraphicsState state;
					state.pipeline = pipeline;
					state.framebuffer = framebuffer;
					state.bindings = {GetBindingSet(texture)};
					state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(m_VertexBuffer)};
					state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(m_IndexBuffer).setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT);
					state.viewport.addViewport(nvrhi::Viewport(width, height));
					state.viewport.addScissorRect(nvrhi::Rect(static_cast<int>(minX), static_cast<int>(maxX), static_cast<int>(minY), static_cast<int>(maxY)));
					m_CommandList->setGraphicsState(state);
					m_CommandList->setPushConstants(&constants, sizeof(constants));
					m_CommandList->drawIndexed(nvrhi::DrawArguments()
												   .setVertexCount(command.ElemCount)
												   .setStartIndexLocation(indexOffset + command.IdxOffset)
												   .setStartVertexLocation(vertexOffset + command.VtxOffset));
				}
				vertexOffset += static_cast<uint32_t>(list->VtxBuffer.Size);
				indexOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
			}
		}

		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);

		// Evict binding sets for textures not drawn recently (e.g. resized viewport targets).
		for (auto it = m_BindingSets.begin(); it != m_BindingSets.end();)
		{
			if (m_Frame - it->second.LastUsedFrame > s_EvictAfterFrames)
				it = m_BindingSets.erase(it);
			else
				++it;
		}
	}

}
