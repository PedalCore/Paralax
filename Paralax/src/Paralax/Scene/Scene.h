#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Core/Timestep.h"
#include "Paralax/Core/UUID.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Paralax {

	class Entity;

	// A collection of entities (an entt registry) plus the scene hierarchy. Main-thread-only.
	//
	// Hierarchy: every entity has a RelationshipComponent; root entities are kept in a stable, user-defined order
	// (GetRootEntities), children in RelationshipComponent::Children order. Serialization follows this order.
	//
	// Destruction requested while the scene is being updated (e.g. from a script) is deferred to the end of the
	// update, so systems never observe a half-destroyed entity.
	class Scene
	{
	public:
		explicit Scene(std::string name = "Untitled");
		~Scene();

		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		// Deep copy: same entities (same UUIDs), components and hierarchy. Used to enter play mode.
		static Ref<Scene> Copy(const Scene& source);

		Entity CreateEntity(const std::string& name = "Entity");
		// Creates an entity with a specific UUID. Verifies the UUID is valid and unused.
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = "Entity");
		// Destroys the entity and its descendants (deferred while updating). Unknown or null entities are ignored.
		void DestroyEntity(Entity entity);
		// Creates a copy of the entity (and descendants) with new UUIDs, as a sibling after the original.
		Entity DuplicateEntity(Entity entity);

		Entity GetEntityByUUID(UUID uuid);
		std::optional<Entity> TryGetEntityByUUID(UUID uuid);
		// First entity (in hierarchy order) with this name, if any.
		std::optional<Entity> FindEntityByName(std::string_view name);
		// All entities with this tag, in hierarchy order.
		std::vector<Entity> FindEntitiesByTag(std::string_view tag);
		bool IsValid(Entity entity) const;
		size_t GetEntityCount() const { return m_EntityMap.size(); }

		// Hierarchy. SetParent keeps the child's world transform unless keepWorldTransform is false. Parenting an
		// entity to itself or to one of its descendants is rejected (returns false, logs an error).
		bool SetParent(Entity child, std::optional<Entity> parent, bool keepWorldTransform = true);
		std::optional<Entity> GetParent(Entity entity);
		const std::vector<UUID>& GetRootEntities() const { return m_RootEntities; }
		// Moves an entity to a new position among its siblings (roots or parent's children). Index is clamped.
		void SetSiblingIndex(Entity entity, size_t index);
		bool IsAncestorOf(Entity ancestor, Entity entity);

		// Visits entities depth-first in hierarchy order.
		void ForEachEntity(const std::function<void(Entity)>& visitor);

		glm::mat4 GetWorldTransform(Entity entity);
		// Sets the local transform so the entity ends up at the given world transform.
		void SetWorldTransform(Entity entity, const glm::mat4& worldTransform);

		// The primary camera entity, if any (first in hierarchy order with CameraComponent::Primary).
		std::optional<Entity> GetPrimaryCameraEntity();

		// Runtime lifecycle (play mode). OnRuntimeStart/Stop bracket simulation; OnUpdateRuntime advances it.
		void OnRuntimeStart();
		void OnRuntimeStop();
		void OnUpdateRuntime(Timestep ts);
		bool IsRunning() const { return m_IsRunning; }

		void SetViewportSize(uint32_t width, uint32_t height);
		uint32_t GetViewportWidth() const { return m_ViewportWidth; }
		uint32_t GetViewportHeight() const { return m_ViewportHeight; }

		const std::string& GetName() const { return m_Name; }
		void SetName(std::string name) { m_Name = std::move(name); }

		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<Components...>();
		}

	private:
		void DestroyEntityImmediate(entt::entity handle);
		void FlushPendingDestruction();
		std::vector<UUID>& GetSiblingList(UUID parent);
		void DetachFromParent(entt::entity handle);
		Entity DuplicateRecursive(Entity source, UUID newParent);
		// Links an entity under a parent without cycle checks, transform changes or root-list maintenance. Used by
		// SceneSerializer, which validates the whole hierarchy and rebuilds the root list itself (keeps loading O(n)).
		void AttachChildUnchecked(Entity child, Entity parent);

	private:
		std::string m_Name;
		entt::registry m_Registry;
		std::unordered_map<uint64_t, entt::entity> m_EntityMap;
		std::vector<UUID> m_RootEntities;

		bool m_IsRunning = false;
		uint32_t m_UpdateDepth = 0;
		std::vector<UUID> m_PendingDestruction;

		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;

		friend class Entity;
		friend class SceneSerializer;
	};

}
