#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Core/JsonText.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
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
		// The registry format SaveRegistry writes, and the newest LoadRegistry reads.
		static constexpr int CurrentRegistryVersion = 1;
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
		// The handle of the file asset at path (absolute, or relative to the asset directory), or 0 if none is
		// registered there. Where a hand-edited registry lists the file under several handles, the lowest, on every
		// platform; what is done through the path (a reload, an edit) then reaches only that handle's cached asset.
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

		// Writes AssetRegistry.json atomically. Returns false (with an error logged), leaving the file on disk as it was,
		// if the registry is larger than LoadRegistry reads (MaxRegistryBytes), while the registry is read-only (see
		// IsRegistryReadOnly) and its file is still in place, or if the file has changed on disk since this manager
		// loaded or last saved it (a version-control checkout or merge, or another editor): saving would replace that
		// change with this registry. The file is compared by size and a hash of its bytes, not by modification time,
		// which a checkout can leave as it was. To recover from a change, call LoadRegistry (the editor's
		// asset.reload_registry) or reopen the project, then save again. A missing file is never refused: it is created
		// anew. The file is not locked, so a change made between the check and the write is still replaced.
		bool SaveRegistry() const;
		// Replaces the registry with the one on disk (a missing file yields an empty registry, as for a new project),
		// and records what the file is, so SaveRegistry can tell whether it changes. A loaded asset whose entry the new
		// registry drops, or gives another type, file or parent, leaves the cache: the next GetAsset loads it from its new
		// entry, with a Version above the one it had, so caches keyed on (handle, Version) never serve the old one.
		// Other loaded assets, memory assets and built-ins stay cached as they are.
		//
		// Returns false, with an error logged, if the file exists but cannot be read: too large, nested too deeply,
		// malformed (merge-conflict markers, say), not an asset registry, of a version this build does not read (1 to
		// CurrentRegistryVersion: a newer engine's registry, say) or unreadable. Nothing is registered then, and the
		// registry is read-only until a LoadRegistry succeeds, so imports, removals and reloads fail instead of replacing
		// the file, and every handle it lists, with only what was registered since.
		//
		// Entries it cannot read (an asset type this build does not know, a handle that is not one or is reserved, a
		// path outside the asset directory, a parent that is not a registered file asset, a second, different entry
		// for a handle) are skipped, with one warning that counts them; the rest are registered and true is returned.
		// The registry is then read-only too, since a save would drop the skipped entries for good.
		//
		// To recover, repair the file or restore it (from version control, say), then reopen the project or call
		// LoadRegistry (the editor's asset.reload_registry). Deleting the file gives up what it listed; saves then
		// create a new one.
		bool LoadRegistry();
		// True while SaveRegistry refuses to replace the file because of what the last LoadRegistry found: a file it
		// could not read, or entries it skipped. A LoadRegistry that reads every entry clears it, and so does a save
		// once the file is gone (it is then created anew).
		bool IsRegistryReadOnly() const { return m_RegistryGuard != RegistryGuard::None; }
		// How many entries the last LoadRegistry skipped because it could not read them.
		size_t GetSkippedRegistryEntryCount() const { return m_SkippedEntries; }

		// Converts between absolute paths and registry paths (relative, '/' separators). Returns nullopt for
		// paths outside the asset directory, and for names that are not well-formed UTF-8 (the registry and scenes
		// store paths as UTF-8, so such a name would point at no file once read back); outError, if given, then
		// says why the name was refused (it stays empty for a path outside the directory).
		std::optional<std::filesystem::path> ToRegistryPath(const std::filesystem::path& path, std::string* outError = nullptr) const;
		std::filesystem::path ToAbsolutePath(const std::filesystem::path& registryPath) const;

	private:
		// Why SaveRegistry must not replace the registry file while it is in place.
		enum class RegistryGuard : uint8_t
		{
			None,
			Unreadable,     // The last LoadRegistry could not read the file: saving would replace every entry.
			SkippedEntries, // The last LoadRegistry skipped entries it could not read: saving would drop them.
		};

		// What a registry file holds, to tell it from another: its size and an FNV-1a hash of its bytes. This detects
		// accidental changes (a checkout, another editor), not forgeries.
		struct RegistryFileIdentity
		{
			uint64_t Size = 0;
			uint64_t Hash = 0;

			bool operator==(const RegistryFileIdentity&) const = default;
		};

		static RegistryFileIdentity IdentifyRegistryText(std::string_view text);

		Ref<Asset> LoadAsset(const AssetMetadata& metadata);
		// Puts a file-backed asset in the cache under handle. One that replaces an asset LoadRegistry evicted gets a
		// Version above the evicted one's.
		void CacheAsset(AssetHandle handle, Ref<Asset> asset);
		// Replaces m_Registry and the save guard with what the registry file holds (the body of LoadRegistry).
		bool ReadRegistryFile();
		Ref<Asset> CreateBuiltin(AssetHandle handle) const;
		// Takes an asset and its sub-assets out of the registry and the cache (without saving), returning them so a
		// failed save can put them back.
		std::vector<std::pair<AssetMetadata, Ref<Asset>>> DetachAsset(AssetHandle handle);
		// Whether SaveRegistry may replace the file at path: there is none, or it is, unchanged, the file the last
		// LoadRegistry read in full or the last SaveRegistry wrote. Logs why not.
		bool CanReplaceRegistryFile(const std::filesystem::path& path) const;

	private:
		std::filesystem::path m_AssetDirectory;
		size_t m_MaxRegistryBytes = MaxRegistryBytes;
		// Set by LoadRegistry, and kept until a save replaces the file. Mutable because SaveRegistry, which is const (it
		// changes nothing that is registered), clears it once it has written the file anew: from then on the file is one
		// this manager wrote.
		mutable RegistryGuard m_RegistryGuard = RegistryGuard::None;
		size_t m_SkippedEntries = 0;
		// The registry file as the last LoadRegistry read it or the last SaveRegistry wrote it; nullopt if there was
		// none (or it could not be read). Mutable for the same reason.
		mutable std::optional<RegistryFileIdentity> m_RegistryIdentity;
		std::unordered_map<uint64_t, AssetMetadata> m_Registry;
		std::unordered_map<uint64_t, Ref<Asset>> m_Loaded;
		// The Version each asset LoadRegistry evicted had, until its handle is loaded again (CacheAsset).
		std::unordered_map<uint64_t, uint32_t> m_EvictedVersions;
		// Handles that are unknown or failed to load, reported once (cleared by imports, reloads and registry loads).
		std::unordered_set<uint64_t> m_Failed;
		// Sub-assets produced by the most recent mesh load (handle, type), used to rebuild registry entries.
		std::vector<std::pair<AssetHandle, AssetType>> m_LastSubAssets;
	};

}
