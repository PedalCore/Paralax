#pragma once

#include "Paralax/Asset/MaterialAsset.h"
#include "Paralax/Asset/MeshAsset.h"
#include "Paralax/Asset/TextureAsset.h"
#include "Paralax/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Paralax {

	// A glTF camera, converted to Paralax's conventions (looks along the node's -Z, like Paralax cameras).
	struct ImportedCamera
	{
		bool Orthographic = false;
		float FieldOfView = 1.0f;       // Vertical, radians (perspective).
		float OrthographicSize = 10.0f; // Full height of the view volume (orthographic).
		float Near = 0.1f;
		float Far = 1000.0f;
	};

	enum class ImportedLightType
	{
		Directional,
		Point,
		Spot
	};

	// A KHR_lights_punctual light, converted to Paralax's units (see MeshImporter::Import).
	struct ImportedLight
	{
		ImportedLightType Type = ImportedLightType::Point;
		glm::vec3 Color{1.0f};
		float Intensity = 1.0f;
		float Range = 10.0f;         // Point and spot.
		float InnerConeAngle = 0.0f; // Spot: half-angles, radians.
		float OuterConeAngle = 0.785f;
	};

	// One node of the file's scene, with its local transform (relative to its parent node).
	struct ImportedNode
	{
		std::string Name;
		std::optional<size_t> Parent; // Index into MeshImportResult::Nodes; parents always come before children.
		glm::vec3 Translation{0.0f};
		glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 Scale{1.0f};
		AssetHandle Mesh = 0; // The node's mesh (one of MeshImportResult::NodeMeshes), or 0.
		std::optional<ImportedCamera> Camera;
		std::optional<ImportedLight> Light;
	};

	// Everything imported from one glTF file:
	// - Mesh: every triangle mesh of the scene combined, each submesh carrying its node's transform (one entity can
	//   show the whole file);
	// - NodeMeshes: each glTF mesh the scene uses, on its own and in its own space, for one entity per node;
	// - Nodes: the scene's node hierarchy (see BuildModelPrefab);
	// - Materials and Textures, shared by both kinds of mesh.
	// Every sub-asset's handle is derived deterministically from the file's handle (see DeriveSubAssetHandle), so
	// scenes and prefabs referencing them stay valid across re-imports.
	struct MeshImportResult
	{
		Ref<MeshAsset> Mesh;
		std::vector<Ref<MeshAsset>> NodeMeshes;
		std::vector<ImportedNode> Nodes;
		std::vector<Ref<MaterialAsset>> Materials;
		std::vector<Ref<TextureAsset>> Textures;
		std::vector<std::string> Warnings;
	};

	enum class SubAssetKind : uint64_t
	{
		Material = 1,
		Texture = 2,
		Mesh = 3,         // A per-node mesh, keyed by its (unique) name.
		MeshByIndex = 4,  // A per-node mesh without a unique name, keyed by its index in the file.
		HeightTexture = 5 // A parallax height map named by a material's extras, keyed by its file name.
	};

	// Stable handle for the index-th sub-asset of a kind within a parent asset. Never returns 0.
	AssetHandle DeriveSubAssetHandle(AssetHandle parent, SubAssetKind kind, uint64_t index);

	class MeshImporter
	{
	public:
		// Imports a .gltf/.glb file: the triangle meshes of the default scene (or scene 0), combined and per node; its
		// node hierarchy with cameras and KHR_lights_punctual lights; PBR materials; and embedded or external images.
		// Returns false (with outError set) if the file cannot be parsed or contains no triangles. Unsupported
		// features (other primitive types, missing images, skins, animations) are skipped with warnings.
		//
		// Light intensities are divided by 683 lm/W, undoing the photometric conversion of Blender's exporter in its
		// default ("Standard") lighting mode, so a Blender sun of strength 3 becomes a DirectionalLight of Intensity 3
		// and a 1000 W point light (1000 / 4 pi before the conversion) one of about 80. A point or spot light without
		// a range reaches as far as its light stays above 0.01 (10 * sqrt(Intensity) metres). Values are clamped to
		// the components' ranges. Malformed light data is ignored with a warning rather than failing the import.
		//
		// Parallax occlusion mapping, which glTF cannot express, is read from material extras as Blender exporters can
		// write them: "pom": true (or "parallax_mode": "POM"), "height_map" (1 = surface; looked up beside the model, in
		// its "textures" folder, then beside the material's base color image, never outside the model's folder),
		// "height_depth_m" and "texture_repeat_m"; HeightScale becomes depth / repeat (UV units). A 16-bit height map
		// keeps its precision.
		//
		// Per-node meshes are keyed by mesh name (by index when unnamed or repeated), so re-exports that renumber
		// meshes keep each entity's mesh. They are built whenever the file is loaded, which roughly doubles its
		// CPU-side geometry while it is loaded.
		static bool Import(const std::filesystem::path& path, AssetHandle meshHandle, MeshImportResult& outResult, std::string& outError);
	};

}
