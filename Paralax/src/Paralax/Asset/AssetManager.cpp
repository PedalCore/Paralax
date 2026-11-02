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
				m_Loaded[material->Handle] = material;
			}
			for (const auto& texture : result.Textures)
			{
				m_Registry[texture->Handle] = {texture->Handle, AssetType::Texture, metadata.FilePath, metadata.Handle};
				m_Loaded[texture->Handle] = texture;
			}
			for (const auto& nodeMesh : result.NodeMeshes)
			{
				m_Registry[nodeMesh->Handle] = {nodeMesh->Handle, AssetType::Mesh, metadata.FilePath, metadata.Handle};
				m_Loaded[nodeMesh->Handle] = nodeMesh;
			}
			for (const auto& animation : result.Animations)
			{
				m_Registry[animation.Clip->Handle] = {animation.Clip->Handle, AssetType::AnimationClip, metadata.FilePath, metadata.Handle};
				m_Loaded[animation.Clip->Handle] = animation.Clip;
			}
			m_Loaded[metadata.Handle] = result.Mesh;
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
			m_Loaded[handle] = asset;
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
					m_Loaded[material->Handle] = material;
					m_LastSubAssets.emplace_back(material->Handle, AssetType::Material);
				}
				for (const auto& texture : result.Textures)
				{
					m_Loaded[texture->Handle] = texture;
					m_LastSubAssets.emplace_back(texture->Handle, AssetType::Texture);
				}
				for (const auto& nodeMesh : result.NodeMeshes)
				{
					m_Loaded[nodeMesh->Handle] = nodeMesh;
					m_LastSubAssets.emplace_back(nodeMesh->Handle, AssetType::Mesh);
				}
				for (const auto& animation : result.Animations)
				{
					m_Loaded[animation.Clip->Handle] = animation.Clip;
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
		m_Loaded[handle] = asset;

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
				auto previous = previousSubVersions.find(subHandle);
				m_Loaded[subHandle]->Version = previous != previousSubVersions.end() ? previous->second + 1 : 1;
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
		if (m_RegistryUnreadable)
		{
			// Writing now would replace every entry of the file that failed to load with only those registered since,
			// and every handle the project's scenes use would be lost.
			PX_CORE_ERROR("Not saving the asset registry: '{}' could not be read, and saving would replace all it lists. Repair or restore the file, then reload the "
						  "registry (asset.reload_registry in the editor) or reopen the project.",
						  ToUtf8(path));
			return false;
		}

		// Any other file than the one last read or written was put there by someone else (a version-control checkout or
		// merge, another editor), and this registry, which never saw it, would replace it.
		auto changed = [&] {
			PX_CORE_ERROR("Not saving the asset registry: '{}' has changed on disk since it was loaded or last saved (by a version-control checkout or merge, or "
						  "another editor, say), and saving would replace that change. Reload the registry (asset.reload_registry in the editor) or reopen the "
						  "project, then try again.",
						  ToUtf8(path));
			return false;
		};
		if (!m_RegistryIdentity)
			return changed();
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error)
			return cannotCheck(error.message());
		if (static_cast<uint64_t>(size) != m_RegistryIdentity->Size)
			return changed();
		// The size matches one this manager read or wrote, so it is within m_MaxRegistryBytes.
		std::string text;
		std::string readError;
		if (!ReadTextFile(path, text, readError, m_MaxRegistryBytes))
			return cannotCheck(readError);
		if (IdentifyRegistryText(text) != *m_RegistryIdentity)
			return changed();
		return true;
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

		json document = {{"Paralax", {{"Type", "AssetRegistry"}, {"Version", 1}}}, {"Assets", std::move(assets)}};
		const std::string text = FormatJsonDocument(document);
		if (!WriteFileAtomically(path, text, m_MaxRegistryBytes))
			return false;
		// The file on disk is now one this manager wrote (after an unreadable one was removed, the save that creates it
		// anew), so later saves may replace it as long as it stays as written.
		m_RegistryUnreadable = false;
		m_RegistryIdentity = IdentifyRegistryText(text);
		return true;
	}

	bool AssetManager::LoadRegistry()
	{
		m_Registry.clear();
		m_Failed.clear();
		m_RegistryUnreadable = false;
		m_RegistryIdentity.reset();
		const std::filesystem::path path = m_AssetDirectory / RegistryFileName;
		// A registry that exists but cannot be read stays as it is on disk: SaveRegistry refuses until one loads.
		auto unreadable = [&](const std::string& reason) {
			PX_CORE_ERROR("Asset registry '{}' {}; nothing is registered, and the file is left untouched until it can be read", ToUtf8(path), reason);
			m_RegistryUnreadable = true;
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
		if (document.is_discarded() || !document.is_object() || !document.contains("Assets") || !document["Assets"].is_array())
			return unreadable("is malformed");

		auto stringField = [](const json& entry, const char* name) -> std::string {
			const auto it = entry.find(name);
			return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
		};

		for (const json& entry : document["Assets"])
		{
			AssetMetadata metadata;
			if (!entry.is_object() || !entry.contains("Handle") || !ParseHandle(entry["Handle"], metadata.Handle) || !metadata.Handle.IsValid())
			{
				PX_CORE_WARN("Asset registry: skipping entry without a valid handle");
				continue;
			}
			if (static_cast<uint64_t>(metadata.Handle) < FirstUserHandle)
			{
				// Built-ins own this range and GetAsset resolves them first; a registry entry there would be a
				// different asset depending on which API is asked.
				PX_CORE_WARN("Asset registry: skipping entry {} in the reserved built-in range", static_cast<uint64_t>(metadata.Handle));
				continue;
			}
			metadata.Type = AssetTypeFromString(stringField(entry, "Type"));
			if (entry.contains("Parent") && !ParseHandle(entry["Parent"], metadata.Parent))
				PX_CORE_WARN("Asset registry: invalid parent for {}", static_cast<uint64_t>(metadata.Handle));

			// Registry paths must be relative and stay inside the asset directory.
			const std::filesystem::path declared = FromUtf8(stringField(entry, "Path"));
			const std::optional<std::filesystem::path> contained = declared.empty() || declared.is_absolute() ? std::nullopt : ToRegistryPath(declared);
			if (metadata.Type == AssetType::None || !contained)
			{
				PX_CORE_WARN("Asset registry: skipping malformed or out-of-project entry {}", static_cast<uint64_t>(metadata.Handle));
				continue;
			}
			metadata.FilePath = *contained;
			m_Registry[metadata.Handle] = std::move(metadata);
		}

		// Sub-assets are one level deep: a parent is a registered file asset with no parent of its own. An entry that
		// is its own parent, a cycle or a longer chain would recurse in GetAsset, so such entries are dropped here.
		std::unordered_set<AssetHandle> fileAssets;
		for (const auto& [handle, metadata] : m_Registry)
			if (!metadata.Parent.IsValid())
				fileAssets.insert(handle);
		for (auto it = m_Registry.begin(); it != m_Registry.end();)
		{
			if (!it->second.Parent.IsValid() || fileAssets.contains(it->second.Parent))
				++it;
			else
			{
				PX_CORE_WARN("Asset registry: skipping {} whose parent {} is not a registered file asset", static_cast<uint64_t>(it->first), static_cast<uint64_t>(it->second.Parent));
				it = m_Registry.erase(it);
			}
		}

		// Several entries for one file are all kept, since scenes may use any of their handles, and saved back as they
		// are. Lookups by path give the lowest (see GetHandleForPath); each such path is reported once.
		std::map<std::filesystem::path, std::vector<uint64_t>> handlesByPath;
		for (const auto& [handle, metadata] : m_Registry)
		{
			if (!metadata.Parent.IsValid())
				handlesByPath[metadata.FilePath].push_back(handle);
		}
		for (auto& [registryPath, handles] : handlesByPath)
		{
			if (handles.size() < 2)
				continue;
			std::sort(handles.begin(), handles.end());
			// A hostile file could list one path a million times: the message names a few handles and counts the rest.
			constexpr size_t MaxListed = 4;
			std::string others;
			for (size_t i = 1; i < handles.size() && i <= MaxListed; i++)
				others += fmt::format("{}{}", i > 1 ? ", " : "", handles[i]);
			if (handles.size() - 1 > MaxListed)
				others += fmt::format(" and {} more", handles.size() - 1 - MaxListed);
			PX_CORE_WARN("Asset registry '{}' lists '{}' {} times; looking it up by path gives the lowest handle, {}, not {}", ToUtf8(path), ToUtf8(registryPath), handles.size(),
						 handles.front(), others);
		}
		return true;
	}

}
