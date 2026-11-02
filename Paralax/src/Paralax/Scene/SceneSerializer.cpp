#include "pxpch.h"
#include "Paralax/Scene/SceneSerializer.h"

#include "Paralax/Core/JsonText.h"
#include "Paralax/Scene/ComponentRegistry.h"
#include "Paralax/Scene/Entity.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <unordered_map>
#include <fstream>
#include <sstream>

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

	bool WriteFileAtomically(const std::filesystem::path& path, const std::string& contents)
	{
		std::error_code error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), error);

		std::filesystem::path temporary = path;
		temporary += ".tmp";
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				PX_CORE_ERROR("Cannot write '{}'", temporary.string());
				return false;
			}
			file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
			file.flush();
			if (!file)
			{
				PX_CORE_ERROR("Failed while writing '{}'", temporary.string());
				std::filesystem::remove(temporary, error);
				return false;
			}
		}

		std::filesystem::rename(temporary, path, error);
		if (error)
		{
			PX_CORE_ERROR("Cannot replace '{}': {}", path.string(), error.message());
			std::filesystem::remove(temporary, error);
			return false;
		}
		return true;
	}

	bool ReadTextFile(const std::filesystem::path& path, std::string& outContents)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return false;
		std::ostringstream stream;
		stream << file.rdbuf();
		outContents = stream.str();
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
		return WriteFileAtomically(path, Serialize(scene).dump(1, '\t') + "\n");
	}

	Ref<Scene> SceneSerializer::LoadFromFile(const std::filesystem::path& path)
	{
		std::string text;
		if (!ReadTextFile(path, text))
		{
			PX_CORE_ERROR("Cannot open scene '{}'", path.string());
			return nullptr;
		}
		if (JsonExceedsDepth(text, MaxJsonDepth))
		{
			PX_CORE_ERROR("Scene '{}' nests too deeply", path.string());
			return nullptr;
		}

		json document = json::parse(text, nullptr, false);
		if (document.is_discarded())
		{
			PX_CORE_ERROR("Scene '{}' is not valid JSON", path.string());
			return nullptr;
		}

		auto scene = CreateRef<Scene>(path.stem().string());
		std::string error;
		std::vector<std::string> warnings;
		if (!Deserialize(*scene, document, error, &warnings))
		{
			PX_CORE_ERROR("Cannot load scene '{}': {}", path.string(), error);
			return nullptr;
		}
		for (const std::string& warning : warnings)
			PX_CORE_WARN("Scene '{}': {}", path.string(), warning);
		return scene;
	}

}
