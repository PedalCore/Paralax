#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Core/Timestep.h"
#include "Paralax/Core/UUID.h"
#include "Paralax/Scene/SceneSystem.h"

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

		// Deep copy: same entities (same UUIDs), components and hierarchy. Used to enter play mode. Systems and the
		// entity id sequence are not copied (they belong to a running scene).
		static Ref<Scene> Copy(const Scene& source);

		// Creates a root entity with a new id (see SetEntityIdSequence).
		Entity CreateEntity(const std::string& name = "Entity");
		// Creates an entity with a specific UUID. Verifies the UUID is valid and unused.
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = "Entity");
		// Destroys the entity and its descendants (deferred while updating). Unknown or null entities are ignored.
		void DestroyEntity(Entity entity);
		// Whether the entity, or one of its ancestors, is to be destroyed when the current update finishes. Systems
		// that run several times per update (fixed updates) skip such entities. O(depth); false for invalid entities.
		bool IsDestructionPending(Entity entity) const;
		// Creates a copy of the entity (and descendants) with new UUIDs, as a sibling after the original.
		Entity DuplicateEntity(Entity entity);
		// Copies an entity subtree from another scene into this one with new UUIDs, as the last root (or the last
		// child of parent). Verifies the entities are valid. Used to instantiate prefabs.
		Entity CopySubtreeFrom(Scene& source, Entity sourceRoot);
		Entity CopySubtreeFrom(Scene& source, Entity sourceRoot, Entity parent);

		// Where CreateEntity (and so DuplicateEntity and CopySubtreeFrom) gets new ids. Without a sequence (the default,
		// as in the editor) each id is a random UUID, so entities saved to files stay unique across sessions. With one,
		// ids are taken from it in order, so a game that spawns entities gets the same ids in the same order every run
		// (GameSession gives the scene it plays one). Either way an id already used in this scene is skipped, which
		// consumes it from the sequence like any other, so creation never clashes and stays reproducible. nullopt
		// returns to random ids.
		void SetEntityIdSequence(std::optional<UUIDSequence> sequence) { m_EntityIdSequence = sequence; }
		// The sequence in its current state (its next value is the next id tried), or nullopt for random ids.
		const std::optional<UUIDSequence>& GetEntityIdSequence() const { return m_EntityIdSequence; }

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

		// Runtime lifecycle (play mode). OnRuntimeStart/Stop bracket simulation; OnUpdateRuntime advances it by
		// running each system in the order added. Starting a running scene or stopping a stopped one is ignored.
		void OnRuntimeStart();
		void OnRuntimeStop();
		void OnUpdateRuntime(Timestep ts);
		// Runs every system's OnFixedUpdate, in the order added; ignored when the scene is not running. Called by the
		// system that owns the fixed clock (the PhysicsSystem) before each of its steps, from inside its OnUpdate, so
		// destruction requested meanwhile waits for the update to finish. Asserts it is not called from inside itself.
		void OnFixedUpdateRuntime(Timestep step);
		// Whether any system wants OnFixedUpdate calls now (SceneSystem::WantsFixedUpdate). Without any, the fixed
		// clock's owner may skip the per-step work that only those calls need.
		bool WantsFixedUpdate() const;
		bool IsRunning() const { return m_IsRunning; }

		// Attaches a runtime system (scripting, physics...). Verifies the scene is not running yet. Returns a
		// non-owning pointer to the system.
		SceneSystem* AddSystem(Scope<SceneSystem> system);
		template<typename T>
		T* GetSystem() const
		{
			for (const Scope<SceneSystem>& system : m_Systems)
			{
				if (T* typed = dynamic_cast<T*>(system.get()))
					return typed;
			}
			return nullptr;
		}

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
		// A new entity id: from the sequence if there is one, else random; never 0 or an id already in this scene.
		UUID GenerateEntityId();
		void DestroyEntityImmediate(entt::entity handle);
		void NotifySystemsOfDestruction(entt::entity root);
		void FlushPendingDestruction();
		std::vector<UUID>& GetSiblingList(UUID parent);
		void DetachFromParent(entt::entity handle);
		// Copies a subtree from source (which may be this scene) with new UUIDs; children are linked in order. The
		// root copy is linked under newParent only if it is valid; otherwise it stays a root (appended).
		Entity CopySubtree(Scene& source, entt::entity sourceRoot, UUID newParent);
		// Links an entity under a parent without cycle checks, transform changes or root-list maintenance. Used by
		// SceneSerializer, which validates the whole hierarchy and rebuilds the root list itself (keeps loading O(n)).
		void AttachChildUnchecked(Entity child, Entity parent);

	private:
		std::string m_Name;
		entt::registry m_Registry;
		std::unordered_map<uint64_t, entt::entity> m_EntityMap;
		std::vector<UUID> m_RootEntities;
		std::optional<UUIDSequence> m_EntityIdSequence;

		bool m_IsRunning = false;
		uint32_t m_UpdateDepth = 0;
		bool m_InFixedUpdate = false;
		std::vector<Scope<SceneSystem>> m_Systems;
		std::vector<UUID> m_PendingDestruction;

		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;

		friend class Entity;
		friend class SceneSerializer;
	};

}
