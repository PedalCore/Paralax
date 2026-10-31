#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <unordered_map>

namespace Paralax {

	class MeshAsset;
	class TextureAsset;

	struct GPUMesh
	{
		nvrhi::BufferHandle VertexBuffer;
		nvrhi::BufferHandle IndexBuffer;
		uint32_t Version = 0;
	};

	struct GPUTexture
	{
		nvrhi::TextureHandle Texture;
		uint32_t Version = 0;
	};

	// GPU copies of CPU assets, created on first use and re-uploaded when the asset's Version changes. Uploads are
	// recorded into the command list passed in (which must be open). Main-thread-only.
	class GPUResourceCache
	{
	public:
		explicit GPUResourceCache(nvrhi::IDevice* device);

		const GPUMesh* GetMesh(const MeshAsset& mesh, nvrhi::ICommandList* commandList);
		nvrhi::ITexture* GetTexture(const TextureAsset& texture, nvrhi::ICommandList* commandList);

		// 1x1 fallback textures used when a material slot has no texture.
		nvrhi::ITexture* GetWhiteTexture() const { return m_White; }
		nvrhi::ITexture* GetBlackTexture() const { return m_Black; }
		// Tangent-space "flat" normal (0.5, 0.5, 1).
		nvrhi::ITexture* GetFlatNormalTexture() const { return m_FlatNormal; }

		// Uploads the fallback textures; call once with an open command list before first use.
		void Initialize(nvrhi::ICommandList* commandList);

		void Remove(AssetHandle handle);
		void Clear();
		size_t GetMeshCount() const { return m_Meshes.size(); }
		size_t GetTextureCount() const { return m_Textures.size(); }

	private:
		nvrhi::TextureHandle CreateSolidTexture(nvrhi::ICommandList* commandList, uint32_t rgba, const char* name, bool srgb);

	private:
		nvrhi::DeviceHandle m_Device;
		std::unordered_map<uint64_t, GPUMesh> m_Meshes;
		std::unordered_map<uint64_t, GPUTexture> m_Textures;
		nvrhi::TextureHandle m_White;
		nvrhi::TextureHandle m_Black;
		nvrhi::TextureHandle m_FlatNormal;
	};

}
