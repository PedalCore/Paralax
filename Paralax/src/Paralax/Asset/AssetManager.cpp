#include "pxpch.h"
#include "Paralax/Asset/AssetManager.h"

#include "Paralax/Asset/MaterialAsset.h"
#include "Paralax/Asset/MaterialSerializer.h"
#include "Paralax/Asset/MeshAsset.h"
#include "Paralax/Asset/MeshImporter.h"
#include "Paralax/Asset/TextureAsset.h"
#include "Paralax/Audio/AudioClip.h"
#include "Paralax/Core/EngineResources.h"
#include "Paralax/Core/JsonText.h"
#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Scene/Prefab.h"
#include "Paralax/Scene/SceneSerializer.h"
#include "Paralax/Scripting/ScriptAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <unordered_set>

namespace Paralax {

	using json = nlohmann::json;

	namespace {

		std::string HandleString(AssetHandle handle)
		{
			return std::to_string(static_cast<uint64_t>(handle));
		}

		bool ParseHandle(const json& value, AssetHandle& out)
		{
			if (!value.is_string())
				return false;
			const std::string& text = value.get_ref<const std::string&>();
			uint64_t parsed = 0;
			auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
			if (ec != std::errc() || ptr != text.data() + text.size() || text.empty())
				return false;
			out = AssetHandle(parsed);
			return true;
		}

		// A new random handle outside the reserved built-in range.
		AssetHandle GenerateUserHandle()
		{
			for (;;)
			{
				AssetHandle handle;
				if (static_cast<uint64_t>(handle) >= AssetManager::FirstUserHandle)
					return handle;
			}
		}

	}

	std::span<const BuiltinAssetInfo> AssetManager::GetBuiltinAssets()
	{
		static constexpr BuiltinAssetInfo builtins[] = {
			{BuiltinCube, AssetType::Mesh, "Cube"},
			{BuiltinSphere, AssetType::Mesh, "Sphere"},
			{BuiltinPlane, AssetType::Mesh, "Plane"},
			{BuiltinCylinder, AssetType::Mesh, "Cylinder"},
			{BuiltinCapsule, AssetType::Mesh, "Capsule"},
			{BuiltinDefaultMaterial, AssetType::Material, "Default"},
			{BuiltinGroundMaterial, AssetType::Material, "Ground"},
			{BuiltinSkyEnvironment, AssetType::EnvironmentMap, "Kloofendal Partly Cloudy"},
			{BuiltinStudioEnvironment, AssetType::EnvironmentMap, "Studio Small 09"},
		};
		return builtins;
	}

	AssetManager::AssetManager(std::filesystem::path assetDirectory, size_t maxRegistryBytes)
		: m_AssetDirectory(std::move(assetDirectory))
		, m_MaxRegistryBytes(maxRegistryBytes)
	{
		if (!m_AssetDirectory.empty())
		{
			std::error_code error;
			m_AssetDirectory = std::filesystem::weakly_canonical(m_AssetDirectory, error);
			LoadRegistry();
		}
	}

	AssetManager::~AssetManager() = default;

	std::optional<std::filesystem::path> AssetManager::ToRegistryPath(const std::filesystem::path& path, std::string* outError) const
	{
		if (m_AssetDirectory.empty())
			return std::nullopt;

		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::weakly_canonical(path.is_absolute() ? path : m_AssetDirectory / path, error);
		if (error)
			return std::nullopt;

		const std::filesystem::path relative = absolute.lexically_relative(m_AssetDirectory);
		if (relative.empty() || *relative.begin() == "..")
			return std::nullopt;
		// Only names that survive the trip through UTF-8 unchanged are accepted: any bytes are allowed on POSIX and
		// unpaired surrogates on Windows, and either would be stored altered and point at no file after a reload.
		const std::string text = ToUtf8(relative);
		std::filesystem::path registryPath = FromUtf8(text);
		if (!IsWellFormedUtf8(text) || registryPath != relative)
		{
			// Shown with U+FFFD in place of what is not UTF-8, which marks the part to rename.
			if (outError)
				*outError = fmt::format("'{}' is not a valid UTF-8 file name; rename it", Utf16ToUtf8(Utf8ToUtf16(text)));
			return std::nullopt;
		}
		return registryPath;
	}

	std::filesystem::path AssetManager::ToAbsolutePath(const std::filesystem::path& registryPath) const
	{
		return m_AssetDirectory / registryPath;
	}

	AssetHandle AssetManager::ImportAsset(const std::filesystem::path& path)
	{
		// A newly imported (or re-imported) file may fix a handle that failed before.
		m_Failed.clear();
		std::string nameError;
		const std::optional<std::filesystem::path> registryPath = ToRegistryPath(path, &nameError);
		if (!registryPath)
		{
			if (!nameError.empty())
				PX_CORE_ERROR("Cannot import: {}", nameError);
			else
				PX_CORE_ERROR("Cannot import '{}': not inside the asset directory '{}'", ToUtf8(path), ToUtf8(m_AssetDirectory));
			return 0;
		}
		if (AssetHandle existing = GetHandleForPath(*registryPath); existing.IsValid())
			return existing;

		const std::filesystem::path absolute = ToAbsolutePath(*registryPath);
		std::error_code error;
		if (!std::filesystem::is_regular_file(absolute, error))
		{
			PX_CORE_ERROR("Cannot import '{}': file not found", ToUtf8(absolute));
			return 0;
		}

		const AssetType type = AssetTypeFromExtension(absolute);
		if (type == AssetType::None)
		{
			PX_CORE_ERROR("Cannot import '{}': unsupported file type", ToUtf8(absolute));
			return 0;
		}

		AssetMetadata metadata;
		metadata.Handle = GenerateUserHandle();
		metadata.Type = type;
		metadata.FilePath = *registryPath;

		if (type == AssetType::Mesh)
		{
			// Import now so sub-assets get registered and the file is known to be valid.
			MeshImportResult result;
			std::string importError;
			if (!MeshImporter::Import(absolute, metadata.Handle, result, importError))
			{
				PX_CORE_ERROR("Cannot import '{}': {}", ToUtf8(absolute), importError);
				return 0;
			}
			for (const std::string& warning : result.Warnings)
				PX_CORE_WARN("Importing '{}': {}", ToUtf8(absolute), warning);

			for (const auto& material : result.Materials)
			{
				m_Registry[material->Handle] = {material->Handle, AssetType::Material, metadata.FilePath, metadata.Handle};
				CacheAsset(material->Handle, material);
			}
			for (const auto& texture : result.Textures)
			{
				m_Registry[texture->Handle] = {texture->Handle, AssetType::Texture, metadata.FilePath, metadata.Handle};
				CacheAsset(texture->Handle, texture);
			}
			for (const auto& nodeMesh : result.NodeMeshes)
			{
				m_Registry[nodeMesh->Handle] = {nodeMesh->Handle, AssetType::Mesh, metadata.FilePath, metadata.Handle};
				CacheAsset(nodeMesh->Handle, nodeMesh);
			}
			for (const auto& animation : result.Animations)
			{
				m_Registry[animation.Clip->Handle] = {animation.Clip->Handle, AssetType::AnimationClip, metadata.FilePath, metadata.Handle};
				CacheAsset(animation.Clip->Handle, animation.Clip);
			}
			CacheAsset(metadata.Handle, result.Mesh);
		}

		m_Registry[metadata.Handle] = metadata;
		if (!SaveRegistry())
		{
			// The registry on disk is what the next session and the runtime know: a handle recorded only in memory
			// would leave every scene that references it broken, so the import is undone instead.
			DetachAsset(metadata.Handle);
			PX_CORE_ERROR("Cannot import '{}': the asset registry could not be saved", ToUtf8(absolute));
			return 0;
		}
		PX_CORE_INFO("Imported {} '{}' as {}", AssetTypeToString(type), ToUtf8(metadata.FilePath), static_cast<uint64_t>(metadata.Handle));
		return metadata.Handle;
	}

	std::vector<std::pair<AssetMetadata, Ref<Asset>>> AssetManager::DetachAsset(AssetHandle handle)
	{
		std::vector<std::pair<AssetMetadata, Ref<Asset>>> detached;
		auto take = [&](uint64_t taken) {
			auto loaded = m_Loaded.find(taken);
			detached.emplace_back(m_Registry[taken], loaded == m_Loaded.end() ? nullptr : loaded->second);
			m_Registry.erase(taken);
			m_Loaded.erase(taken);
		};
		// Sub-assets go with their parent.
		std::vector<uint64_t> subAssets;
		for (const auto& [subHandle, metadata] : m_Registry)
		{
			if (metadata.Parent == handle)
				subAssets.push_back(subHandle);
		}
		for (uint64_t subHandle : subAssets)
			take(subHandle);
		if (m_Registry.contains(handle))
			take(handle);
		return detached;
	}

	bool AssetManager::RemoveAsset(AssetHandle handle)
	{
		m_Failed.erase(handle);
		if (!m_Registry.contains(handle))
			return false;

		const std::vector<std::pair<AssetMetadata, Ref<Asset>>> removed = DetachAsset(handle);
		if (!SaveRegistry())
		{
			// Memory and disk must agree on what is registered: put the entries back.
			for (const auto& [metadata, asset] : removed)
			{
				m_Registry[metadata.Handle] = metadata;
				if (asset)
					m_Loaded[metadata.Handle] = asset;
			}
			PX_CORE_ERROR("Cannot remove asset {}: the asset registry could not be saved", static_cast<uint64_t>(handle));
			return false;
		}
		return true;
	}

	bool AssetManager::IsRegistered(AssetHandle handle) const
	{
		return m_Registry.contains(handle);
	}

	bool AssetManager::IsLoaded(AssetHandle handle) const
	{
		return m_Loaded.contains(handle);
	}

	bool AssetManager::IsBuiltin(AssetHandle handle) const
	{
		const uint64_t value = handle;
		return std::ranges::any_of(GetBuiltinAssets(), [value](const BuiltinAssetInfo& info) { return info.Handle == value; });
	}

	const AssetMetadata* AssetManager::GetMetadata(AssetHandle handle) const
	{
		auto it = m_Registry.find(handle);
		return it == m_Registry.end() ? nullptr : &it->second;
	}

	AssetHandle AssetManager::GetHandleForPath(const std::filesystem::path& path) const
	{
		// Relative paths are normalised ("./a.png", "x/../a.png") without touching the file system.
		const std::optional<std::filesystem::path> registryPath = path.is_absolute() ? ToRegistryPath(path) : std::optional(FromUtf8(ToUtf8(path.lexically_normal())));
		if (!registryPath)
			return 0;
		// A hand-edited or merged registry can list one file under several handles (LoadRegistry warns). The lowest
		// answers, so the result never depends on the hash map's order, which differs between platforms.
		uint64_t lowest = 0;
		for (const auto& [handle, metadata] : m_Registry)
		{
			if (!metadata.Parent.IsValid() && metadata.FilePath == *registryPath && (lowest == 0 || handle < lowest))
				lowest = handle;
		}
		return lowest;
	}

	AssetType AssetManager::GetAssetType(AssetHandle handle) const
	{
		if (const AssetMetadata* metadata = GetMetadata(handle))
			return metadata->Type;
		if (auto it = m_Loaded.find(handle); it != m_Loaded.end())
			return it->second->GetType();
		for (const BuiltinAssetInfo& info : GetBuiltinAssets())
			if (info.Handle == static_cast<uint64_t>(handle))
				return info.Type;
		return AssetType::None;
	}

	std::vector<AssetMetadata> AssetManager::GetRegisteredAssets() const
	{
		std::vector<AssetMetadata> assets;
		assets.reserve(m_Registry.size());
		for (const auto& [handle, metadata] : m_Registry)
			assets.push_back(metadata);
		// Ordered by the UTF-8 text the registry stores (byte order is code point order), so the file lists entries
		// in the same order on every platform; path::operator< compares UTF-16 on Windows and bytes elsewhere.
		std::sort(assets.begin(), assets.end(), [](const AssetMetadata& a, const AssetMetadata& b) {
			const std::string pathA = ToUtf8(a.FilePath), pathB = ToUtf8(b.FilePath);
			if (pathA != pathB)
				return pathA < pathB;
			return static_cast<uint64_t>(a.Handle) < static_cast<uint64_t>(b.Handle);
		});
		return assets;
	}

	Ref<Asset> AssetManager::GetAsset(AssetHandle handle)
	{
		if (!handle.IsValid())
			return nullptr;
		if (auto it = m_Loaded.find(handle); it != m_Loaded.end())
			return it->second;
		// Failures are reported once; renderers ask for assets every frame, and retrying a missing or broken file
		// each time would flood the log and hit the disk. Importing or reloading clears this.
		if (m_Failed.contains(handle))
			return nullptr;

		if (IsBuiltin(handle))
		{
			Ref<Asset> builtin = CreateBuiltin(handle);
			if (builtin)
				m_Loaded[handle] = builtin;
			else
				m_Failed.insert(handle);
			return builtin;
		}

		const AssetMetadata* metadata = GetMetadata(handle);
		if (!metadata)
		{
			PX_CORE_ERROR("Unknown asset {}", static_cast<uint64_t>(handle));
			m_Failed.insert(handle);
			return nullptr;
		}

		if (metadata->Parent.IsValid())
		{
			// LoadRegistry and ImportAsset only register one-level parents; refuse anything else rather than recurse.
			const AssetHandle parent = metadata->Parent;
			const AssetMetadata* parentMetadata = parent != handle ? GetMetadata(parent) : nullptr;
			if (!parentMetadata || parentMetadata->Parent.IsValid())
			{
				PX_CORE_ERROR("Asset {} has an invalid parent {}", static_cast<uint64_t>(handle), static_cast<uint64_t>(parent));
				m_Failed.insert(handle);
				return nullptr;
			}
			// Loading the parent registers its sub-assets in the cache.
			if (!GetAsset(parent))
			{
				m_Failed.insert(handle);
				return nullptr;
			}
			auto it = m_Loaded.find(handle);
			if (it == m_Loaded.end())
			{
				PX_CORE_ERROR("Asset {} is no longer produced by its source file", static_cast<uint64_t>(handle));
				m_Failed.insert(handle);
				return nullptr;
			}
			return it->second;
		}

		Ref<Asset> asset = LoadAsset(*metadata);
		if (asset)
			CacheAsset(handle, asset);
		else
			m_Failed.insert(handle);
		return asset;
	}

	Ref<Asset> AssetManager::LoadAsset(const AssetMetadata& metadata)
	{
		const std::filesystem::path path = ToAbsolutePath(metadata.FilePath);
		Ref<Asset> asset;

		switch (metadata.Type)
		{
			case AssetType::Texture:
				asset = TextureAsset::LoadFromFile(path, true);
				break;
			case AssetType::EnvironmentMap:
			{
				Ref<TextureAsset> texture = TextureAsset::LoadFromFile(path, false, true, AssetType::EnvironmentMap);
				if (texture && texture->Format != TextureFormat::RGBA32F)
					PX_CORE_WARN("Environment map '{}' is not HDR; lighting will be clamped", ToUtf8(path));
				asset = texture;
				break;
			}
			case AssetType::Material:
				asset = MaterialSerializer::LoadFromFile(path);
				break;
			case AssetType::Mesh:
			{
				MeshImportResult result;
				std::string error;
				if (!MeshImporter::Import(path, metadata.Handle, result, error))
				{
					PX_CORE_ERROR("Failed to load mesh '{}': {}", ToUtf8(path), error);
					return nullptr;
				}
				m_LastSubAssets.clear();
				for (const auto& material : result.Materials)
				{
					CacheAsset(material->Handle, material);
					m_LastSubAssets.emplace_back(material->Handle, AssetType::Material);
				}
				for (const auto& texture : result.Textures)
				{
					CacheAsset(texture->Handle, texture);
					m_LastSubAssets.emplace_back(texture->Handle, AssetType::Texture);
				}
				for (const auto& nodeMesh : result.NodeMeshes)
				{
					CacheAsset(nodeMesh->Handle, nodeMesh);
					m_LastSubAssets.emplace_back(nodeMesh->Handle, AssetType::Mesh);
				}
				for (const auto& animation : result.Animations)
				{
					CacheAsset(animation.Clip->Handle, animation.Clip);
					m_LastSubAssets.emplace_back(animation.Clip->Handle, AssetType::AnimationClip);
				}
				asset = result.Mesh;
				break;
			}
			case AssetType::Prefab:
				asset = PrefabAsset::LoadFromFile(path);
				break;
			case AssetType::Script:
			{
				Ref<ScriptAsset> script = ScriptAsset::LoadFromFile(path);
				if (script)
					script->Name = ToUtf8(metadata.FilePath); // Project-relative names read better in errors.
				asset = script;
				break;
			}
			case AssetType::Audio:
				asset = AudioClip::LoadFromFile(path);
				break;
			default:
				PX_CORE_ERROR("Assets of type {} are not loaded by the AssetManager ('{}')", AssetTypeToString(metadata.Type), ToUtf8(metadata.FilePath));
				return nullptr;
		}

		if (asset)
			asset->Handle = metadata.Handle;
		return asset;
	}

	Ref<Asset> AssetManager::CreateBuiltin(AssetHandle handle) const
	{
		Ref<Asset> asset;
		switch (static_cast<uint64_t>(handle))
		{
			case BuiltinCube: asset = MeshPrimitives::CreateCube(); break;
			case BuiltinSphere: asset = MeshPrimitives::CreateSphere(); break;
			case BuiltinPlane: asset = MeshPrimitives::CreatePlane(10.0f, 10); break;
			case BuiltinCylinder: asset = MeshPrimitives::CreateCylinder(); break;
			case BuiltinCapsule: asset = MeshPrimitives::CreateCapsule(); break;
			case BuiltinDefaultMaterial:
			{
				// Light grey: a physically plausible albedo (pure white would blow out lit surfaces).
				auto material = CreateRef<MaterialAsset>();
				material->BaseColorFactor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
				asset = material;
				break;
			}
			case BuiltinGroundMaterial:
			{
				// Mid-grey stone leaves headroom for sunlight and grazing-angle sky reflections.
				auto material = CreateRef<MaterialAsset>();
				material->BaseColorFactor = glm::vec4(0.22f, 0.24f, 0.27f, 1.0f);
				material->RoughnessFactor = 0.4f;
				asset = material;
				break;
			}
			case BuiltinSkyEnvironment:
			case BuiltinStudioEnvironment:
			{
				const auto directory = GetEngineResourceDirectory();
				if (directory.empty())
				{
					PX_CORE_ERROR("Cannot locate engine resources for environment {}", static_cast<uint64_t>(handle));
					return nullptr;
				}
				const char* name = static_cast<uint64_t>(handle) == BuiltinSkyEnvironment ? "kloofendal_48d_partly_cloudy_puresky_2k.hdr" : "studio_small_09_2k.hdr";
				asset = TextureAsset::LoadFromFile(directory / "Environments" / name, false, true, AssetType::EnvironmentMap);
				break;
			}
			default: return nullptr;
		}
		if (asset)
			asset->Handle = handle;
		return asset;
	}

	void AssetManager::AddMemoryAsset(Ref<Asset> asset)
	{
		PX_CORE_VERIFY(asset && asset->Handle.IsValid(), "Memory assets need a valid handle");
		PX_CORE_VERIFY(!m_Loaded.contains(asset->Handle) && !m_Registry.contains(asset->Handle) && !IsBuiltin(asset->Handle),
					   "Asset handle {} is already in use", static_cast<uint64_t>(asset->Handle));
		m_Loaded[asset->Handle] = std::move(asset);
	}

	bool AssetManager::ReloadAsset(AssetHandle handle)
	{
		m_Failed.erase(handle);
		const AssetMetadata* metadata = GetMetadata(handle);
		if (!metadata || metadata->Parent.IsValid())
			return false;

		const uint32_t previousVersion = m_Loaded.contains(handle) ? m_Loaded[handle]->Version : 0;

		// Sub-assets are re-produced by the reload; remember their versions so caches see them change.
		std::unordered_map<uint64_t, uint32_t> previousSubVersions;
		for (const auto& [subHandle, subMetadata] : m_Registry)
		{
			if (subMetadata.Parent == handle && m_Loaded.contains(subHandle))
				previousSubVersions[subHandle] = m_Loaded[subHandle]->Version;
		}
		// The registry's record of the sub-assets, ordered so a before/after comparison is exact.
		auto subAssetEntries = [&] {
			std::map<uint64_t, std::pair<AssetType, std::string>> entries;
			for (const auto& [subHandle, subMetadata] : m_Registry)
			{
				if (subMetadata.Parent == handle)
					entries[subHandle] = {subMetadata.Type, ToUtf8(subMetadata.FilePath)};
			}
			return entries;
		};
		const auto previousEntries = subAssetEntries();

		const AssetMetadata parentMetadata = *metadata;
		Ref<Asset> asset = LoadAsset(parentMetadata);
		if (!asset)
			return false;
		asset->Version = previousVersion + 1;
		CacheAsset(handle, asset);

		if (parentMetadata.Type == AssetType::Mesh)
		{
			// Rebuild the registry's sub-asset entries from exactly what the file produces now: drop entries (and
			// cached assets) the file no longer yields, register new ones and bump versions of re-created ones.
			std::unordered_map<uint64_t, AssetType> produced;
			for (const auto& [subHandle, type] : m_LastSubAssets)
				produced[subHandle] = type;
			for (auto it = m_Registry.begin(); it != m_Registry.end();)
			{
				if (it->second.Parent == handle && !produced.contains(it->first))
				{
					m_Loaded.erase(it->first);
					it = m_Registry.erase(it);
				}
				else
				{
					++it;
				}
			}
			for (const auto& [subHandle, type] : produced)
			{
				m_Registry[subHandle] = {AssetHandle(subHandle), type, parentMetadata.FilePath, handle};
				// A sub-asset cached before counts up from its version; a new one keeps the version CacheAsset gave it (1, or
				// above that of one LoadRegistry evicted).
				if (auto previous = previousSubVersions.find(subHandle); previous != previousSubVersions.end())
					m_Loaded[subHandle]->Version = previous->second + 1;
			}
			// Only a change needs recording, so a model that kept its objects still reloads where the registry cannot be
			// written (a read-only project, a file held open elsewhere).
			if (subAssetEntries() != previousEntries && !SaveRegistry())
			{
				PX_CORE_ERROR("Reloaded '{}', but the asset registry could not be saved; its sub-assets are recorded only in memory", ToUtf8(parentMetadata.FilePath));
				return false;
			}
		}
		return true;
	}

	AssetManager::RegistryFileIdentity AssetManager::IdentifyRegistryText(std::string_view text)
	{
		// FNV-1a. Size and hash together tell a changed file from the one recorded without trusting modification
		// times, which a checkout can leave as they were and some file systems store coarsely.
		uint64_t hash = 0xCBF29CE484222325ull;
		for (const char c : text)
			hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
		return {static_cast<uint64_t>(text.size()), hash};
	}

	bool AssetManager::CanReplaceRegistryFile(const std::filesystem::path& path) const
	{
		auto cannotCheck = [&](const std::string& reason) {
			PX_CORE_ERROR("Not saving the asset registry: '{}' cannot be checked ({})", ToUtf8(path), reason);
			return false;
		};
		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		if (error)
			return cannotCheck(error.message());
		// Once the file is gone (removed by the user, or by a checkout), a save loses nothing: it creates the file anew.
		if (!exists)
			return true;
		switch (m_RegistryGuard)
		{
			case RegistryGuard::Unreadable:
				// Writing now would replace every entry of the file that failed to load with only those registered since,
				// and every handle the project's scenes use would be lost.
				PX_CORE_ERROR("Not saving the asset registry: '{}' could not be read, and saving would replace all it lists. Repair or restore the file, then reload "
							  "the registry (asset.reload_registry in the editor) or reopen the project.",
							  ToUtf8(path));
				return false;
			case RegistryGuard::SkippedEntries:
				// The entries a newer engine or a hand edit wrote, which this build cannot represent, would be gone.
				PX_CORE_ERROR("Not saving the asset registry: {} {} in '{}' could not be read when it was loaded, and saving would drop {}. Fix the file, then reload "
							  "the registry (asset.reload_registry in the editor) or reopen the project.",
							  m_SkippedEntries, m_SkippedEntries == 1 ? "entry" : "entries", ToUtf8(path), m_SkippedEntries == 1 ? "it" : "them");
				return false;
			case RegistryGuard::None: break;
		}

		// Any other file than the one last read or written was put there by someone else (a version-control checkout or
		// merge, another editor), and this registry, which never saw it, would replace it.
		std::string reason;
		const std::optional<bool> matches = RegistryFileMatches(path, reason);
		if (!matches)
			return cannotCheck(reason);
		if (!*matches)
		{
			PX_CORE_ERROR("Not saving the asset registry: '{}' has changed on disk since it was loaded or last saved (by a version-control checkout or merge, or "
						  "another editor, say), and saving would replace that change. Reload the registry (asset.reload_registry in the editor) or reopen the "
						  "project, then try again.",
						  ToUtf8(path));
			return false;
		}
		return true;
	}

	std::optional<bool> AssetManager::RegistryFileMatches(const std::filesystem::path& path, std::string& outError) const
	{
		if (!m_RegistryIdentity)
			return false;
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error)
		{
			outError = error.message();
			return std::nullopt;
		}
		if (static_cast<uint64_t>(size) != m_RegistryIdentity->Size)
			return false;
		// The size matches one this manager read or wrote, so it is within m_MaxRegistryBytes.
		std::string text;
		if (!ReadTextFile(path, text, outError, m_MaxRegistryBytes))
			return std::nullopt;
		return IdentifyRegistryText(text) == *m_RegistryIdentity;
	}

	std::optional<bool> AssetManager::HasRegistryChangedOnDisk() const
	{
		if (m_AssetDirectory.empty())
			return false;
		const std::filesystem::path path = m_AssetDirectory / RegistryFileName;
		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		if (error)
			return std::nullopt;
		if (!exists)
			return false; // A save would create it: nothing on disk differs from what it would replace.
		std::string reason;
		const std::optional<bool> matches = RegistryFileMatches(path, reason);
		if (!matches)
			return std::nullopt;
		return !*matches;
	}

	bool AssetManager::SaveRegistry() const
	{
		if (m_AssetDirectory.empty())
			return false;
		const std::filesystem::path path = m_AssetDirectory / RegistryFileName;
		if (!CanReplaceRegistryFile(path))
			return false;

		json assets = json::array();
		for (const AssetMetadata& metadata : GetRegisteredAssets())
		{
			json entry = {{"Handle", HandleString(metadata.Handle)}, {"Type", std::string(AssetTypeToString(metadata.Type))}, {"Path", ToUtf8(metadata.FilePath)}};
			if (metadata.Parent.IsValid())
				entry["Parent"] = HandleString(metadata.Parent);
			assets.push_back(std::move(entry));
		}

		json document = {{"Paralax", {{"Type", "AssetRegistry"}, {"Version", CurrentRegistryVersion}}}, {"Assets", std::move(assets)}};
		const std::string text = FormatJsonDocument(document);
		if (!WriteFileAtomically(path, text, m_MaxRegistryBytes))
			return false;
		// The file on disk is now one this manager wrote (after a guarded one was removed, the save that creates it
		// anew), so later saves may replace it as long as it stays as written.
		m_RegistryGuard = RegistryGuard::None;
		m_RegistryIdentity = IdentifyRegistryText(text);
		return true;
	}

	void AssetManager::CacheAsset(AssetHandle handle, Ref<Asset> asset)
	{
		if (const auto evicted = m_EvictedVersions.find(handle); evicted != m_EvictedVersions.end())
		{
			asset->Version = std::max(asset->Version, evicted->second + 1);
			m_EvictedVersions.erase(evicted);
		}
		m_Loaded[handle] = std::move(asset);
	}

	bool AssetManager::LoadRegistry()
	{
		const std::unordered_map<uint64_t, AssetMetadata> previous = std::move(m_Registry);
		const bool loaded = ReadRegistryFile();

		// A cached asset must be what its entry now names: otherwise play mode and captures would show an asset the
		// registry no longer lists, or the file it used to name, which an export (a copy of the file) would not. Each
		// evicted asset's Version is kept, so the one loaded in its place counts on from it and no cache keyed on
		// (handle, Version) can take one for the other. Memory assets and built-ins are never registered, so they stay.
		auto evict = [&](uint64_t handle) {
			const auto cached = m_Loaded.find(handle);
			if (cached == m_Loaded.end())
				return;
			uint32_t& version = m_EvictedVersions[handle];
			version = std::max(version, cached->second->Version);
			m_Loaded.erase(cached);
		};
		std::unordered_set<uint64_t> changed;
		for (const auto& [handle, before] : previous)
		{
			const AssetMetadata* now = GetMetadata(handle);
			if (!now || now->Type != before.Type || now->FilePath != before.FilePath || now->Parent != before.Parent)
			{
				changed.insert(handle);
				evict(handle);
			}
		}
		// Sub-assets come from their parent's file: one whose parent's entry changed is stale too, even if its own is not.
		for (const auto& [handle, metadata] : m_Registry)
		{
			if (metadata.Parent.IsValid() && changed.contains(metadata.Parent))
				evict(handle);
		}
		return loaded;
	}

	bool AssetManager::ReadRegistryFile()
	{
		m_Registry.clear();
		m_Failed.clear();
		m_RegistryGuard = RegistryGuard::None;
		m_SkippedEntries = 0;
		m_RegistryIdentity.reset();
		const std::filesystem::path path = m_AssetDirectory / RegistryFileName;
		// A registry that exists but cannot be read stays as it is on disk: SaveRegistry refuses until one loads.
		auto unreadable = [&](const std::string& reason) {
			PX_CORE_ERROR("Asset registry '{}' {}; nothing is registered, and the file is left untouched until it can be read", ToUtf8(path), reason);
			m_RegistryGuard = RegistryGuard::Unreadable;
			return false;
		};
		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		if (error)
			return unreadable(fmt::format("cannot be checked ({})", error.message()));
		if (!exists)
			return true; // A new project: the first save creates it.

		std::string text;
		std::string readError;
		if (!ReadTextFile(path, text, readError, m_MaxRegistryBytes))
			return unreadable(fmt::format("cannot be read ({})", readError));
		m_RegistryIdentity = IdentifyRegistryText(text);
		if (JsonExceedsDepth(text, MaxJsonDepth))
			return unreadable("nests too deeply");
		const json document = json::parse(text, nullptr, false);
		if (document.is_discarded() || !document.is_object())
			return unreadable("is malformed");
		// A newer engine's registry can hold what this build cannot represent (asset types, fields, another layout), and
		// rewriting it would lose that, so a version outside the ones this build reads is refused whole, before its
		// layout is looked at.
		const json* header = document.contains("Paralax") ? &document["Paralax"] : nullptr;
		if (!header || !header->is_object() || !header->contains("Type") || !(*header)["Type"].is_string() || (*header)["Type"].get_ref<const std::string&>() != "AssetRegistry")
			return unreadable("is not a Paralax asset registry (missing \"Paralax\": { \"Type\": \"AssetRegistry\" })");
		std::string foundVersion;
		if (!ReadDocumentVersion(*header, CurrentRegistryVersion, foundVersion))
			return unreadable(fmt::format("has an unsupported version: expected 1-{}, got {}", CurrentRegistryVersion, foundVersion));
		if (!document.contains("Assets") || !document["Assets"].is_array())
			return unreadable("is malformed (no \"Assets\" array)");

		auto stringField = [](const json& entry, const char* name) -> std::string {
			const auto it = entry.find(name);
			return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
		};
		// An entry that cannot be read is skipped, and the registry becomes read-only, since the next save would drop it
		// for good. The first few are named (a hostile file can hold millions, so the rest are not even described), and
		// one warning counts them all.
		constexpr size_t MaxNamedSkips = 10;
		size_t skipped = 0;
		auto skip = [&](auto&& describe) {
			if (++skipped <= MaxNamedSkips)
				PX_CORE_WARN("Asset registry '{}': skipping {}", ToUtf8(path), describe());
		};

		const json& entries = document["Assets"];
		for (size_t index = 0; index < entries.size(); index++)
		{
			const json& entry = entries[index];
			AssetMetadata metadata;
			if (!entry.is_object() || !entry.contains("Handle") || !ParseHandle(entry["Handle"], metadata.Handle) || !metadata.Handle.IsValid())
			{
				// Counted from 1, as someone reading the file counts.
				skip([&] { return fmt::format("entry #{}, which has no valid handle", index + 1); });
				continue;
			}
			const uint64_t handle = metadata.Handle;
			if (handle < FirstUserHandle)
			{
				// Built-ins own this range and GetAsset resolves them first; a registry entry there would be a
				// different asset depending on which API is asked.
				skip([&] { return fmt::format("entry {}, in the range reserved for built-in assets", handle); });
				continue;
			}
			metadata.Type = AssetTypeFromString(stringField(entry, "Type"));
			if (metadata.Type == AssetType::None)
			{
				skip([&] { return fmt::format("entry {}, whose type this build does not know", handle); });
				continue;
			}
			// Kept as a file asset, a sub-asset whose parent is unreadable would load its parent's file as itself.
			if (entry.contains("Parent") && !ParseHandle(entry["Parent"], metadata.Parent))
			{
				skip([&] { return fmt::format("entry {}, whose parent is not a handle", handle); });
				continue;
			}

			// Registry paths must be relative and stay inside the asset directory.
			const std::filesystem::path declared = FromUtf8(stringField(entry, "Path"));
			const std::optional<std::filesystem::path> contained = declared.empty() || declared.is_absolute() ? std::nullopt : ToRegistryPath(declared);
			if (!contained)
			{
				skip([&] { return fmt::format("entry {}, whose path is missing or outside the asset directory", handle); });
				continue;
			}
			metadata.FilePath = *contained;
			if (const auto existing = m_Registry.find(handle); existing != m_Registry.end())
			{
				// The same entry twice loses nothing when saved once; two different ones cannot both be kept.
				const AssetMetadata& first = existing->second;
				if (first.Type != metadata.Type || first.FilePath != metadata.FilePath || first.Parent != metadata.Parent)
					skip([&] { return fmt::format("entry {}, a second entry for that handle that differs from the first", handle); });
				continue;
			}
			m_Registry[handle] = std::move(metadata);
		}

		// Sub-assets are one level deep: a parent is a registered file asset with no parent of its own. An entry that
		// is its own parent, a cycle or a longer chain would recurse in GetAsset, so such entries are dropped here (in
		// handle order, so the warnings are the same on every platform).
		std::unordered_set<AssetHandle> fileAssets;
		for (const auto& [handle, metadata] : m_Registry)
			if (!metadata.Parent.IsValid())
				fileAssets.insert(handle);
		std::vector<uint64_t> orphans;
		for (const auto& [handle, metadata] : m_Registry)
		{
			if (metadata.Parent.IsValid() && !fileAssets.contains(metadata.Parent))
				orphans.push_back(handle);
		}
		std::sort(orphans.begin(), orphans.end());
		for (uint64_t handle : orphans)
		{
			const uint64_t parent = m_Registry[handle].Parent;
			skip([&] { return fmt::format("entry {}, whose parent {} is not a registered file asset", handle, parent); });
			m_Registry.erase(handle);
		}

		m_SkippedEntries = skipped;
		if (skipped > 0)
		{
			m_RegistryGuard = RegistryGuard::SkippedEntries;
			const bool one = skipped == 1;
			PX_CORE_WARN("Asset registry '{}': skipped {} {} this build cannot read{}. The registry is read-only until the file is fixed and reloaded, since saving "
						 "would drop {}.",
						 ToUtf8(path), skipped, one ? "entry" : "entries", skipped > MaxNamedSkips ? fmt::format(" (the first {} are named above)", MaxNamedSkips) : "",
						 one ? "it" : "them");
		}

		// Several entries for one file are all kept, since scenes may use any of their handles, and saved back as they
		// are. Lookups by path give the lowest (see GetHandleForPath); each such path is reported once, in the order of
		// the UTF-8 text the file stores, so the warnings read the same on every platform.
		std::vector<std::pair<std::string, uint64_t>> fileEntries;
		for (const auto& [handle, metadata] : m_Registry)
		{
			if (!metadata.Parent.IsValid())
				fileEntries.emplace_back(ToUtf8(metadata.FilePath), handle);
		}
		std::sort(fileEntries.begin(), fileEntries.end());
		// A hostile file could list a million paths twice, or one path a million times: a few paths are named, each
		// with a few of its handles, and the rest are counted.
		constexpr size_t MaxNamedPaths = 10;
		constexpr size_t MaxListedHandles = 4;
		size_t duplicatedPaths = 0;
		for (size_t first = 0, end = 0; first < fileEntries.size(); first = end)
		{
			end = first + 1;
			while (end < fileEntries.size() && fileEntries[end].first == fileEntries[first].first)
				end++;
			const size_t count = end - first;
			if (count < 2 || ++duplicatedPaths > MaxNamedPaths)
				continue;
			std::string others;
			for (size_t i = 1; i < count && i <= MaxListedHandles; i++)
				others += fmt::format("{}{}", i > 1 ? ", " : "", fileEntries[first + i].second);
			if (count - 1 > MaxListedHandles)
				others += fmt::format(" and {} more", count - 1 - MaxListedHandles);
			// The path comes from the file: quoted, so it cannot break the log line, and cut short if it is long.
			PX_CORE_WARN("Asset registry '{}' lists {} {} times; looking it up by path gives the lowest handle, {}, not {} (reloads and edits by path reach only "
						 "that one)",
						 ToUtf8(path), QuoteText(fileEntries[first].first), count, fileEntries[first].second, others);
		}
		if (duplicatedPaths > MaxNamedPaths)
			PX_CORE_WARN("Asset registry '{}' lists {} more paths under several handles", ToUtf8(path), duplicatedPaths - MaxNamedPaths);
		return true;
	}

}
