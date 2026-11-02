#pragma once

#include "Paralax/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <unordered_map>
#include <unordered_set>

struct ImDrawData;
struct ImTextureData;

namespace Paralax {

	// Which ImGui textures have a failed creation on record. ImGui asks for a texture again every frame until it
	// exists, so ImGuiRenderer logs a failure only the first time, and again only once the texture has been created or
	// destroyed in between. Main-thread-only.
	class ImGuiTextureFailures
	{
	public:
		// Records a failed creation. Returns true for the texture's first failure since it was last created or
		// destroyed: the one to log.
		bool Failed(int textureId) { return m_Failed.insert(textureId).second; }
		void Created(int textureId) { m_Failed.erase(textureId); }
		void Destroyed(int textureId) { m_Failed.erase(textureId); }
		void Clear() { m_Failed.clear(); }
		size_t GetCount() const { return m_Failed.size(); }

	private:
		std::unordered_set<int> m_Failed;
	};

	// Renders Dear ImGui draw data with NVRHI, including ImGui's dynamic texture requests (font atlas creation and
	// updates, ImGuiBackendFlags_RendererHasTextures). Images drawn with ImGui::Image use an nvrhi::ITexture* as
	// their ImTextureID (see ToImTextureID). Main-thread-only.
	class ImGuiRenderer
	{
	public:
		explicit ImGuiRenderer(nvrhi::IDevice* device);
		~ImGuiRenderer();

		ImGuiRenderer(const ImGuiRenderer&) = delete;
		ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

		// Creates shaders and pipeline state. Returns false (with an error logged) on failure.
		bool Initialize();

		// Processes texture requests and draws into the framebuffer (which must have one color attachment).
		void Render(ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer);

		// Destroys all textures ImGui asked this backend to create.
		void Shutdown();

		static uint64_t ToImTextureID(nvrhi::ITexture* texture) { return reinterpret_cast<uint64_t>(texture); }

	private:
		void UpdateTexture(ImTextureData* texture, nvrhi::ICommandList* commandList);
		nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture);
		nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* framebuffer);

	private:
		nvrhi::DeviceHandle m_Device;
		nvrhi::CommandListHandle m_CommandList;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::BufferHandle m_VertexBuffer;
		nvrhi::BufferHandle m_IndexBuffer;
		std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> m_Pipelines;
		// Binding sets per texture; textures unused for a few frames are evicted.
		struct CachedBindingSet
		{
			nvrhi::BindingSetHandle Set;
			nvrhi::TextureHandle Texture; // Keeps the texture alive while the binding set may be in flight.
			uint64_t LastUsedFrame = 0;
		};
		std::unordered_map<nvrhi::ITexture*, CachedBindingSet> m_BindingSets;
		// GPU textures for ImGui-managed textures (font atlas), by ImTextureData::UniqueID.
		std::unordered_map<int, nvrhi::TextureHandle> m_Textures;
		ImGuiTextureFailures m_TextureFailures;
		uint64_t m_Frame = 0;
	};

}
