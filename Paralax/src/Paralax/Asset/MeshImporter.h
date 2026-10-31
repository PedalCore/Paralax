#pragma once

#include "Paralax/Asset/MaterialAsset.h"
#include "Paralax/Asset/MeshAsset.h"
#include "Paralax/Asset/TextureAsset.h"
#include "Paralax/Core/Base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Paralax {

	// Everything imported from one glTF file: the mesh plus the materials and textures it references. Materials and
	// textures are sub-assets whose handles are derived deterministically from the mesh handle (see
	// DeriveSubAssetHandle), so scenes referencing them stay valid across re-imports.
	struct MeshImportResult
	{
		Ref<MeshAsset> Mesh;
		std::vector<Ref<MaterialAsset>> Materials;
		std::vector<Ref<TextureAsset>> Textures;
		std::vector<std::string> Warnings;
	};

	enum class SubAssetKind : uint64_t
	{
		Material = 1,
		Texture = 2
	};

	// Stable handle for the index-th sub-asset of a kind within a parent asset. Never returns 0.
	AssetHandle DeriveSubAssetHandle(AssetHandle parent, SubAssetKind kind, uint64_t index);

	class MeshImporter
	{
	public:
		// Imports a .gltf/.glb file: all triangle primitives of the default scene (or scene 0), flattened into
		// submeshes with their node transforms; PBR materials; and embedded or external images. Returns false
		// (with outError set) if the file cannot be parsed or contains no triangles. Unsupported features (other
		// primitive types, missing images) are skipped with warnings.
		static bool Import(const std::filesystem::path& path, AssetHandle meshHandle, MeshImportResult& outResult, std::string& outError);
	};

}
