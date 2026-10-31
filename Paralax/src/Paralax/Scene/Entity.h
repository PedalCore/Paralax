#pragma once

#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/Scene.h"

#include <entt/entt.hpp>

namespace Paralax {

	// Lightweight, copyable handle to an entity in a Scene. A default-constructed Entity is null. The handle does
	// not keep the scene alive; check Scene::IsValid before using a handle that may be stale.
	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene)
			: m_Handle(handle)
			, m_Scene(scene)
		{
		}

		// Adds a component (verifies it is not already present) and returns it.
		template<typename T, typename... Args>
		T& AddComponent(Args&&... args)
		{
			PX_CORE_VERIFY(!HasComponent<T>(), "Entity already has component {}", typeid(T).name());
			return Registry().template emplace<T>(m_Handle, std::forward<Args>(args)...);
		}

		template<typename T, typename... Args>
		T& AddOrReplaceComponent(Args&&... args)
		{
			return Registry().template emplace_or_replace<T>(m_Handle, std::forward<Args>(args)...);
		}

		// Returns the component (verifies it is present).
		template<typename T>
		T& GetComponent() const
		{
			PX_CORE_VERIFY(HasComponent<T>(), "Entity does not have component {}", typeid(T).name());
			return Registry().template get<T>(m_Handle);
		}

		template<typename T>
		T* TryGetComponent() const
		{
			return Registry().template try_get<T>(m_Handle);
		}

		template<typename... T>
		bool HasComponent() const
		{
			return Registry().template all_of<T...>(m_Handle);
		}

		template<typename T>
		void RemoveComponent()
		{
			PX_CORE_VERIFY(HasComponent<T>(), "Entity does not have component {}", typeid(T).name());
			static_assert(!std::is_same_v<T, IDComponent> && !std::is_same_v<T, RelationshipComponent> && !std::is_same_v<T, TransformComponent> && !std::is_same_v<T, TagComponent>,
						  "Core components cannot be removed");
			Registry().template remove<T>(m_Handle);
		}

		UUID GetUUID() const { return GetComponent<IDComponent>().ID; }
		const std::string& GetName() const { return GetComponent<TagComponent>().Name; }
		TransformComponent& GetTransform() const { return GetComponent<TransformComponent>(); }

		entt::entity GetHandle() const { return m_Handle; }
		Scene* GetScene() const { return m_Scene; }

		explicit operator bool() const { return m_Scene != nullptr && m_Handle != entt::null; }
		bool operator==(const Entity& other) const { return m_Handle == other.m_Handle && m_Scene == other.m_Scene; }
		bool operator!=(const Entity& other) const { return !(*this == other); }

	private:
		entt::registry& Registry() const
		{
			PX_CORE_VERIFY(*this, "Operation on a null Entity");
			PX_CORE_VERIFY(m_Scene->m_Registry.valid(m_Handle), "Operation on a destroyed Entity");
			return m_Scene->m_Registry;
		}

	private:
		entt::entity m_Handle = entt::null;
		Scene* m_Scene = nullptr;
	};

}
