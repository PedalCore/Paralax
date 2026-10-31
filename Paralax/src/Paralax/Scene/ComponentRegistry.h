#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Core/Reflection.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Paralax {

	// Type-erased description of a component type.
	struct ComponentInfo
	{
		std::string Name; // Stable serialized/scripting name, e.g. "Transform".
		std::string Description;
		std::vector<FieldInfo> Fields;
		// False for components managed by the engine (users cannot add/remove them).
		bool UserEditable = true;

		bool (*Has)(const entt::registry&, entt::entity) = nullptr;
		void* (*Get)(entt::registry&, entt::entity) = nullptr; // nullptr if absent
		void* (*Add)(entt::registry&, entt::entity) = nullptr; // default-constructs; returns existing if present
		// nullptr for components that are not UserEditable (they cannot be removed).
		void (*Remove)(entt::registry&, entt::entity) = nullptr;
		// Copies the component (exactly, without JSON round-tripping) from one entity to another, replacing any
		// existing instance on the destination.
		void (*Copy)(const entt::registry& source, entt::entity from, entt::registry& destination, entt::entity to) = nullptr;

		const FieldInfo* FindField(std::string_view name) const;
	};

	// Registry of all reflected component types, in a stable order (serialization and inspector order).
	class ComponentRegistry
	{
	public:
		static std::span<const ComponentInfo> GetAll();
		static const ComponentInfo* Find(std::string_view name);

		// Reads a field as JSON.
		static nlohmann::json GetField(const FieldInfo& field, const void* component);
		// Writes a field from JSON. Returns false (and sets outError) if the value has the wrong shape, is out of
		// range or names an unknown enum value; the field is left unchanged in that case.
		static bool SetField(const FieldInfo& field, void* component, const nlohmann::json& value, std::string& outError);

		// Whole component <-> JSON object of fields. FromJson ignores missing fields (keeping current values) and
		// reports unknown or invalid ones via outError (but still applies the valid ones).
		static nlohmann::json ToJson(const ComponentInfo& info, const void* component);
		static bool FromJson(const ComponentInfo& info, void* component, const nlohmann::json& object, std::string& outError);
	};

}
