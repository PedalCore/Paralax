#include "pxpch.h"
#include "Paralax/Scene/SceneSerializer.h"

#include "Paralax/Core/JsonText.h"
#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Scene/ComponentRegistry.h"
#include "Paralax/Scene/Entity.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <unordered_map>
#include <fstream>

namespace Paralax {

	using json = nlohmann::json;

	namespace {

		std::string UUIDToString(UUID uuid)
		{
			return std::to_string(static_cast<uint64_t>(uuid));
		}

		bool ParseUUID(const json& value, UUID& out)
		{
			if (value.is_number_unsigned())
			{
				out = UUID(value.get<uint64_t>());
				return true;
			}
			if (!value.is_string())
				return false;
			const std::string& text = value.get_ref<const std::string&>();
			uint64_t parsed = 0;
			auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
			if (ec != std::errc() || ptr != text.data() + text.size() || text.empty())
				return false;
			out = UUID(parsed);
			return true;
		}

	}

	bool WriteFileAtomically(const std::filesystem::path& path, const std::string& contents, size_t maxBytes)
	{
		if (contents.size() > maxBytes)
		{
			PX_CORE_ERROR("Cannot write '{}': the document is too large ({} bytes; the limit is {})", ToUtf8(path), contents.size(), maxBytes);
			return false;
		}
		std::error_code error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), error);

		std::filesystem::path temporary = path;
		temporary += ".tmp";
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				PX_CORE_ERROR("Cannot write '{}'", ToUtf8(temporary));
				return false;
			}
			file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
			file.flush();
			if (!file)
			{
				PX_CORE_ERROR("Failed while writing '{}'", ToUtf8(temporary));
				std::filesystem::remove(temporary, error);
				return false;
			}
		}

		std::filesystem::rename(temporary, path, error);
		if (error)
		{
			PX_CORE_ERROR("Cannot replace '{}': {}", ToUtf8(path), error.message());
			std::filesystem::remove(temporary, error);
			return false;
		}
		return true;
	}

	bool ReadTextFile(const std::filesystem::path& path, std::string& outContents, std::string& outError, size_t maxBytes)
	{
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error)
		{
			outError = error.message();
			return false;
		}
		if (size > maxBytes)
		{
			outError = fmt::format("the file is too large ({} bytes; the limit is {})", size, maxBytes);
			return false;
		}

		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			outError = "the file cannot be opened";
			return false;
		}
		// Read at most the measured size, so a file that grows meanwhile can never exceed the cap; one that shrinks
		// reads short, like any file changed under a reader.
		std::string contents(static_cast<size_t>(size), '\0');
		file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
		contents.resize(static_cast<size_t>(file.gcount()));
		if (file.bad())
		{
			outError = "the file cannot be read";
			return false;
		}
		outContents = std::move(contents);
		return true;
	}

	json SceneSerializer::Serialize(Scene& scene, std::string_view documentType)
	{
		json document;
		document["Paralax"] = {{"Type", documentType}, {"Version", CurrentVersion}};
		document["Name"] = scene.GetName();

		json entities = json::array();
		entt::registry& registry = scene.GetRegistry();
		scene.ForEachEntity([&](Entity entity) {
			json entry;
			entry["ID"] = UUIDToString(entity.GetUUID());
			entry["Parent"] = UUIDToString(entity.GetComponent<RelationshipComponent>().Parent);

			json components = json::object();
			for (const ComponentInfo& info : ComponentRegistry::GetAll())
			{
				if (void* component = info.Get(registry, entity.GetHandle()))
					components[info.Name] = ComponentRegistry::ToJson(info, component);
			}
			entry["Components"] = std::move(components);
			entities.push_back(std::move(entry));
		});
		document["Entities"] = std::move(entities);
		return document;
	}

	bool SceneSerializer::Deserialize(Scene& scene, const json& document, std::string& outError, std::vector<std::string>* outWarnings, std::string_view expectedType)
	{
		auto warn = [&](std::string message) {
			if (outWarnings)
				outWarnings->push_back(std::move(message));
		};
		auto fail = [&](std::string message) {
			outError = std::move(message);
			return false;
		};

		if (scene.GetEntityCount() != 0)
			return fail("Deserialize requires an empty scene");
		if (!document.is_object())
			return fail("Scene document must be a JSON object");

		std::string kind(expectedType); // For messages: "scene", "prefab".
		std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		// Type-checked access only: nlohmann's value() throws on type mismatches, and engine code must not throw.
		const json* header = document.contains("Paralax") ? &document["Paralax"] : nullptr;
		if (!header || !header->is_object() || !header->contains("Type") || !(*header)["Type"].is_string() || (*header)["Type"].get_ref<const std::string&>() != expectedType)
			return fail(fmt::format("Not a Paralax {} (missing \"Paralax\": {{ \"Type\": \"{}\" }})", kind, expectedType));
		const int version = header->contains("Version") && (*header)["Version"].is_number_integer() ? (*header)["Version"].get<int>() : 0;
		if (version < 1 || version > CurrentVersion)
			return fail(fmt::format("Unsupported {} version {} (this build reads 1-{})", kind, version, CurrentVersion));

		if (document.contains("Name") && document["Name"].is_string())
			scene.SetName(document["Name"].get<std::string>());

		if (!document.contains("Entities") || !document["Entities"].is_array())
			return fail("Scene document has no \"Entities\" array");
		const json& entities = document["Entities"];

		// Pass 1: validate IDs and create every entity, so parents can be referenced before they appear.
		std::vector<std::pair<Entity, const json*>> created;
		created.reserve(entities.size());
		for (size_t i = 0; i < entities.size(); i++)
		{
			const json& entry = entities[i];
			UUID uuid = 0;
			if (!entry.is_object() || !entry.contains("ID") || !ParseUUID(entry["ID"], uuid) || !uuid.IsValid())
			{
				warn(fmt::format("Entity #{} has no valid ID and was skipped", i));
				continue;
			}
			if (scene.TryGetEntityByUUID(uuid))
			{
				// A duplicate ID makes references ambiguous; refuse the document rather than guess.
				for (auto& [entity, unused] : created)
					scene.DestroyEntity(entity);
				return fail(fmt::format("Duplicate entity ID {}", static_cast<uint64_t>(uuid)));
			}
			created.emplace_back(scene.CreateEntityWithUUID(uuid), &entry);
		}

		// Pass 2: components.
		entt::registry& registry = scene.GetRegistry();
		for (auto& [entity, entry] : created)
		{
			if (!entry->contains("Components"))
				continue;
			const json& components = (*entry)["Components"];
			if (!components.is_object())
			{
				warn(fmt::format("Entity {}: \"Components\" is not an object", static_cast<uint64_t>(entity.GetUUID())));
				continue;
			}

			for (auto it = components.begin(); it != components.end(); ++it)
			{
				const ComponentInfo* info = ComponentRegistry::Find(it.key());
				if (!info)
				{
					warn(fmt::format("Entity {}: unknown component '{}' skipped", static_cast<uint64_t>(entity.GetUUID()), it.key()));
					continue;
				}
				std::string error;
				if (!ComponentRegistry::FromJson(*info, info->Add(registry, entity.GetHandle()), it.value(), error))
					warn(fmt::format("Entity {}: {}", static_cast<uint64_t>(entity.GetUUID()), error));
			}
		}

		// Names: an empty name is not allowed (the editor and lookups rely on one).
		for (auto& [entity, entry] : created)
		{
			if (entity.GetComponent<TagComponent>().Name.empty())
				entity.GetComponent<TagComponent>().Name = "Entity";
		}

		// Pass 3: hierarchy. Parents are validated for the whole document first (missing parents, self-parenting
		// and cycles are rejected with warnings), then linked in document order, which is sibling order. This keeps
		// loading linear even for very deep hierarchies.
		std::unordered_map<uint64_t, uint64_t> parentOf;
		for (auto& [entity, entry] : created)
		{
			UUID parentID = 0;
			if (entry->contains("Parent") && !ParseUUID((*entry)["Parent"], parentID))
				warn(fmt::format("Entity {}: invalid Parent", static_cast<uint64_t>(entity.GetUUID())));
			if (!parentID.IsValid())
				continue;
			if (!scene.TryGetEntityByUUID(parentID))
			{
				warn(fmt::format("Entity {}: parent {} not found; kept as a root", static_cast<uint64_t>(entity.GetUUID()), static_cast<uint64_t>(parentID)));
				continue;
			}
			parentOf[entity.GetUUID()] = parentID;
		}

		// Cycle detection: walk each chain once, colouring visited entities (0 = new, 1 = on the current path,
		// 2 = known good). Any entity whose chain loops is made a root.
		std::unordered_map<uint64_t, int> state;
		for (auto& [entity, entry] : created)
		{
			std::vector<uint64_t> path;
			uint64_t current = entity.GetUUID();
			while (state[current] == 0)
			{
				state[current] = 1;
				path.push_back(current);
				auto it = parentOf.find(current);
				if (it == parentOf.end())
					break;
				current = it->second;
			}
			if (state[current] == 1 && parentOf.contains(current))
			{
				warn(fmt::format("Entity {}: parent cycle broken; kept as a root", current));
				parentOf.erase(current);
			}
			for (uint64_t id : path)
				state[id] = 2;
		}

		// Rebuild the root list in one pass (document order), linking children as we go: O(n) overall.
		scene.m_RootEntities.clear();
		for (auto& [entity, entry] : created)
		{
			auto it = parentOf.find(entity.GetUUID());
			if (it == parentOf.end())
				scene.m_RootEntities.push_back(entity.GetUUID());
			else
				scene.AttachChildUnchecked(entity, scene.GetEntityByUUID(UUID(it->second)));
		}

		return true;
	}

	bool SceneSerializer::SaveToFile(Scene& scene, const std::filesystem::path& path)
	{
		return WriteFileAtomically(path, FormatJsonDocument(Serialize(scene)), MaxTextFileBytes);
	}

	Ref<Scene> SceneSerializer::LoadFromFile(const std::filesystem::path& path)
	{
		std::string text;
		std::string error;
		if (!ReadTextFile(path, text, error))
		{
			PX_CORE_ERROR("Cannot read scene '{}': {}", ToUtf8(path), error);
			return nullptr;
		}
		if (JsonExceedsDepth(text, MaxJsonDepth))
		{
			PX_CORE_ERROR("Scene '{}' nests too deeply", ToUtf8(path));
			return nullptr;
		}

		json document = json::parse(text, nullptr, false);
		if (document.is_discarded())
		{
			PX_CORE_ERROR("Scene '{}' is not valid JSON", ToUtf8(path));
			return nullptr;
		}

		auto scene = CreateRef<Scene>(ToUtf8(path.stem()));
		std::vector<std::string> warnings;
		if (!Deserialize(*scene, document, error, &warnings))
		{
			PX_CORE_ERROR("Cannot load scene '{}': {}", ToUtf8(path), error);
			return nullptr;
		}
		for (const std::string& warning : warnings)
			PX_CORE_WARN("Scene '{}': {}", ToUtf8(path), warning);
		return scene;
	}

}
