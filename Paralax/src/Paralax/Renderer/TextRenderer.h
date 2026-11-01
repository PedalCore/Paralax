#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Renderer/FontAtlas.h"

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Paralax {

	class Scene;

	// Draws the scene's Text components (see TextComponent) over an already tone-mapped image, from a
	// signed-distance-field atlas of the engine's font, so text stays sharp at any size and is never affected by
	// exposure. Screen-space text covers the whole output; world-space text is depth-tested against the scene.
	// Main-thread-only.
	class TextRenderer
	{
	public:
		// Glyphs drawn in one frame beyond this are dropped (with the rest of their entity's text).
		static constexpr uint32_t MaxGlyphsPerFrame = 65536;
		// Screen-space sizes are given for a viewport this high and scale with the real one.
		static constexpr float ReferenceViewportHeight = 1080.0f;

		explicit TextRenderer(nvrhi::IDevice* device);
		~TextRenderer();

		TextRenderer(const TextRenderer&) = delete;
		TextRenderer& operator=(const TextRenderer&) = delete;

		// Builds the glyph atlas and GPU state, uploading the atlas through the open command list. Returns false
		// (with an error logged) if the font or shaders are unavailable.
		bool Initialize(nvrhi::ICommandList* commandList);

		// Records the scene's text into the open command list, over output. World-space text is depth-tested against
		// depth (the scene's reversed-Z depth buffer, the output's size) when given, otherwise drawn on top. Returns
		// the number of glyphs drawn.
		uint32_t Render(nvrhi::ICommandList* commandList, Scene& scene, const glm::mat4& viewProjection, nvrhi::IFramebuffer* output, nvrhi::ITexture* depth);

		const FontAtlas* GetAtlas() const { return m_Atlas.get(); }

	private:
		struct Vertex
		{
			glm::vec3 Position;
			glm::vec2 UV;
			glm::vec4 Color;
		};

		struct DepthFramebuffer
		{
			nvrhi::ITexture* Color = nullptr;
			nvrhi::ITexture* Depth = nullptr;
			nvrhi::FramebufferHandle Framebuffer;
			uint64_t LastUsedFrame = 0;
		};

		nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* framebuffer, bool depthTest);
		nvrhi::IFramebuffer* GetDepthFramebuffer(nvrhi::IFramebuffer* output, nvrhi::ITexture* depth);

		nvrhi::DeviceHandle m_Device;
		Ref<FontAtlas> m_Atlas;
		nvrhi::TextureHandle m_AtlasTexture;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::BindingSetHandle m_BindingSet;
		std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> m_Pipelines;
		// Framebuffers pairing an output's colour image with the scene depth, for depth-tested world-space text. An
		// entry keeps its images alive (a swapchain's, after the swapchain is replaced, is legal but wasteful), so
		// entries unused for a few frames are evicted every frame.
		std::vector<DepthFramebuffer> m_DepthFramebuffers;
		nvrhi::BufferHandle m_VertexBuffer;
		std::vector<Vertex> m_Vertices;
		uint64_t m_Frame = 0;
	};

}
