#pragma once

#include "Paralax/Asset/AssetHandle.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace Paralax {

	enum class AssetType
	{
		None = 0,
		Texture,
		EnvironmentMap,
		Material,
		Mesh,
		Scene,
		Prefab,
		Script,
		Audio,
		AnimationClip // Keyframed transform animation, from a model (glTF) file.
	};

	std::string_view AssetTypeToString(AssetType type);
	AssetType AssetTypeFromString(std::string_view name);
	// Asset type implied by a file extension (case-insensitive), or AssetType::None if unsupported.
	AssetType AssetTypeFromExtension(const std::filesystem::path& path);

	// Base class of all loaded assets. Assets hold CPU-side data only; GPU copies are owned by the renderer.
	class Asset
	{
	public:
		virtual ~Asset() = default;

		virtual AssetType GetType() const = 0;

		AssetHandle Handle = 0;
		// Incremented whenever the asset's data changes (e.g. hot reload), so caches can detect stale copies.
		uint32_t Version = 1;
	};

	struct AssetMetadata
	{
		AssetHandle Handle = 0;
		AssetType Type = AssetType::None;
		// Path relative to the project's asset directory, with '/' separators. Empty for memory-only assets.
		std::filesystem::path FilePath;
		// For sub-assets (e.g. materials inside a glTF file): the asset whose loading produces this one.
		AssetHandle Parent = 0;
	};

}
