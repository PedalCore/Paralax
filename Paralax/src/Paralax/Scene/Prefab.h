#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Scene.h"
#include "Paralax/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace Paralax {

	// A reusable entity subtree (".pxprefab"). The prefab owns a private template scene holding exactly one root
	// entity and its descendants; instantiating copies that subtree into a scene with new UUIDs and marks the copy's
	// root with a PrefabInstanceComponent. Main-thread-only.
	//
	// File format: a scene document (see SceneSerializer) whose header type is "Prefab" and that has exactly one
	// root entity.
	class PrefabAsset : public Asset
	{
	public:
		// The cap shared by every text document; also what the editor checks before it writes a prefab.
		static constexpr size_t MaxFileBytes = MaxTextFileBytes;

		AssetType GetType() const override { return AssetType::Prefab; }

		// Captures a copy of an entity and its descendants. Verifies the entity is valid in the scene.
		static Ref<PrefabAsset> CreateFromEntity(Scene& scene, Entity entity);
		// Returns nullptr (with an error logged) if the file is missing, too large, malformed, or does not have
		// exactly one root entity.
		static Ref<PrefabAsset> LoadFromFile(const std::filesystem::path& path);
		// Parses a prefab document. Returns nullptr and sets outError on failure.
		static Ref<PrefabAsset> FromJson(const nlohmann::json& document, std::string& outError);

		nlohmann::json ToJson() const;
		// Writes atomically. Returns false (with an error logged) on failure.
		bool SaveToFile(const std::filesystem::path& path) const;

		// Copies the prefab into a scene (as the last child of parent, or the last root) and returns the new root.
		// If position is set, it replaces the root's local translation.
		Entity Instantiate(Scene& scene, std::optional<Entity> parent = std::nullopt, std::optional<glm::vec3> position = std::nullopt) const;

		// Number of entities in the prefab (root and descendants).
		size_t GetEntityCount() const { return m_Template->GetEntityCount(); }
		const std::string& GetRootName() const;

	private:
		PrefabAsset();
		Entity GetRoot() const;

	private:
		Ref<Scene> m_Template;
	};

}
