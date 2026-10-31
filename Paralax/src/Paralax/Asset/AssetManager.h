#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Paralax {

	// Owns the asset registry of a project and the cache of loaded assets. Main-thread-only.
	//
	// Registered assets live in files under the asset directory and are listed in "AssetRegistry.json" there.
	// Sub-assets (materials and textures inside a glTF file) are registered with their parent's handle and are
	// loaded by loading the parent. Memory assets (built-ins, generated content) exist only in the cache.
	class AssetManager
	{
	public:
		// Well-known handles of built-in assets, available in every project.
		static constexpr uint64_t BuiltinCube = 1;
		static constexpr uint64_t BuiltinSphere = 2;
		static constexpr uint64_t BuiltinPlane = 3;
		static constexpr uint64_t BuiltinCylinder = 4;
		static constexpr uint64_t BuiltinCapsule = 5;
		static constexpr uint64_t BuiltinDefaultMaterial = 16;
		static constexpr uint64_t FirstUserHandle = 1024; // Generated handles are never below this.

		static constexpr const char* RegistryFileName = "AssetRegistry.json";

		// assetDirectory may be empty (no project): only built-in and memory assets are then available.
		explicit AssetManager(std::filesystem::path assetDirectory = {});
		~AssetManager();

		AssetManager(const AssetManager&) = delete;
		AssetManager& operator=(const AssetManager&) = delete;

		const std::filesystem::path& GetAssetDirectory() const { return m_AssetDirectory; }

		// Registers a file (absolute, or relative to the asset directory) and returns its handle. Importing an
		// already registered file returns the existing handle. Returns 0 (with an error logged) if the file does
		// not exist, lies outside the asset directory or has an unsupported type. Mesh imports also register
		// their materials and textures as sub-assets. The registry is saved automatically.
		AssetHandle ImportAsset(const std::filesystem::path& path);

		// Removes an asset (and its sub-assets) from the registry and the cache. The file is not deleted.
		bool RemoveAsset(AssetHandle handle);

		bool IsRegistered(AssetHandle handle) const;
		bool IsLoaded(AssetHandle handle) const;
		bool IsBuiltin(AssetHandle handle) const;
		const AssetMetadata* GetMetadata(AssetHandle handle) const;
		AssetHandle GetHandleForPath(const std::filesystem::path& path) const;
		// The asset's type, from metadata, the cache or the built-in table; None if unknown.
		AssetType GetAssetType(AssetHandle handle) const;
		// All registered assets (including sub-assets), sorted by path then handle.
		std::vector<AssetMetadata> GetRegisteredAssets() const;

		// Returns the asset, loading it on first use. Returns nullptr (errors logged) if the handle is unknown or
		// loading fails. Typed overload returns nullptr if the asset is of a different type.
		Ref<Asset> GetAsset(AssetHandle handle);
		template<typename T>
		Ref<T> GetAsset(AssetHandle handle)
		{
			return std::dynamic_pointer_cast<T>(GetAsset(handle));
		}

		// Adds an asset that exists only in memory. Verifies the handle is set and not already in use.
		void AddMemoryAsset(Ref<Asset> asset);

		// Reloads a file-backed asset from disk, bumping its Version. Returns false if it cannot be reloaded.
		bool ReloadAsset(AssetHandle handle);

		bool SaveRegistry() const;
		// Replaces the registry with the one on disk (a missing file yields an empty registry).
		bool LoadRegistry();

		// Converts between absolute paths and registry paths (relative, '/' separators). Returns nullopt for
		// paths outside the asset directory.
		std::optional<std::filesystem::path> ToRegistryPath(const std::filesystem::path& path) const;
		std::filesystem::path ToAbsolutePath(const std::filesystem::path& registryPath) const;

	private:
		Ref<Asset> LoadAsset(const AssetMetadata& metadata);
		Ref<Asset> CreateBuiltin(AssetHandle handle) const;

	private:
		std::filesystem::path m_AssetDirectory;
		std::unordered_map<uint64_t, AssetMetadata> m_Registry;
		std::unordered_map<uint64_t, Ref<Asset>> m_Loaded;
		// Sub-assets produced by the most recent mesh load (handle, type), used to rebuild registry entries.
		std::vector<std::pair<AssetHandle, AssetType>> m_LastSubAssets;
	};

}
