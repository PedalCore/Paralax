#include "pxpch.h"
#include "Paralax/Renderer/GPUResourceCache.h"

#include "Paralax/Asset/MeshAsset.h"
#include "Paralax/Asset/TextureAsset.h"

namespace Paralax {

	GPUResourceCache::GPUResourceCache(nvrhi::IDevice* device)
		: m_Device(device)
	{
		PX_CORE_VERIFY(device, "GPUResourceCache requires a device");
	}

	void GPUResourceCache::Initialize(nvrhi::ICommandList* commandList)
	{
		m_White = CreateSolidTexture(commandList, 0xFFFFFFFFu, "WhiteTexture", false);
		m_Black = CreateSolidTexture(commandList, 0xFF000000u, "BlackTexture", false);
		m_FlatNormal = CreateSolidTexture(commandList, 0xFFFF8080u, "FlatNormalTexture", false);
	}

	nvrhi::TextureHandle GPUResourceCache::CreateSolidTexture(nvrhi::ICommandList* commandList, uint32_t rgba, const char* name, bool srgb)
	{
		nvrhi::TextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.format = srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
		desc.debugName = name;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle texture = m_Device->createTexture(desc);
		// Little-endian: 0xAABBGGRR packs bytes R, G, B, A.
		commandList->writeTexture(texture, 0, 0, &rgba, 4);
		return texture;
	}

	const GPUMesh* GPUResourceCache::GetMesh(const MeshAsset& mesh, nvrhi::ICommandList* commandList)
	{
		auto& validation = m_MeshValidation[mesh.Handle];
		if (!validation.Checked || validation.Version != mesh.Version)
		{
			std::string error;
			validation.Checked = true;
			validation.Version = mesh.Version;
			validation.Valid = mesh.Validate(error) && !mesh.Vertices.empty() && !mesh.Indices.empty();
			if (!validation.Valid)
			{
				PX_CORE_ERROR("Rejected renderer mesh {} version {}: {}", static_cast<uint64_t>(mesh.Handle), mesh.Version, error.empty() ? "empty geometry" : error);
				m_Meshes.erase(mesh.Handle);
			}
		}
		if (!validation.Valid)
			return nullptr;

		GPUMesh& entry = m_Meshes[mesh.Handle];
		if (entry.VertexBuffer && entry.Version == mesh.Version)
			return &entry;

		nvrhi::BufferDesc vertexDesc;
		vertexDesc.byteSize = mesh.Vertices.size() * sizeof(MeshVertex);
		vertexDesc.isVertexBuffer = true;
		vertexDesc.canHaveRawViews = true;
		vertexDesc.debugName = "MeshVertices";
		vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
		vertexDesc.keepInitialState = true;
		auto vertices = m_Device->createBuffer(vertexDesc);

		nvrhi::BufferDesc indexDesc;
		indexDesc.byteSize = mesh.Indices.size() * sizeof(uint32_t);
		indexDesc.isIndexBuffer = true;
		indexDesc.canHaveRawViews = true;
		indexDesc.debugName = "MeshIndices";
		indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
		indexDesc.keepInitialState = true;
		auto indices = m_Device->createBuffer(indexDesc);
		if (!vertices || !indices)
		{
			if (!validation.ReportedAllocationFailure)
				PX_CORE_ERROR("Failed to allocate renderer mesh {}", static_cast<uint64_t>(mesh.Handle));
			validation.ReportedAllocationFailure = true;
			return nullptr;
		}
		validation.ReportedAllocationFailure = false;
		entry.VertexBuffer = std::move(vertices);
		entry.IndexBuffer = std::move(indices);

		commandList->writeBuffer(entry.VertexBuffer, mesh.Vertices.data(), vertexDesc.byteSize);
		commandList->writeBuffer(entry.IndexBuffer, mesh.Indices.data(), indexDesc.byteSize);
		entry.Version = mesh.Version;
		return &entry;
	}

	nvrhi::ITexture* GPUResourceCache::GetTexture(const TextureAsset& texture, nvrhi::ICommandList* commandList)
	{
		if (texture.Mips.empty() || texture.Width == 0 || texture.Height == 0)
			return nullptr;

		GPUTexture& entry = m_Textures[texture.Handle];
		if (entry.Texture && entry.Version == texture.Version)
			return entry.Texture;

		nvrhi::TextureDesc desc;
		desc.width = texture.Width;
		desc.height = texture.Height;
		desc.mipLevels = texture.GetMipCount();
		desc.format = texture.Format == TextureFormat::RGBA32F ? nvrhi::Format::RGBA32_FLOAT
					  : texture.SRGB                           ? nvrhi::Format::SRGBA8_UNORM
															   : nvrhi::Format::RGBA8_UNORM;
		desc.debugName = "AssetTexture";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		entry.Texture = m_Device->createTexture(desc);

		uint32_t width = texture.Width;
		for (uint32_t mip = 0; mip < texture.GetMipCount(); mip++)
		{
			commandList->writeTexture(entry.Texture, 0, mip, texture.Mips[mip].data(), static_cast<size_t>(width) * texture.GetBytesPerPixel());
			width = std::max(width / 2, 1u);
		}
		entry.Version = texture.Version;
		return entry.Texture;
	}

	void GPUResourceCache::Remove(AssetHandle handle)
	{
		m_Meshes.erase(handle);
		m_MeshValidation.erase(handle);
		m_Textures.erase(handle);
	}

	void GPUResourceCache::Clear()
	{
		m_Meshes.clear();
		m_MeshValidation.clear();
		m_Textures.clear();
	}

}
