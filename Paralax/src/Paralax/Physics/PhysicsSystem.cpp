#include "pxpch.h"
#include "Paralax/Physics/PhysicsSystem.h"

#include "Paralax/Math/Math.h"
#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Scene.h"

// Jolt.h must come first among the Jolt headers.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Paralax {

	namespace {

		// --- Process-wide Jolt setup -----------------------------------------------------------------------

		// Jolt's trace hook is printf-style.
		PX_PRINTF_FORMAT(1, 2)
		void JoltTrace(const char* format, ...)
		{
			char buffer[1024];
			va_list arguments;
			va_start(arguments, format);
			std::vsnprintf(buffer, sizeof(buffer), format, arguments);
			va_end(arguments);
			PX_CORE_TRACE("Jolt: {}", buffer);
		}

		// Jolt's type registry is global; it is created once and lives for the rest of the process.
		void EnsureJoltInitialized()
		{
			static std::once_flag s_Once;
			static Scope<JPH::Factory> s_Factory;
			std::call_once(s_Once, [] {
				JPH::RegisterDefaultAllocator();
				JPH::Trace = &JoltTrace;
				s_Factory = CreateScope<JPH::Factory>();
				JPH::Factory::sInstance = s_Factory.get();
				JPH::RegisterTypes();
			});
		}

		// --- Layers: static bodies never test against each other --------------------------------------------

		namespace Layers {
			constexpr JPH::ObjectLayer Static = 0;
			constexpr JPH::ObjectLayer Moving = 1;
		}

		namespace BroadPhaseLayers {
			constexpr JPH::BroadPhaseLayer Static(0);
			constexpr JPH::BroadPhaseLayer Moving(1);
			constexpr uint32_t Count = 2;
		}

		class BroadPhaseLayerMapping final : public JPH::BroadPhaseLayerInterface
		{
		public:
			uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
			{
				return layer == Layers::Static ? BroadPhaseLayers::Static : BroadPhaseLayers::Moving;
			}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
			{
				return layer == BroadPhaseLayers::Static ? "Static" : "Moving";
			}
#endif
		};

		class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		public:
			bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
			{
				return layer != Layers::Static || broadPhaseLayer == BroadPhaseLayers::Moving;
			}
		};

		class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
		{
		public:
			bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a != Layers::Static || b != Layers::Static; }
		};

		class IgnoreTriggers final : public JPH::BodyFilter
		{
		public:
			bool ShouldCollideLocked(const JPH::Body& body) const override { return !body.IsSensor(); }
		};

		JPH::Vec3 ToJolt(const glm::vec3& v)
		{
			return JPH::Vec3(v.x, v.y, v.z);
		}

		JPH::Quat ToJolt(const glm::quat& q)
		{
			return JPH::Quat(q.x, q.y, q.z, q.w);
		}

		glm::vec3 FromJolt(JPH::Vec3Arg v)
		{
			return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
		}

		glm::quat FromJolt(JPH::QuatArg q)
		{
			return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
		}

		bool IsFinite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// Everything that, when changed, requires rebuilding a body.
		struct BodyConfiguration
		{
			RigidbodyComponent Rigidbody;
			bool HasRigidbody = false;
			ColliderComponent Collider;
			glm::vec3 WorldScale{1.0f};

			bool operator==(const BodyConfiguration& other) const
			{
				const RigidbodyComponent& a = Rigidbody;
				const RigidbodyComponent& b = other.Rigidbody;
				const ColliderComponent& c = Collider;
				const ColliderComponent& d = other.Collider;
				return HasRigidbody == other.HasRigidbody && a.Type == b.Type && a.Mass == b.Mass && a.LinearDamping == b.LinearDamping &&
					   a.AngularDamping == b.AngularDamping && a.GravityScale == b.GravityScale && a.LockRotation == b.LockRotation &&
					   a.ContinuousCollision == b.ContinuousCollision && c.Shape == d.Shape && c.Size == d.Size && c.Offset == d.Offset &&
					   c.Friction == d.Friction && c.Restitution == d.Restitution && c.IsTrigger == d.IsTrigger && SameScale(WorldScale, other.WorldScale);
			}

			RigidbodyType Type() const { return HasRigidbody ? Rigidbody.Type : RigidbodyType::Static; }

			// The world scale is recovered from a matrix every update, so it is compared with a relative tolerance:
			// rounding must not rebuild (and so stop) a rotating body.
			static bool SameScale(const glm::vec3& a, const glm::vec3& b)
			{
				const glm::vec3 tolerance = 1e-5f * glm::max(glm::vec3(1.0f), glm::max(glm::abs(a), glm::abs(b)));
				return glm::all(glm::lessThanEqual(glm::abs(a - b), tolerance));
			}
		};

		// Builds the collision shape for a collider at a world scale. Returns null (with outError set) if Jolt rejects
		// it; sizes are clamped to sane positive values first so hostile scene data cannot crash the engine.
		constexpr float MinimumExtent = 0.001f;
		constexpr float MaximumExtent = 100000.0f;

		JPH::ShapeRefC CreateShape(const ColliderComponent& collider, const glm::vec3& scale, std::string& outError)
		{
			auto clampExtent = [](float value) {
				return std::isfinite(value) ? std::clamp(std::abs(value), MinimumExtent, MaximumExtent) : 1.0f;
			};
			const glm::vec3 size(clampExtent(collider.Size.x * scale.x), clampExtent(collider.Size.y * scale.y), clampExtent(collider.Size.z * scale.z));

			JPH::ShapeSettings::ShapeResult result;
			switch (collider.Shape)
			{
				case ColliderShape::Box:
				{
					const glm::vec3 half = size * 0.5f;
					// The convex radius rounds edges for stability; it must fit inside the box.
					const float convexRadius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
					result = JPH::BoxShapeSettings(ToJolt(half), convexRadius).Create();
					break;
				}
				case ColliderShape::Sphere:
					result = JPH::SphereShapeSettings(0.5f * std::max({size.x, size.y, size.z})).Create();
					break;
				case ColliderShape::Capsule:
				{
					const float radius = 0.5f * std::max(size.x, size.z);
					// Total height includes both caps; a capsule shorter than its diameter degenerates to a sphere.
					const float halfCylinder = std::max(0.5f * size.y - radius, MinimumExtent);
					result = JPH::CapsuleShapeSettings(halfCylinder, radius).Create();
					break;
				}
				case ColliderShape::Cylinder:
				{
					const float radius = 0.5f * std::max(size.x, size.z);
					const float halfHeight = 0.5f * size.y;
					const float convexRadius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min(radius, halfHeight));
					result = JPH::CylinderShapeSettings(halfHeight, radius, convexRadius).Create();
					break;
				}
			}
			if (result.HasError() || !result.IsValid())
			{
				outError = result.HasError() ? std::string(result.GetError().c_str()) : std::string("unknown collider shape");
				return nullptr;
			}
			JPH::ShapeRefC shape = result.Get();
			const glm::vec3 offset = collider.Offset * scale;
			if (IsFinite(offset) && glm::length(offset) > 1e-6f)
			{
				JPH::ShapeSettings::ShapeResult offsetResult = JPH::RotatedTranslatedShapeSettings(ToJolt(offset), JPH::Quat::sIdentity(), shape).Create();
				if (offsetResult.HasError())
				{
					outError = offsetResult.GetError().c_str();
					return nullptr;
				}
				shape = offsetResult.Get();
			}
			return shape;
		}

		// Simulation capacity beyond the body count (see PhysicsSystemSpecification::MaxBodies).
		constexpr uint32_t MaxBodyPairs = 65536;
		constexpr uint32_t MaxContactConstraints = 16384;
		constexpr uint32_t TempAllocatorBytes = 64 * 1024 * 1024;
		// How far beyond a removed or moved body sleeping neighbours are woken.
		constexpr float WakeMargin = 0.1f;

		uint32_t ChooseWorkerThreads(uint32_t requested)
		{
			const uint32_t available = std::max(std::thread::hardware_concurrency(), 2u) - 1;
			return std::min(requested, available);
		}

		// A pair of entities in contact (the smaller UUID first), and whether a trigger is involved.
		struct ContactPair
		{
			uint64_t A = 0;
			uint64_t B = 0;
			bool Trigger = false;

			// Trigger is part of the identity: a collider that becomes a trigger while touching ends its collision and
			// starts a trigger contact.
			auto operator<=>(const ContactPair& other) const = default;
		};

		// Collects, from Jolt's worker threads, the pairs touching during a step (new or persisting contacts). Jolt's own
		// "removed" callbacks are not used: it also reports contacts as removed when bodies fall asleep, while games
		// expect a resting box to keep touching the ground. Contact state is derived from these sets instead.
		class ContactCollector final : public JPH::ContactListener
		{
		public:
			void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&, JPH::ContactSettings&) override { Record(a, b); }
			void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&, JPH::ContactSettings&) override { Record(a, b); }

			// The pairs touching during the last step, sorted and unique.
			std::vector<ContactPair> Take()
			{
				std::lock_guard lock(m_Mutex);
				std::vector<ContactPair> pairs;
				pairs.swap(m_Pairs);
				std::sort(pairs.begin(), pairs.end());
				pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
				return pairs;
			}

		private:
			void Record(const JPH::Body& a, const JPH::Body& b)
			{
				const uint64_t first = a.GetUserData();
				const uint64_t second = b.GetUserData();
				const ContactPair pair{std::min(first, second), std::max(first, second), a.IsSensor() || b.IsSensor()};
				std::lock_guard lock(m_Mutex);
				m_Pairs.push_back(pair);
			}

			std::mutex m_Mutex;
			std::vector<ContactPair> m_Pairs;
		};

	}

	struct PhysicsSystem::Impl
	{
		struct BodyRecord
		{
			JPH::BodyID Body;
			BodyConfiguration Configuration;
			// The world transform the body and entity last agreed on: a different one means something else moved
			// the entity.
			glm::mat4 World{1.0f};
		};

		explicit Impl(PhysicsSystemSpecification specification)
			: Specification(specification)
		{
		}

		PhysicsSystemSpecification Specification;
		Scene* ActiveScene = nullptr;

		Scope<JPH::TempAllocatorImpl> TempAllocator;
		Scope<JPH::JobSystem> JobSystem;
		BroadPhaseLayerMapping BroadPhaseMapping;
		ObjectVsBroadPhaseFilter ObjectVsBroadPhase;
		ObjectPairFilter ObjectPairs;
		ContactCollector Contacts;
		Scope<JPH::PhysicsSystem> Physics;

		std::unordered_map<uint64_t, BodyRecord> Records; // By entity UUID.
		std::vector<ContactPair> Touching;                // Pairs in contact after the last step, sorted.
		std::vector<ContactEvent> PendingEvents;
		bool DroppedEvents = false;
		// Forces added since the last update, and force x time not yet delivered by a step (by entity; ordered maps
		// so they are applied in a deterministic order).
		std::map<uint64_t, glm::vec3> PendingForces;
		std::map<uint64_t, glm::vec3> PendingMomentum;
		// Entities whose current problem has been logged, so it is reported once rather than every update.
		std::unordered_set<uint64_t> Reported;
		std::unordered_set<uint64_t> Advised;
		float Accumulator = 0.0f;
		uint64_t Steps = 0;

		JPH::BodyInterface& Bodies() const { return Physics->GetBodyInterface(); }

		const BodyRecord* Find(UUID entity) const
		{
			if (!Physics)
				return nullptr;
			auto it = Records.find(entity);
			return it == Records.end() ? nullptr : &it->second;
		}

		const BodyRecord* FindDynamic(UUID entity) const
		{
			const BodyRecord* record = Find(entity);
			return record && record->Configuration.Type() == RigidbodyType::Dynamic ? record : nullptr;
		}

		// True the first time a problem is reported for an entity (until it is resolved).
		bool FirstReport(uint64_t entity) { return Reported.insert(entity).second; }

		void Start(Scene& scene)
		{
			EnsureJoltInitialized();
			ActiveScene = &scene;
			TempAllocator = CreateScope<JPH::TempAllocatorImpl>(TempAllocatorBytes);
			const uint32_t threads = ChooseWorkerThreads(Specification.WorkerThreads);
			if (threads == 0)
				JobSystem = CreateScope<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
			else
				JobSystem = CreateScope<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(threads));

			Physics = CreateScope<JPH::PhysicsSystem>();
			const uint32_t maxBodies = std::max(Specification.MaxBodies, 16u);
			// Contact capacity is preallocated from the temporary allocator every step, so it is sized for contacts,
			// not bodies; overflowing it drops contacts with a warning (see Update) rather than crashing.
			Physics->Init(maxBodies, 0, MaxBodyPairs, MaxContactConstraints, BroadPhaseMapping, ObjectVsBroadPhase, ObjectPairs);
			Physics->SetGravity(ToJolt(Specification.Gravity));
			Physics->SetContactListener(&Contacts);
			Accumulator = 0.0f;
			Steps = 0;
			Sync(scene);
			Physics->OptimizeBroadPhase();
		}

		void Stop()
		{
			if (!Physics)
				return;
			for (auto& [entity, record] : Records)
			{
				Bodies().RemoveBody(record.Body);
				Bodies().DestroyBody(record.Body);
			}
			Records.clear();
			Touching.clear();
			PendingEvents.clear();
			DroppedEvents = false;
			PendingForces.clear();
			PendingMomentum.clear();
			Reported.clear();
			Advised.clear();
			Physics.reset();
			JobSystem.reset();
			TempAllocator.reset();
			ActiveScene = nullptr;
		}

		// Wakes the sleeping bodies around a body (Jolt does not when a body is removed or teleported, so a box resting
		// on a platform that vanishes or moves would otherwise hang in the air).
		void WakeAround(const JPH::BodyID& body)
		{
			JPH::AABox bounds = Bodies().GetTransformedShape(body).GetWorldSpaceBounds();
			bounds.ExpandBy(JPH::Vec3::sReplicate(WakeMargin));
			Bodies().ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
		}

		void RemoveBody(UUID entity)
		{
			auto it = Records.find(entity);
			if (it == Records.end())
				return;
			WakeAround(it->second.Body);
			Bodies().RemoveBody(it->second.Body);
			Bodies().DestroyBody(it->second.Body);
			Records.erase(it);
		}

		// Colliders are not combined into an ancestor's rigid body; say so once, since it is a likely authoring mistake.
		void AdviseIfUnderRigidbody(Scene& scene, Entity entity)
		{
			if (Advised.contains(entity.GetUUID()))
				return;
			for (std::optional<Entity> ancestor = scene.GetParent(entity); ancestor; ancestor = scene.GetParent(*ancestor))
			{
				const auto* rigidbody = ancestor->TryGetComponent<RigidbodyComponent>();
				if (rigidbody && rigidbody->Type != RigidbodyType::Static)
				{
					Advised.insert(entity.GetUUID());
					PX_CORE_WARN("Physics: '{}' has a Collider but no Rigidbody, under '{}' which has one. Colliders are not combined into a parent's body, so it is a separate static body that the parent collides with; put the collider on '{}' instead",
								 entity.GetName(), ancestor->GetName(), ancestor->GetName());
					return;
				}
			}
		}

		BodyRecord* CreateBody(Scene& scene, Entity entity, const BodyConfiguration& configuration, const glm::mat4& world, const glm::vec3& position, const glm::quat& rotation)
		{
			const uint64_t id = entity.GetUUID();
			std::string shapeError;
			JPH::ShapeRefC shape = CreateShape(configuration.Collider, configuration.WorldScale, shapeError);
			if (!shape)
			{
				if (FirstReport(id))
					PX_CORE_ERROR("Physics: cannot create the collider of '{}': {}", entity.GetName(), shapeError);
				return nullptr;
			}

			const RigidbodyType type = configuration.Type();
			const JPH::EMotionType motion = type == RigidbodyType::Static      ? JPH::EMotionType::Static
											: type == RigidbodyType::Kinematic ? JPH::EMotionType::Kinematic
																			   : JPH::EMotionType::Dynamic;
			JPH::BodyCreationSettings settings(shape, JPH::RVec3(position.x, position.y, position.z), ToJolt(glm::normalize(rotation)), motion,
											   type == RigidbodyType::Static ? Layers::Static : Layers::Moving);
			settings.mUserData = id;
			settings.mFriction = configuration.Collider.Friction;
			settings.mRestitution = configuration.Collider.Restitution;
			settings.mIsSensor = configuration.Collider.IsTrigger;
			// Kinematic triggers (e.g. a moving pickup zone) should still see static and kinematic bodies.
			settings.mCollideKinematicVsNonDynamic = configuration.Collider.IsTrigger;
			if (configuration.HasRigidbody)
			{
				const RigidbodyComponent& body = configuration.Rigidbody;
				settings.mLinearDamping = std::max(body.LinearDamping, 0.0f);
				settings.mAngularDamping = std::max(body.AngularDamping, 0.0f);
				settings.mGravityFactor = body.GravityScale;
				settings.mMotionQuality = body.ContinuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
				if (body.LockRotation)
					settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
				if (type == RigidbodyType::Dynamic)
				{
					settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
					settings.mMassPropertiesOverride.mMass = std::max(body.Mass, 0.001f);
				}
			}

			JPH::Body* created = Bodies().CreateBody(settings);
			if (!created)
			{
				if (FirstReport(id))
					PX_CORE_ERROR("Physics: the body limit ({}) is reached; '{}' is not simulated", Specification.MaxBodies, entity.GetName());
				return nullptr;
			}
			const JPH::BodyID body = created->GetID();
			Bodies().AddBody(body, type == RigidbodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
			// A static body that appears wakes what it now overlaps (moving bodies wake their neighbours themselves).
			if (type == RigidbodyType::Static)
			{
				WakeAround(body);
				AdviseIfUnderRigidbody(scene, entity);
			}
			Reported.erase(id);
			BodyRecord& record = Records[id];
			record = BodyRecord{body, configuration, world};
			return &record;
		}

		// Brings the bodies in line with the scene. stepping is false only at the start, when moved bodies are simply
		// placed; while running, kinematic bodies are moved smoothly over the coming steps (stepSeconds; they are
		// listed in movedKinematic, to be stopped afterwards) and other bodies are teleported.
		void Sync(Scene& scene, float stepSeconds = 0.0f, std::vector<JPH::BodyID>* movedKinematic = nullptr)
		{
			std::vector<uint64_t> seen;
			scene.ForEachEntity([&](Entity entity) {
				const uint64_t id = entity.GetUUID();
				const auto* collider = entity.TryGetComponent<ColliderComponent>();
				const auto* rigidbody = entity.TryGetComponent<RigidbodyComponent>();
				if (!collider)
				{
					if (rigidbody && FirstReport(id))
						PX_CORE_WARN("Physics: '{}' has a Rigidbody but no Collider, so it is not simulated", entity.GetName());
					return;
				}
				seen.push_back(id);
				BodyConfiguration configuration;
				configuration.Collider = *collider;
				if (rigidbody)
				{
					configuration.Rigidbody = *rigidbody;
					configuration.HasRigidbody = true;
				}
				const glm::mat4 world = scene.GetWorldTransform(entity);
				glm::vec3 translation{0.0f};
				glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
				if (!Math::DecomposeTransform(world, translation, rotation, configuration.WorldScale))
				{
					// Zero scale (say, a group hidden by scaling its parent to 0) or a non-finite transform: no body until
					// the transform is usable again.
					RemoveBody(id);
					if (FirstReport(id))
						PX_CORE_WARN("Physics: '{}' has a zero-scale or non-finite transform, so it is not simulated", entity.GetName());
					return;
				}

				auto it = Records.find(id);
				std::optional<std::pair<JPH::Vec3, JPH::Vec3>> carriedVelocity;
				if (it != Records.end() && !(it->second.Configuration == configuration))
				{
					// Rebuilt with new settings; a body that stays dynamic keeps its motion.
					if (it->second.Configuration.Type() == RigidbodyType::Dynamic && configuration.Type() == RigidbodyType::Dynamic)
						carriedVelocity.emplace(Bodies().GetLinearVelocity(it->second.Body), Bodies().GetAngularVelocity(it->second.Body));
					RemoveBody(id);
					it = Records.end();
				}
				if (it == Records.end())
				{
					BodyRecord* created = CreateBody(scene, entity, configuration, world, translation, rotation);
					if (created && carriedVelocity)
						Bodies().SetLinearAndAngularVelocity(created->Body, carriedVelocity->first, carriedVelocity->second);
					return;
				}
				BodyRecord& record = it->second;
				if (record.World == world)
					return;
				// Moved by something other than the simulation (a script, the editor, a parent).
				const JPH::RVec3 position(translation.x, translation.y, translation.z);
				const JPH::Quat orientation = ToJolt(glm::normalize(rotation));
				const RigidbodyType type = configuration.Type();
				if (type == RigidbodyType::Kinematic && movedKinematic)
				{
					// On an update without steps the move waits for the next one (record.World is kept, so it is seen
					// again): teleporting instead would drop what the body carries.
					if (stepSeconds <= 0.0f)
						return;
					Bodies().MoveKinematic(record.Body, position, orientation, stepSeconds);
					movedKinematic->push_back(record.Body);
				}
				else
				{
					WakeAround(record.Body); // What rested on it at the old place.
					Bodies().SetPositionAndRotation(record.Body, position, orientation, type == RigidbodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
					if (type == RigidbodyType::Static)
						WakeAround(record.Body); // What it now overlaps.
				}
				record.World = world;
			});

			// Entities that lost their collider.
			std::sort(seen.begin(), seen.end());
			std::vector<uint64_t> orphaned;
			for (const auto& [entity, record] : Records)
			{
				if (!std::binary_search(seen.begin(), seen.end(), entity))
					orphaned.push_back(entity);
			}
			std::sort(orphaned.begin(), orphaned.end());
			for (uint64_t entity : orphaned)
				RemoveBody(UUID(entity));
		}

		void WriteBack(Scene& scene)
		{
			// Hierarchy order, so a dynamic parent is written before its dynamic children compute their world transform.
			scene.ForEachEntity([&](Entity entity) {
				auto it = Records.find(entity.GetUUID());
				if (it == Records.end() || it->second.Configuration.Type() != RigidbodyType::Dynamic)
					return;
				BodyRecord& record = it->second;
				JPH::RVec3 position;
				JPH::Quat rotation;
				Bodies().GetPositionAndRotation(record.Body, position, rotation);
				const glm::mat4 world = Math::ComposeTransform(glm::vec3(position.GetX(), position.GetY(), position.GetZ()), FromJolt(rotation), record.Configuration.WorldScale);
				scene.SetWorldTransform(entity, world);
				// Re-read: the local transform round trip may differ by rounding, and that must not count as an
				// external move next update.
				record.World = scene.GetWorldTransform(entity);
			});
		}

		static ContactEvent MakeEvent(const ContactPair& pair, bool enter)
		{
			ContactEvent event;
			event.Type = pair.Trigger ? (enter ? ContactEventType::TriggerEnter : ContactEventType::TriggerExit)
									  : (enter ? ContactEventType::CollisionEnter : ContactEventType::CollisionExit);
			event.A = UUID(pair.A);
			event.B = UUID(pair.B);
			return event;
		}

		void Emit(const ContactPair& pair, bool enter)
		{
			if (PendingEvents.size() >= Specification.MaxPendingContactEvents)
			{
				if (!DroppedEvents)
					PX_CORE_WARN("Physics: more than {} contact events are waiting to be taken; further events are dropped", Specification.MaxPendingContactEvents);
				DroppedEvents = true;
				return;
			}
			PendingEvents.push_back(MakeEvent(pair, enter));
		}

		// A body that exists and is awake (static bodies never are).
		bool IsAwake(uint64_t entity) const
		{
			auto it = Records.find(entity);
			return it != Records.end() && Bodies().IsActive(it->second.Body);
		}

		// Compares the pairs touching this step with the previous state: new pairs enter; missing pairs exit unless both
		// bodies still exist and are asleep (Jolt does not report contacts of sleeping bodies). Both lists are sorted,
		// so events come out in a deterministic order.
		void UpdateContacts(const std::vector<ContactPair>& current)
		{
			std::vector<ContactPair> next;
			size_t i = 0;
			size_t j = 0;
			while (i < Touching.size() || j < current.size())
			{
				if (j == current.size() || (i < Touching.size() && Touching[i] < current[j]))
				{
					const ContactPair& previous = Touching[i++];
					const bool sleeping = Records.contains(previous.A) && Records.contains(previous.B) && !IsAwake(previous.A) && !IsAwake(previous.B);
					if (sleeping)
						next.push_back(previous);
					else
						Emit(previous, false);
				}
				else if (i == Touching.size() || current[j] < Touching[i])
				{
					Emit(current[j], true);
					next.push_back(current[j++]);
				}
				else
				{
					next.push_back(current[j++]);
					i++;
				}
			}
			Touching = std::move(next);
		}

		void Update(Scene& scene, float seconds)
		{
			if (!Physics)
				return;
			const float step = Specification.FixedTimestep;
			const float maxSeconds = step * static_cast<float>(Specification.MaxStepsPerUpdate);
			// A long hitch is simulated as at most MaxStepsPerUpdate steps; the rest of it is dropped, so a slow frame
			// slows the simulation down instead of stalling the next frames further.
			const float frameSeconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, maxSeconds) : 0.0f;
			Accumulator += frameSeconds;
			uint32_t steps = static_cast<uint32_t>(std::min(Accumulator / step, static_cast<float>(Specification.MaxStepsPerUpdate)));
			Accumulator = std::max(Accumulator - static_cast<float>(steps) * step, 0.0f);
			if (steps == Specification.MaxStepsPerUpdate)
				Accumulator = std::min(Accumulator, step);

			// A force acts for the frame it was added in: its momentum (force x frame time) is delivered by the next
			// step, so the result does not depend on how frames line up with steps.
			for (const auto& [entity, force] : PendingForces)
				PendingMomentum[entity] += force * frameSeconds;
			PendingForces.clear();

			std::vector<JPH::BodyID> movedKinematic;
			Sync(scene, static_cast<float>(steps) * step, &movedKinematic);
			if (steps > 0)
			{
				for (const auto& [entity, momentum] : PendingMomentum)
				{
					if (const BodyRecord* record = FindDynamic(UUID(entity)))
						Bodies().AddImpulse(record->Body, ToJolt(momentum));
				}
				PendingMomentum.clear();
			}
			for (uint32_t i = 0; i < steps; i++)
			{
				const JPH::EPhysicsUpdateError error = Physics->Update(step, 1, TempAllocator.get(), JobSystem.get());
				if (error != JPH::EPhysicsUpdateError::None)
					PX_CORE_WARN("Physics: the simulation ran out of capacity (error {}); increase the limits", static_cast<int>(error));
				Steps++;
				UpdateContacts(Contacts.Take());
			}
			// MoveKinematic gives a body the velocity to reach its target over these steps; without stopping it there, it
			// would keep drifting in the simulation while the scene shows it at the target.
			for (const JPH::BodyID& body : movedKinematic)
				Bodies().SetLinearAndAngularVelocity(body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
			if (steps > 0)
				WriteBack(scene);
		}
	};

	PhysicsSystem::PhysicsSystem(PhysicsSystemSpecification specification)
		: m_Impl(CreateScope<Impl>(specification))
	{
		PX_CORE_VERIFY(std::isfinite(specification.FixedTimestep) && specification.FixedTimestep > 0.0f && specification.FixedTimestep <= 1.0f, "The physics time step must be in (0, 1] seconds");
		PX_CORE_VERIFY(specification.MaxStepsPerUpdate > 0 && specification.MaxStepsPerUpdate <= 1000, "MaxStepsPerUpdate must be in [1, 1000]");
		PX_CORE_VERIFY(IsFinite(specification.Gravity), "Gravity must be finite");
	}

	PhysicsSystem::~PhysicsSystem()
	{
		m_Impl->Stop();
	}

	void PhysicsSystem::OnRuntimeStart(Scene& scene)
	{
		m_Impl->Stop();
		m_Impl->Start(scene);
	}

	void PhysicsSystem::OnUpdate(Scene& scene, Timestep timestep)
	{
		if (m_Impl->ActiveScene == &scene)
			m_Impl->Update(scene, timestep.GetSeconds());
	}

	void PhysicsSystem::OnRuntimeStop(Scene& scene)
	{
		if (m_Impl->ActiveScene == &scene)
			m_Impl->Stop();
	}

	void PhysicsSystem::OnEntityDestroying(Scene& scene, const Entity& entity)
	{
		if (m_Impl->ActiveScene != &scene)
			return;
		const uint64_t id = entity.GetUUID();
		m_Impl->RemoveBody(UUID(id));
		m_Impl->Reported.erase(id);
		m_Impl->Advised.erase(id);
		m_Impl->PendingForces.erase(id);
		m_Impl->PendingMomentum.erase(id);
	}

	std::vector<ContactEvent> PhysicsSystem::TakeContactEvents()
	{
		std::vector<ContactEvent> events;
		events.swap(m_Impl->PendingEvents);
		m_Impl->DroppedEvents = false;
		return events;
	}

	bool PhysicsSystem::AddForce(UUID entity, const glm::vec3& force)
	{
		if (!m_Impl->FindDynamic(entity) || !IsFinite(force))
			return false;
		m_Impl->PendingForces[entity] += force;
		return true;
	}

	bool PhysicsSystem::AddImpulse(UUID entity, const glm::vec3& impulse)
	{
		const Impl::BodyRecord* record = m_Impl->FindDynamic(entity);
		if (!record || !IsFinite(impulse))
			return false;
		m_Impl->Bodies().AddImpulse(record->Body, ToJolt(impulse));
		return true;
	}

	bool PhysicsSystem::SetLinearVelocity(UUID entity, const glm::vec3& velocity)
	{
		const Impl::BodyRecord* record = m_Impl->FindDynamic(entity);
		if (!record || !IsFinite(velocity))
			return false;
		m_Impl->Bodies().SetLinearVelocity(record->Body, ToJolt(velocity));
		m_Impl->Bodies().ActivateBody(record->Body);
		return true;
	}

	std::optional<glm::vec3> PhysicsSystem::GetLinearVelocity(UUID entity) const
	{
		const Impl::BodyRecord* record = m_Impl->Find(entity);
		if (!record)
			return std::nullopt;
		return FromJolt(m_Impl->Bodies().GetLinearVelocity(record->Body));
	}

	bool PhysicsSystem::SetAngularVelocity(UUID entity, const glm::vec3& velocity)
	{
		const Impl::BodyRecord* record = m_Impl->FindDynamic(entity);
		if (!record || !IsFinite(velocity))
			return false;
		m_Impl->Bodies().SetAngularVelocity(record->Body, ToJolt(velocity));
		m_Impl->Bodies().ActivateBody(record->Body);
		return true;
	}

	std::optional<glm::vec3> PhysicsSystem::GetAngularVelocity(UUID entity) const
	{
		const Impl::BodyRecord* record = m_Impl->Find(entity);
		if (!record)
			return std::nullopt;
		return FromJolt(m_Impl->Bodies().GetAngularVelocity(record->Body));
	}

	bool PhysicsSystem::HasBody(UUID entity) const
	{
		return m_Impl->Find(entity) != nullptr;
	}

	std::optional<RaycastHit> PhysicsSystem::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const
	{
		if (!m_Impl->Physics || !IsFinite(origin) || !IsFinite(direction) || !std::isfinite(maxDistance) || !(maxDistance > 0.0f) || glm::length(direction) < 1e-8f)
			return std::nullopt;
		const glm::vec3 unit = glm::normalize(direction);
		const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), ToJolt(unit * maxDistance));
		JPH::RayCastResult result;
		IgnoreTriggers filter;
		if (!m_Impl->Physics->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, filter))
			return std::nullopt;

		RaycastHit hit;
		hit.Distance = result.mFraction * maxDistance;
		hit.Point = origin + unit * hit.Distance;
		JPH::BodyLockRead lock(m_Impl->Physics->GetBodyLockInterface(), result.mBodyID);
		if (!lock.Succeeded())
			return std::nullopt;
		const JPH::Body& body = lock.GetBody();
		hit.Entity = UUID(body.GetUserData());
		hit.Normal = FromJolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
		return hit;
	}

	glm::vec3 PhysicsSystem::GetGravity() const
	{
		return m_Impl->Physics ? FromJolt(m_Impl->Physics->GetGravity()) : m_Impl->Specification.Gravity;
	}

	void PhysicsSystem::SetGravity(const glm::vec3& gravity)
	{
		if (!IsFinite(gravity))
			return;
		m_Impl->Specification.Gravity = gravity;
		if (m_Impl->Physics)
			m_Impl->Physics->SetGravity(ToJolt(gravity));
	}

	uint64_t PhysicsSystem::GetStepCount() const
	{
		return m_Impl->Steps;
	}

}
