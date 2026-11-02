#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Core/JsonText.h"

#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Paralax {

	struct BuiltinAssetInfo
	{
		uint64_t Handle;
		AssetType Type;
		const char* Name;
	};

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
		static constexpr uint64_t BuiltinGroundMaterial = 17;
		static constexpr uint64_t BuiltinSkyEnvironment = 18;
		static constexpr uint64_t BuiltinStudioEnvironment = 19;
		static constexpr uint64_t FirstUserHandle = 1024; // Generated handles are never below this.

		// Immutable, process-lifetime view; safe on any thread and never fails.
		static std::span<const BuiltinAssetInfo> GetBuiltinAssets();

		static constexpr const char* RegistryFileName = "AssetRegistry.json";
		// The largest registry file LoadRegistry reads and SaveRegistry writes, so a saved registry always loads.
		static constexpr size_t MaxRegistryBytes = MaxTextFileBytes;

		// assetDirectory may be empty (no project): only built-in and memory assets are then available.
		// maxRegistryBytes replaces MaxRegistryBytes for this manager; it exists so tests can reach the limit with a
		// small registry, and production code leaves it alone.
		explicit AssetManager(std::filesystem::path assetDirectory = {}, size_t maxRegistryBytes = MaxRegistryBytes);
		~AssetManager();

		AssetManager(const AssetManager&) = delete;
		AssetManager& operator=(const AssetManager&) = delete;

		const std::filesystem::path& GetAssetDirectory() const { return m_AssetDirectory; }

		// Registers a file (absolute, or relative to the asset directory) and returns its handle. Importing an
		// already registered file returns the existing handle. Returns 0 (with an error logged) if the file does
		// not exist, lies outside the asset directory or has an unsupported type. Mesh imports also register
		// their materials and textures as sub-assets. The registry is saved at once; if that fails, the import is
		// undone and 0 is returned, so no handle exists that the next session would not know.
		AssetHandle ImportAsset(const std::filesystem::path& path);

		// Removes an asset (and its sub-assets) from the registry and the cache. The file is not deleted. Returns
		// false if the handle is not registered, or if the registry cannot be saved (the asset then stays).
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

		// Returns the asset, loading it on first use. Returns nullptr if the handle is unknown or loading fails; the
		// error is logged once per handle and the failure is remembered (no retry) until the asset is imported or
		// reloaded or the registry is reloaded. Typed overload returns nullptr if the asset is of a different type.
		Ref<Asset> GetAsset(AssetHandle handle);
		template<typename T>
		Ref<T> GetAsset(AssetHandle handle)
		{
			return std::dynamic_pointer_cast<T>(GetAsset(handle));
		}

		// Adds an asset that exists only in memory. Verifies the handle is set and not already in use.
		void AddMemoryAsset(Ref<Asset> asset);

		// Reloads a file-backed asset from disk, bumping its Version. Returns false if it cannot be reloaded, or if
		// the file now yields different sub-assets and the registry cannot be saved to record them (the reloaded
		// asset is kept in memory). The registry is saved only when its sub-asset entries change.
		bool ReloadAsset(AssetHandle handle);

		// Writes AssetRegistry.json atomically. Refuses a registry larger than LoadRegistry reads (MaxRegistryBytes),
		// leaving the file on disk as it was, and refuses while a file that LoadRegistry could not read is still in
		// place. Returns false (with an error logged) on failure.
		bool SaveRegistry() const;
		// Replaces the registry with the one on disk (a missing file yields an empty registry, as for a new project).
		// Returns false, with an error logged, if the file exists but cannot be read: too large, nested too deeply,
		// malformed (merge-conflict markers, say) or unreadable. Nothing is registered then, and SaveRegistry refuses
		// until a LoadRegistry succeeds, so imports, removals and reloads fail instead of replacing the file, and every
		// handle it lists, with only what was registered since. To recover, repair the file or restore it (from
		// version control, say), then reopen the project or call LoadRegistry. (Deleting the file gives up what it
		// listed; saves then create a new one.)
		bool LoadRegistry();

		// Converts between absolute paths and registry paths (relative, '/' separators). Returns nullopt for
		// paths outside the asset directory, and for names that are not well-formed UTF-8 (the registry and scenes
		// store paths as UTF-8, so such a name would point at no file once read back); outError, if given, then
		// says why the name was refused (it stays empty for a path outside the directory).
		std::optional<std::filesystem::path> ToRegistryPath(const std::filesystem::path& path, std::string* outError = nullptr) const;
		std::filesystem::path ToAbsolutePath(const std::filesystem::path& registryPath) const;

	private:
		Ref<Asset> LoadAsset(const AssetMetadata& metadata);
		Ref<Asset> CreateBuiltin(AssetHandle handle) const;
		// Takes an asset and its sub-assets out of the registry and the cache (without saving), returning them so a
		// failed save can put them back.
		std::vector<std::pair<AssetMetadata, Ref<Asset>>> DetachAsset(AssetHandle handle);

	private:
		std::filesystem::path m_AssetDirectory;
		size_t m_MaxRegistryBytes = MaxRegistryBytes;
		bool m_RegistryUnreadable = false; // The file on disk exists but the last LoadRegistry could not read it.
		std::unordered_map<uint64_t, AssetMetadata> m_Registry;
		std::unordered_map<uint64_t, Ref<Asset>> m_Loaded;
		// Handles that are unknown or failed to load, reported once (cleared by imports, reloads and registry loads).
		std::unordered_set<uint64_t> m_Failed;
		// Sub-assets produced by the most recent mesh load (handle, type), used to rebuild registry entries.
		std::vector<std::pair<AssetHandle, AssetType>> m_LastSubAssets;
	};

}
