#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Core/UUID.h"
#include "Paralax/Scene/SceneSystem.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Paralax {

	struct PhysicsSystemSpecification
	{
		glm::vec3 Gravity{0.0f, -9.81f, 0.0f};
		// The simulation always advances in steps of this size (determinism); frame time is accumulated.
		float FixedTimestep = 1.0f / 60.0f;
		// Steps one update may take to catch up (1 to 1000); time beyond that is dropped (a slow frame slows the
		// simulation down instead of stalling it further).
		uint32_t MaxStepsPerUpdate = 8;
		uint32_t MaxBodies = 65536;
		// Worker threads for the simulation (0 = single-threaded). The result is identical for any count.
		uint32_t WorkerThreads = 2;
		// Colliders one body may combine (its own and its children's); further ones are left out with a warning.
		uint32_t MaxCollidersPerBody = 1024;
		// Contact events kept until TakeContactEvents; further ones are dropped with a warning.
		uint32_t MaxPendingContactEvents = 65536;
	};

	enum class ContactEventType
	{
		CollisionEnter,
		CollisionExit,
		TriggerEnter, // One of the two is a trigger collider.
		TriggerExit
	};

	// A change in contact between two entities. A is the entity with the smaller UUID. Exits may refer to entities that
	// have since been destroyed.
	struct ContactEvent
	{
		ContactEventType Type = ContactEventType::CollisionEnter;
		UUID A = 0;
		UUID B = 0;
	};

	struct RaycastHit
	{
		UUID Entity = 0;
		glm::vec3 Point{0.0f};
		glm::vec3 Normal{0.0f};
		float Distance = 0.0f;
	};

	// Rigid body simulation (Jolt Physics) for a running scene. Add it to a scene before Scene::OnRuntimeStart.
	//
	// Every entity with a RigidbodyComponent is a body of the rigidbody's type. Its shape is its own ColliderComponent
	// plus those of descendants without a Rigidbody of their own (a compound; the body's friction and restitution come
	// from its first collider). A collider with no Rigidbody at or above it is a static body of its own; trigger
	// colliders always are bodies of their own (kinematic sensors following the body above them, if it moves). Events,
	// raycasts and the body controls refer to the body's entity.
	// Bodies follow the scene each update: added, removed, rebuilt when their collider or rigidbody settings (or world
	// scale) change (a dynamic body keeps its velocity), and teleported when something else moves them; kinematic
	// bodies are moved smoothly to their entity's transform during the next step. Entities whose world transform has
	// zero scale or is not finite have no body until it is usable again. Sleeping bodies around a body that is removed
	// or teleported are woken. The simulation advances in fixed steps, then writes the transforms of dynamic bodies back
	// to their entities (world space, so parenting is respected). Problems with an entity are logged once.
	//
	// Contacts are collected during the step and handed out by TakeContactEvents in a deterministic order (scripts
	// receive them as OnCollisionEnter/Exit and OnTriggerEnter/Exit). Simulation is deterministic across platforms for
	// identical inputs.
	//
	// Main-thread-only (the simulation uses worker threads internally).
	class PhysicsSystem final : public SceneSystem
	{
	public:
		explicit PhysicsSystem(PhysicsSystemSpecification specification = {});
		~PhysicsSystem() override;

		PhysicsSystem(const PhysicsSystem&) = delete;
		PhysicsSystem& operator=(const PhysicsSystem&) = delete;

		const char* GetName() const override { return "Physics"; }
		void OnRuntimeStart(Scene& scene) override;
		void OnUpdate(Scene& scene, Timestep timestep) override;
		void OnRuntimeStop(Scene& scene) override;
		void OnEntityDestroying(Scene& scene, const Entity& entity) override;

		// Contact changes since the last call, oldest first. Call it every update (ScriptSystem does): at most
		// MaxPendingContactEvents are kept.
		std::vector<ContactEvent> TakeContactEvents();

		// Body controls for dynamic bodies; each returns false if the entity has no dynamic body or the value is not
		// finite. Both act at the centre of mass. A force acts for the current frame (call it every frame for a
		// steady push): force x frame time is delivered by the next step, whatever the frame rate. An impulse changes
		// the velocity at once.
		bool AddForce(UUID entity, const glm::vec3& force);
		bool AddImpulse(UUID entity, const glm::vec3& impulse);
		bool SetLinearVelocity(UUID entity, const glm::vec3& velocity);
		bool SetAngularVelocity(UUID entity, const glm::vec3& velocity);
		// Velocity of any body (zero for static ones); nullopt if the entity has no body.
		std::optional<glm::vec3> GetLinearVelocity(UUID entity) const;
		std::optional<glm::vec3> GetAngularVelocity(UUID entity) const;
		bool HasBody(UUID entity) const;

		// Closest non-trigger collider hit by a ray from origin along direction (need not be normalized), within
		// maxDistance. Returns nullopt for no hit, a zero direction or a non-positive distance.
		std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;

		glm::vec3 GetGravity() const;
		void SetGravity(const glm::vec3& gravity);

		// Bodies created since the scene started running, rebuilds included (a diagnostic: a body rebuilt every update
		// loses its contacts' history and its sleep).
		uint64_t GetBodiesCreated() const;

		// Number of simulation steps taken since the scene started running.
		uint64_t GetStepCount() const;

	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
