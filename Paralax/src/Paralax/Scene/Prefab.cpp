#include "pxpch.h"
#include "Paralax/Scene/Prefab.h"

#include "Paralax/Core/JsonText.h"
#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/SceneSerializer.h"

namespace Paralax {

	PrefabAsset::PrefabAsset()
		: m_Template(CreateRef<Scene>("Prefab"))
	{
	}

	Ref<PrefabAsset> PrefabAsset::CreateFromEntity(Scene& scene, Entity entity)
	{
		PX_CORE_VERIFY(scene.IsValid(entity), "CreateFromEntity needs a valid entity");
		Ref<PrefabAsset> prefab(new PrefabAsset());
		Entity root = prefab->m_Template->CopySubtreeFrom(scene, entity);
		// The template is not an instance of anything: instances get their own marker when instantiated.
		if (root.HasComponent<PrefabInstanceComponent>())
			root.RemoveComponent<PrefabInstanceComponent>();
		return prefab;
	}

	Ref<PrefabAsset> PrefabAsset::FromJson(const nlohmann::json& document, std::string& outError)
	{
		Ref<PrefabAsset> prefab(new PrefabAsset());
		std::vector<std::string> warnings;
		if (!SceneSerializer::Deserialize(*prefab->m_Template, document, outError, &warnings, "Prefab"))
			return nullptr;
		for (const std::string& warning : warnings)
			PX_CORE_WARN("Prefab: {}", warning);
		if (prefab->m_Template->GetRootEntities().size() != 1)
		{
			outError = fmt::format("A prefab must have exactly one root entity (found {})", prefab->m_Template->GetRootEntities().size());
			return nullptr;
		}
		return prefab;
	}

	Ref<PrefabAsset> PrefabAsset::LoadFromFile(const std::filesystem::path& path)
	{
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error || size > MaxFileBytes)
		{
			PX_CORE_ERROR("Cannot load prefab '{}': {}", ToUtf8(path), error ? error.message() : "the file is too large");
			return nullptr;
		}
		std::string text;
		if (!ReadTextFile(path, text))
		{
			PX_CORE_ERROR("Cannot open prefab '{}'", ToUtf8(path));
			return nullptr;
		}
		if (JsonExceedsDepth(text, MaxJsonDepth))
		{
			PX_CORE_ERROR("Prefab '{}' nests too deeply", ToUtf8(path));
			return nullptr;
		}
		const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
		if (document.is_discarded())
		{
			PX_CORE_ERROR("Prefab '{}' is not valid JSON", ToUtf8(path));
			return nullptr;
		}
		std::string message;
		Ref<PrefabAsset> prefab = FromJson(document, message);
		if (!prefab)
			PX_CORE_ERROR("Cannot load prefab '{}': {}", ToUtf8(path), message);
		return prefab;
	}

	nlohmann::json PrefabAsset::ToJson() const
	{
		return SceneSerializer::Serialize(*m_Template, "Prefab");
	}

	bool PrefabAsset::SaveToFile(const std::filesystem::path& path) const
	{
		return WriteFileAtomically(path, FormatJsonDocument(ToJson()));
	}

	Entity PrefabAsset::GetRoot() const
	{
		return m_Template->GetEntityByUUID(m_Template->GetRootEntities().front());
	}

	const std::string& PrefabAsset::GetRootName() const
	{
		return GetRoot().GetName();
	}

	Entity PrefabAsset::Instantiate(Scene& scene, std::optional<Entity> parent, std::optional<glm::vec3> position) const
	{
		Entity instance = parent ? scene.CopySubtreeFrom(*m_Template, GetRoot(), *parent) : scene.CopySubtreeFrom(*m_Template, GetRoot());
		instance.AddOrReplaceComponent<PrefabInstanceComponent>().Prefab = Handle;
		if (position)
			instance.GetComponent<TransformComponent>().Translation = *position;
		return instance;
	}

}
