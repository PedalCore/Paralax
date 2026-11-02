#include "pxpch.h"
#include "Paralax/Scene/Scene.h"

#include "Paralax/Scene/ComponentRegistry.h"
#include "Paralax/Scene/Entity.h"

#include <algorithm>
#include <unordered_set>

namespace Paralax {

	namespace {

		// RAII marker for "the scene is updating": destruction is deferred until the outermost update finishes.
		class UpdateScope
		{
		public:
			UpdateScope(uint32_t& depth)
				: m_Depth(depth)
			{
				m_Depth++;
			}
			~UpdateScope() { m_Depth--; }

			UpdateScope(const UpdateScope&) = delete;
			UpdateScope& operator=(const UpdateScope&) = delete;

		private:
			uint32_t& m_Depth;
		};

	}

	Scene::Scene(std::string name)
		: m_Name(std::move(name))
	{
	}

	Scene::~Scene()
	{
		// Systems reference the scene (and scripts may still try to reach it while they shut down): stop and release
		// them while every member is still intact.
		OnRuntimeStop();
		m_Systems.clear();
	}

	Ref<Scene> Scene::Copy(const Scene& source)
	{
		auto destination = CreateRef<Scene>(source.m_Name);
		destination->m_ViewportWidth = source.m_ViewportWidth;
		destination->m_ViewportHeight = source.m_ViewportHeight;

		// Recreate every entity with its UUID, then copy all components exactly (typed copies, no JSON). Entities are
		// created in hierarchy order, the order a scene file lists them in, so the copy's registry stores them as a
		// scene loaded from its serialization does. That is the order to match, not the source's own storage: opening
		// the file, the exported runtime and undo, redo and rollback all build the scene from its serialization, and
		// the editor's in-memory order (shuffled by duplicates, destruction and late additions) is never saved.
		// Renderers take "the first" directional or sky light from a view and sum lights in view order, so a playtest
		// must be lit as the shipped game is. Iterative: hierarchies can be very deep.
		const entt::registry& sourceRegistry = source.m_Registry;
		std::vector<UUID> stack(source.m_RootEntities.rbegin(), source.m_RootEntities.rend());
		while (!stack.empty())
		{
			const UUID uuid = stack.back();
			stack.pop_back();
			const auto it = source.m_EntityMap.find(uuid);
			PX_CORE_VERIFY(it != source.m_EntityMap.end(), "Scene::Copy: the hierarchy names an entity {} that does not exist", static_cast<uint64_t>(uuid));
			const entt::entity sourceHandle = it->second;

			Entity copy = destination->CreateEntityWithUUID(uuid);
			copy.GetComponent<RelationshipComponent>() = sourceRegistry.get<RelationshipComponent>(sourceHandle);
			for (const ComponentInfo& info : ComponentRegistry::GetAll())
			{
				if (info.Has(sourceRegistry, sourceHandle))
					info.Copy(sourceRegistry, sourceHandle, destination->m_Registry, copy.GetHandle());
			}

			// Push children in reverse so they are created in their sibling order.
			const std::vector<UUID>& children = sourceRegistry.get<RelationshipComponent>(sourceHandle).Children;
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
		PX_CORE_ASSERT(destination->m_EntityMap.size() == source.m_EntityMap.size(), "Scene::Copy: the hierarchy does not reach every entity");

		destination->m_RootEntities = source.m_RootEntities;
		return destination;
	}

	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
	{
		PX_CORE_VERIFY(uuid.IsValid(), "Cannot create an entity with the null UUID");
		PX_CORE_VERIFY(!m_EntityMap.contains(uuid), "An entity with UUID {} already exists", static_cast<uint64_t>(uuid));

		Entity entity(m_Registry.create(), this);
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<TagComponent>().Name = name.empty() ? "Entity" : name;
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<RelationshipComponent>();

		m_EntityMap[uuid] = entity.GetHandle();
		m_RootEntities.push_back(uuid);
		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		if (!IsValid(entity))
			return;

		if (m_UpdateDepth > 0)
		{
			m_PendingDestruction.push_back(entity.GetUUID());
			return;
		}

		DestroyEntityImmediate(entity.GetHandle());
		// Systems' destruction callbacks may have requested more destruction (deferred while they ran).
		FlushPendingDestruction();
	}

	bool Scene::IsDestructionPending(Entity entity) const
	{
		if (m_PendingDestruction.empty() || !IsValid(entity))
			return false;
		// Iterative walk up: hierarchies can be very deep.
		UUID id = entity.GetUUID();
		while (id.IsValid())
		{
			if (std::find(m_PendingDestruction.begin(), m_PendingDestruction.end(), id) != m_PendingDestruction.end())
				return true;
			auto it = m_EntityMap.find(id);
			if (it == m_EntityMap.end())
				return false;
			id = m_Registry.get<RelationshipComponent>(it->second).Parent;
		}
		return false;
	}

	void Scene::NotifySystemsOfDestruction(entt::entity root)
	{
		// Callbacks run with destruction deferred, but may still reparent entities: one moved into the dying subtree
		// is notified in a further pass (loop until stable); one moved out survives (its systems were notified).
		UpdateScope scope(m_UpdateDepth);
		std::unordered_set<uint64_t> notified;
		for (;;)
		{
			// Pre-order (parents before children), skipping entities already notified.
			std::vector<UUID> pending;
			std::vector<entt::entity> stack = {root};
			while (!stack.empty())
			{
				const entt::entity current = stack.back();
				stack.pop_back();
				const UUID id = m_Registry.get<IDComponent>(current).ID;
				if (!notified.contains(id))
					pending.push_back(id);
				const std::vector<UUID>& children = m_Registry.get<RelationshipComponent>(current).Children;
				for (auto it = children.rbegin(); it != children.rend(); ++it)
				{
					if (auto child = m_EntityMap.find(*it); child != m_EntityMap.end())
						stack.push_back(child->second);
				}
			}
			if (pending.empty())
				return;
			for (UUID uuid : pending)
			{
				notified.insert(uuid);
				for (const Scope<SceneSystem>& system : m_Systems)
				{
					if (auto it = m_EntityMap.find(uuid); it != m_EntityMap.end())
						system->OnEntityDestroying(*this, Entity(it->second, this));
				}
			}
		}
	}

	void Scene::DestroyEntityImmediate(entt::entity handle)
	{
		if (!m_Registry.valid(handle))
			return;
		if (m_IsRunning && !m_Systems.empty())
		{
			NotifySystemsOfDestruction(handle);
			// Callbacks cannot destroy this subtree (their destruction is deferred), but check anyway.
			if (!m_Registry.valid(handle))
				return;
		}

		// Detach the subtree root from its parent, then destroy the whole subtree iteratively (deep hierarchies
		// must not overflow the stack).
		DetachFromParent(handle);
		std::erase(m_RootEntities, m_Registry.get<IDComponent>(handle).ID);

		std::vector<entt::entity> stack = {handle};
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			for (UUID child : m_Registry.get<RelationshipComponent>(current).Children)
			{
				if (auto it = m_EntityMap.find(child); it != m_EntityMap.end())
					stack.push_back(it->second);
			}
			m_EntityMap.erase(m_Registry.get<IDComponent>(current).ID);
			m_Registry.destroy(current);
		}
	}

	void Scene::FlushPendingDestruction()
	{
		// Destroying may queue more (e.g. via callbacks in future systems); loop until stable.
		while (!m_PendingDestruction.empty())
		{
			std::vector<UUID> pending;
			pending.swap(m_PendingDestruction);
			for (UUID uuid : pending)
			{
				if (auto it = m_EntityMap.find(uuid); it != m_EntityMap.end())
					DestroyEntityImmediate(it->second);
			}
		}
	}

	Entity Scene::DuplicateEntity(Entity entity)
	{
		PX_CORE_VERIFY(IsValid(entity), "Cannot duplicate an invalid entity");
		const UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
		Entity copy = CopySubtree(*this, entity.GetHandle(), parent);

		// Place the copy right after the original among its siblings.
		std::vector<UUID>& siblings = GetSiblingList(parent);
		std::erase(siblings, copy.GetUUID());
		auto it = std::find(siblings.begin(), siblings.end(), entity.GetUUID());
		siblings.insert(it == siblings.end() ? siblings.end() : it + 1, copy.GetUUID());
		return copy;
	}

	Entity Scene::CopySubtree(Scene& source, entt::entity sourceRoot, UUID newParent)
	{
		// Iterative pre-order copy: (source entity, parent UUID of its copy).
		const entt::registry& sourceRegistry = source.m_Registry;
		Entity rootCopy;
		std::vector<std::pair<entt::entity, UUID>> stack = {{sourceRoot, newParent}};
		while (!stack.empty())
		{
			const auto [sourceHandle, parent] = stack.back();
			stack.pop_back();

			Entity copy = CreateEntity();
			for (const ComponentInfo& info : ComponentRegistry::GetAll())
			{
				if (info.Has(sourceRegistry, sourceHandle))
					info.Copy(sourceRegistry, sourceHandle, m_Registry, copy.GetHandle());
			}
			if (parent.IsValid())
			{
				std::erase(m_RootEntities, copy.GetUUID());
				copy.GetComponent<RelationshipComponent>().Parent = parent;
				GetSiblingList(parent).push_back(copy.GetUUID());
			}
			if (!rootCopy)
				rootCopy = copy;

			// Push children in reverse so they are copied (and appended) in their original order.
			const std::vector<UUID>& children = sourceRegistry.get<RelationshipComponent>(sourceHandle).Children;
			for (auto it = children.rbegin(); it != children.rend(); ++it)
				stack.emplace_back(source.m_EntityMap.at(*it), copy.GetUUID());
		}
		return rootCopy;
	}

	Entity Scene::CopySubtreeFrom(Scene& source, Entity sourceRoot)
	{
		PX_CORE_VERIFY(source.IsValid(sourceRoot), "CopySubtreeFrom needs a valid source entity");
		return CopySubtree(source, sourceRoot.GetHandle(), UUID(0));
	}

	Entity Scene::CopySubtreeFrom(Scene& source, Entity sourceRoot, Entity parent)
	{
		PX_CORE_VERIFY(source.IsValid(sourceRoot), "CopySubtreeFrom needs a valid source entity");
		PX_CORE_VERIFY(IsValid(parent), "CopySubtreeFrom: the parent is not an entity of this scene");
		return CopySubtree(source, sourceRoot.GetHandle(), parent.GetUUID());
	}

	Entity Scene::GetEntityByUUID(UUID uuid)
	{
		auto it = m_EntityMap.find(uuid);
		PX_CORE_VERIFY(it != m_EntityMap.end(), "No entity with UUID {}", static_cast<uint64_t>(uuid));
		return {it->second, this};
	}

	std::optional<Entity> Scene::TryGetEntityByUUID(UUID uuid)
	{
		auto it = m_EntityMap.find(uuid);
		if (it == m_EntityMap.end())
			return std::nullopt;
		return Entity(it->second, this);
	}

	std::optional<Entity> Scene::FindEntityByName(std::string_view name)
	{
		std::optional<Entity> result;
		ForEachEntity([&](Entity entity) {
			if (!result && entity.GetName() == name)
				result = entity;
		});
		return result;
	}

	std::vector<Entity> Scene::FindEntitiesByTag(std::string_view tag)
	{
		std::vector<Entity> result;
		ForEachEntity([&](Entity entity) {
			if (entity.GetComponent<TagComponent>().Tag == tag)
				result.push_back(entity);
		});
		return result;
	}

	bool Scene::IsValid(Entity entity) const
	{
		return entity && entity.GetScene() == this && m_Registry.valid(entity.GetHandle());
	}

	std::vector<UUID>& Scene::GetSiblingList(UUID parent)
	{
		if (!parent.IsValid())
			return m_RootEntities;
		return GetEntityByUUID(parent).GetComponent<RelationshipComponent>().Children;
	}

	void Scene::DetachFromParent(entt::entity handle)
	{
		auto& relationship = m_Registry.get<RelationshipComponent>(handle);
		if (!relationship.Parent.IsValid())
			return;

		if (auto parent = TryGetEntityByUUID(relationship.Parent))
			std::erase(parent->GetComponent<RelationshipComponent>().Children, m_Registry.get<IDComponent>(handle).ID);
		relationship.Parent = 0;
	}

	bool Scene::IsAncestorOf(Entity ancestor, Entity entity)
	{
		if (!IsValid(ancestor) || !IsValid(entity))
			return false;

		UUID current = entity.GetComponent<RelationshipComponent>().Parent;
		const UUID target = ancestor.GetUUID();
		while (current.IsValid())
		{
			if (current == target)
				return true;
			current = GetEntityByUUID(current).GetComponent<RelationshipComponent>().Parent;
		}
		return false;
	}

	bool Scene::SetParent(Entity child, std::optional<Entity> parent, bool keepWorldTransform)
	{
		PX_CORE_VERIFY(IsValid(child), "SetParent: invalid child");
		if (parent && !IsValid(*parent))
		{
			PX_CORE_ERROR("SetParent: invalid parent entity");
			return false;
		}
		if (parent && (*parent == child || IsAncestorOf(child, *parent)))
		{
			PX_CORE_ERROR("SetParent: cannot parent '{}' to itself or one of its descendants", child.GetName());
			return false;
		}

		const glm::mat4 world = keepWorldTransform ? GetWorldTransform(child) : glm::mat4(1.0f);
		const UUID childID = child.GetUUID();

		auto& relationship = child.GetComponent<RelationshipComponent>();
		if (relationship.Parent.IsValid())
			DetachFromParent(child.GetHandle());
		else
			std::erase(m_RootEntities, childID);

		if (parent)
		{
			relationship.Parent = parent->GetUUID();
			parent->GetComponent<RelationshipComponent>().Children.push_back(childID);
		}
		else
		{
			m_RootEntities.push_back(childID);
		}

		if (keepWorldTransform)
			SetWorldTransform(child, world);
		return true;
	}

	void Scene::AttachChildUnchecked(Entity child, Entity parent)
	{
		const UUID childID = child.GetUUID();
		child.GetComponent<RelationshipComponent>().Parent = parent.GetUUID();
		parent.GetComponent<RelationshipComponent>().Children.push_back(childID);
	}

	std::optional<Entity> Scene::GetParent(Entity entity)
	{
		const UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
		if (!parent.IsValid())
			return std::nullopt;
		return GetEntityByUUID(parent);
	}

	void Scene::SetSiblingIndex(Entity entity, size_t index)
	{
		PX_CORE_VERIFY(IsValid(entity), "SetSiblingIndex: invalid entity");
		std::vector<UUID>& siblings = GetSiblingList(entity.GetComponent<RelationshipComponent>().Parent);
		const UUID uuid = entity.GetUUID();
		std::erase(siblings, uuid);
		index = std::min(index, siblings.size());
		siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(index), uuid);
	}

	void Scene::ForEachEntity(const std::function<void(Entity)>& visitor)
	{
		// Iterative depth-first walk over a snapshot of each ID list, so visitors may modify the hierarchy (entities
		// destroyed before being reached are skipped; new entities are not visited this pass).
		std::vector<UUID> stack(m_RootEntities.rbegin(), m_RootEntities.rend());
		while (!stack.empty())
		{
			const UUID uuid = stack.back();
			stack.pop_back();
			std::optional<Entity> entity = TryGetEntityByUUID(uuid);
			if (!entity)
				continue;
			visitor(*entity);
			if (IsValid(*entity))
			{
				const std::vector<UUID>& children = entity->GetComponent<RelationshipComponent>().Children;
				stack.insert(stack.end(), children.rbegin(), children.rend());
			}
		}
	}

	glm::mat4 Scene::GetWorldTransform(Entity entity)
	{
		glm::mat4 transform = entity.GetComponent<TransformComponent>().GetLocalMatrix();
		UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
		while (parent.IsValid())
		{
			Entity parentEntity = GetEntityByUUID(parent);
			transform = parentEntity.GetComponent<TransformComponent>().GetLocalMatrix() * transform;
			parent = parentEntity.GetComponent<RelationshipComponent>().Parent;
		}
		return transform;
	}

	void Scene::SetWorldTransform(Entity entity, const glm::mat4& worldTransform)
	{
		glm::mat4 local = worldTransform;
		if (std::optional<Entity> parent = GetParent(entity))
			local = glm::inverse(GetWorldTransform(*parent)) * worldTransform;

		auto& transform = entity.GetComponent<TransformComponent>();
		if (!Math::DecomposeTransform(local, transform.Translation, transform.Rotation, transform.Scale))
			PX_CORE_WARN("SetWorldTransform: degenerate transform for '{}' ignored", entity.GetName());
	}

	std::optional<Entity> Scene::GetPrimaryCameraEntity()
	{
		std::optional<Entity> result;
		ForEachEntity([&](Entity entity) {
			if (result)
				return;
			if (const auto* camera = entity.TryGetComponent<CameraComponent>(); camera && camera->Primary)
				result = entity;
		});
		return result;
	}

	SceneSystem* Scene::AddSystem(Scope<SceneSystem> system)
	{
		PX_CORE_VERIFY(system != nullptr, "AddSystem needs a system");
		PX_CORE_VERIFY(!m_IsRunning, "Systems must be added before the scene starts running");
		return m_Systems.emplace_back(std::move(system)).get();
	}

	void Scene::OnRuntimeStart()
	{
		if (m_IsRunning)
			return;
		m_IsRunning = true;
		{
			UpdateScope scope(m_UpdateDepth);
			for (const Scope<SceneSystem>& system : m_Systems)
				system->OnRuntimeStart(*this);
		}
		if (m_UpdateDepth == 0)
			FlushPendingDestruction();
	}

	void Scene::OnRuntimeStop()
	{
		if (!m_IsRunning)
			return;
		{
			UpdateScope scope(m_UpdateDepth);
			// Reverse order: later systems may depend on earlier ones.
			for (auto it = m_Systems.rbegin(); it != m_Systems.rend(); ++it)
				(*it)->OnRuntimeStop(*this);
		}
		// Entities destroyed during shutdown are removed without further callbacks.
		m_IsRunning = false;
		if (m_UpdateDepth == 0)
			FlushPendingDestruction();
	}

	void Scene::OnUpdateRuntime(Timestep ts)
	{
		if (!m_IsRunning)
			return;
		{
			UpdateScope scope(m_UpdateDepth);
			for (const Scope<SceneSystem>& system : m_Systems)
				system->OnUpdate(*this, ts);
		}

		if (m_UpdateDepth == 0)
			FlushPendingDestruction();
	}

	void Scene::OnFixedUpdateRuntime(Timestep step)
	{
		PX_CORE_ASSERT(!m_InFixedUpdate, "Scene::OnFixedUpdateRuntime called from a system's OnFixedUpdate");
		if (!m_IsRunning || m_InFixedUpdate)
			return;
		{
			// Cleared on every exit, including an assertion thrown in tests.
			struct FixedUpdateScope
			{
				explicit FixedUpdateScope(bool& flag)
					: Flag(flag)
				{
					Flag = true;
				}
				~FixedUpdateScope() { Flag = false; }
				bool& Flag;
			} fixedScope(m_InFixedUpdate);
			UpdateScope scope(m_UpdateDepth);
			for (const Scope<SceneSystem>& system : m_Systems)
				system->OnFixedUpdate(*this, step);
		}
		if (m_UpdateDepth == 0)
			FlushPendingDestruction();
	}

	bool Scene::WantsFixedUpdate() const
	{
		return m_IsRunning && std::any_of(m_Systems.begin(), m_Systems.end(), [](const Scope<SceneSystem>& system) { return system->WantsFixedUpdate(); });
	}

	void Scene::SetViewportSize(uint32_t width, uint32_t height)
	{
		m_ViewportWidth = width;
		m_ViewportHeight = height;
	}

}
