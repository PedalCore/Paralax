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
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <map>
#include <mutex>
#include <set>
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

		// Jolt compiles its own assertions in exactly when NDEBUG is not defined (Jolt/Core/IssueReporting.h), so they
		// exist in Debug builds only. JPH_ENABLE_ASSERTS is Jolt's configuration macro; it is handled here because this
		// file is the only one that sees Jolt. Without a handler, a failed one breaks into the debugger (a silent
		// SIGTRAP outside one) with no message.
#ifdef JPH_ENABLE_ASSERTS
		// The one assertion Jolt raises on a runtime condition rather than on misuse: at the end of every update,
		// that the update reported no error (more contacts than its caches hold, say). Continuing is safe: Jolt
		// returns the same error to the caller in every build, and Impl::Update reports it once per run. The text
		// is Jolt's expression in PhysicsSystem::Update; if a Jolt upgrade changes it, the assertion fails like the
		// others, and the capacity test catches that.
		constexpr std::string_view UpdateErrorAssertion = "errors == EPhysicsUpdateError::None";

		// Installed as JPH::AssertFailed. May be called on Jolt's worker threads.
		bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
		{
			expression = expression ? expression : "";
			file = file ? file : "";
			// Logged once per site and expression: the update-error assertion repeats every step while it holds.
			static std::mutex s_ReportedMutex;
			static std::set<std::tuple<std::string, JPH::uint, std::string>> s_Reported;
			bool first = false;
			{
				std::scoped_lock lock(s_ReportedMutex);
				first = s_Reported.emplace(file, line, expression).second;
			}
			if (first)
				PX_CORE_ERROR("Jolt assertion failed: {} ({}:{}){}{}", expression, file, line, message ? " - " : "", message ? message : "");
			if (std::string_view(expression) == UpdateErrorAssertion)
				return false; // Continue; never break into the debugger.
			// Misuse of Jolt fails like the engine's own assertions: the default handler reports it and stops the
			// process, and tests capture it. A capturing handler throws; Jolt is built without exceptions, so that
			// is only well-defined where no Jolt code is being unwound (PhysicsSystem::RaiseJoltAssertion).
			Assertion::Fail(expression, file, static_cast<int>(line), fmt::format("Jolt: {}", message ? message : "assertion failed"));
		}
#endif

		// Jolt's type registry is global; it is created once and lives for the rest of the process.
		void EnsureJoltInitialized()
		{
			static std::once_flag s_Once;
			static Scope<JPH::Factory> s_Factory;
			std::call_once(s_Once, [] {
				JPH::RegisterDefaultAllocator();
				JPH::Trace = &JoltTrace;
#ifdef JPH_ENABLE_ASSERTS
				JPH::AssertFailed = &JoltAssertFailed;
#endif
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

		// Values recovered from matrices every update are compared with a relative tolerance: rounding must not rebuild
		// (and so stop) a rotating body.
		bool SameVector(const glm::vec3& a, const glm::vec3& b)
		{
			const glm::vec3 tolerance = 1e-5f * glm::max(glm::vec3(1.0f), glm::max(glm::abs(a), glm::abs(b)));
			return glm::all(glm::lessThanEqual(glm::abs(a - b), tolerance));
		}

		// q and -q are the same rotation. Component-wise, so small edits (about 0.001 degrees and up) are not lost.
		bool SameRotation(const glm::quat& a, const glm::quat& b)
		{
			const glm::vec4 x(a.x, a.y, a.z, a.w);
			const glm::vec4 y(b.x, b.y, b.z, b.w);
			return glm::all(glm::lessThanEqual(glm::abs(x - y), glm::vec4(1e-5f))) || glm::all(glm::lessThanEqual(glm::abs(x + y), glm::vec4(1e-5f)));
		}

		// One collider of a body: the body entity's own, or a descendant's merged into it. Its placement is relative to
		// the body (in the body's rotated, unscaled frame); its scale is the collider entity's world scale.
		struct BodyPart
		{
			ColliderComponent Collider;
			glm::vec3 Scale{1.0f};
			glm::vec3 Position{0.0f};
			glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};

			bool operator==(const BodyPart& other) const
			{
				const ColliderComponent& c = Collider;
				const ColliderComponent& d = other.Collider;
				return c.Shape == d.Shape && c.Size == d.Size && c.Offset == d.Offset && c.Friction == d.Friction && c.Restitution == d.Restitution &&
					   c.IsTrigger == d.IsTrigger && SameVector(Scale, other.Scale) && SameVector(Position, other.Position) && SameRotation(Rotation, other.Rotation);
			}

			bool IsAtOrigin() const { return glm::length(Position) < 1e-6f && std::abs(Rotation.w) > 1.0f - 1e-6f; }
		};

		// Everything that, when changed, requires rebuilding a body.
		struct BodyConfiguration
		{
			RigidbodyComponent Rigidbody;
			bool HasRigidbody = false;
			// The body entity's world scale, to write its transform back.
			glm::vec3 WorldScale{1.0f};
			// The body entity's own collider first (if it has one), then merged descendants in hierarchy order.
			std::vector<BodyPart> Parts;
			// A trigger collider under a moving body: a kinematic sensor that follows it.
			bool FollowsBody = false;

			bool operator==(const BodyConfiguration& other) const
			{
				const RigidbodyComponent& a = Rigidbody;
				const RigidbodyComponent& b = other.Rigidbody;
				return HasRigidbody == other.HasRigidbody && a.Type == b.Type && a.Mass == b.Mass && a.LinearDamping == b.LinearDamping &&
					   a.AngularDamping == b.AngularDamping && a.GravityScale == b.GravityScale && a.LockRotation == b.LockRotation &&
					   a.ContinuousCollision == b.ContinuousCollision && FollowsBody == other.FollowsBody && SameVector(WorldScale, other.WorldScale) &&
					   Parts == other.Parts;
			}

			RigidbodyType Type() const { return HasRigidbody ? Rigidbody.Type : (FollowsBody ? RigidbodyType::Kinematic : RigidbodyType::Static); }
			// Triggers never merge, so a trigger body has exactly one part.
			bool IsTrigger() const { return !Parts.empty() && Parts.front().Collider.IsTrigger; }
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

		// A body's shape: its single collider, or a compound of all its parts. Returns null (with outError set) if Jolt
		// rejects it.
		JPH::ShapeRefC CreateBodyShape(const BodyConfiguration& configuration, std::string& outError)
		{
			if (configuration.Parts.size() == 1 && configuration.Parts.front().IsAtOrigin())
				return CreateShape(configuration.Parts.front().Collider, configuration.Parts.front().Scale, outError);
			JPH::StaticCompoundShapeSettings compound;
			for (const BodyPart& part : configuration.Parts)
			{
				JPH::ShapeRefC shape = CreateShape(part.Collider, part.Scale, outError);
				if (!shape)
					return nullptr;
				compound.AddShape(ToJolt(part.Position), ToJolt(glm::normalize(part.Rotation)), shape);
			}
			JPH::ShapeSettings::ShapeResult result = compound.Create();
			if (result.HasError() || !result.IsValid())
			{
				outError = result.HasError() ? std::string(result.GetError().c_str()) : std::string("invalid compound shape");
				return nullptr;
			}
			return result.Get();
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

		// Sets a flag for a scope, and clears it on every exit (including an assertion thrown in tests).
		class FlagScope
		{
		public:
			explicit FlagScope(bool& flag)
				: m_Flag(flag)
			{
				m_Flag = true;
			}
			~FlagScope() { m_Flag = false; }

			FlagScope(const FlagScope&) = delete;
			FlagScope& operator=(const FlagScope&) = delete;

		private:
			bool& m_Flag;
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
		// Whether Jolt's capacity error has been reported this run: the condition (more contacts than the caches
		// hold) tends to persist, and steps run many times a second.
		bool WarnedCapacity = false;
		// Forces added since the last update, and force x time not yet delivered by a step (by entity; ordered maps
		// so they are applied in a deterministic order).
		std::map<uint64_t, glm::vec3> PendingForces;
		std::map<uint64_t, glm::vec3> PendingMomentum;
		// Forces added during OnFixedUpdate, which act for the coming step only.
		std::map<uint64_t, glm::vec3> StepForces;
		bool InFixedUpdate = false;
		// Entities whose current problem has been logged, so it is reported once rather than every update.
		std::unordered_set<uint64_t> Reported;
		// Entities whose configuration note (a capped or unjoinable collider) has been logged; unlike Reported, not
		// cleared when a body is created, since the situation persists.
		std::unordered_set<uint64_t> Advised;
		float Accumulator = 0.0f;
		uint64_t Steps = 0;
		uint64_t BodiesCreated = 0;

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
			BodiesCreated = 0;
			WarnedCapacity = false;
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
			WarnedCapacity = false;
			PendingForces.clear();
			PendingMomentum.clear();
			StepForces.clear();
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

		BodyRecord* CreateBody(Entity entity, const BodyConfiguration& configuration, const glm::mat4& world, const glm::vec3& position, const glm::quat& rotation)
		{
			const uint64_t id = entity.GetUUID();
			std::string shapeError;
			JPH::ShapeRefC shape = CreateBodyShape(configuration, shapeError);
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
			// One material per body: that of its first collider (its own, if it has one).
			const ColliderComponent& material = configuration.Parts.front().Collider;
			settings.mFriction = material.Friction;
			settings.mRestitution = material.Restitution;
			settings.mIsSensor = configuration.IsTrigger();
			// Kinematic triggers (e.g. a moving pickup zone) should still see static and kinematic bodies.
			settings.mCollideKinematicVsNonDynamic = configuration.IsTrigger();
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
				WakeAround(body);
			Reported.erase(id);
			BodiesCreated++;
			BodyRecord& record = Records[id];
			record = BodyRecord{body, configuration, world};
			return &record;
		}

		// A body the scene describes this update.
		struct PendingBody
		{
			Entity Owner;
			BodyConfiguration Configuration;
			glm::mat4 World{1.0f};
			glm::vec3 Position{0.0f};
			glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
			bool Usable = true; // False while the body entity's transform has zero scale or is not finite.
		};

		// The bodies the scene describes. Every entity with a Rigidbody is a body; a collider without one joins the body
		// of its nearest ancestor that has one (as a compound part), or else is a static body of its own. Triggers stay
		// separate bodies (Jolt senses per body); under a moving body they are kinematic sensors that follow it.
		// Hierarchy order visits parents first, so a collider's body is known when the collider is reached.
		std::vector<PendingBody> GatherBodies(Scene& scene)
		{
			// Per entity under a body: that body, and the entity's transform relative to the body entity, built from
			// local transforms only. Unlike a difference of world positions it does not change as the body moves, so a
			// body far from the origin is not rebuilt for rounding, and it costs O(1) per entity.
			struct UnderBody
			{
				uint64_t Body = 0;
				glm::mat4 Relative{1.0f};
			};
			std::vector<PendingBody> bodies;
			std::unordered_map<uint64_t, size_t> bodyIndex;
			std::unordered_map<uint64_t, UnderBody> under;
			std::unordered_set<uint64_t> overflowed;
			scene.ForEachEntity([&](Entity entity) {
				// An entity destroyed during this update (and what is under it) takes no further part in the simulation;
				// its body is removed with the other orphans below.
				if (scene.IsDestructionPending(entity))
					return;
				const uint64_t id = entity.GetUUID();
				const auto* rigidbody = entity.TryGetComponent<RigidbodyComponent>();
				const auto* collider = entity.TryGetComponent<ColliderComponent>();
				UnderBody place;
				if (rigidbody)
					place.Body = id;
				else if (const UUID parent = entity.GetComponent<RelationshipComponent>().Parent; parent.IsValid())
				{
					if (auto it = under.find(parent); it != under.end())
						place = {it->second.Body, it->second.Relative * entity.GetComponent<TransformComponent>().GetLocalMatrix()};
				}
				if (place.Body)
					under[id] = place;
				if (!rigidbody && !collider)
					return;

				PendingBody* owner = place.Body && !rigidbody ? &bodies[bodyIndex.at(place.Body)] : nullptr;
				if (owner && !collider->IsTrigger)
				{
					if (owner->Configuration.IsTrigger())
					{
						if (Advised.insert(id).second)
							PX_CORE_WARN("Physics: '{}' is under '{}', whose own collider is a trigger, so it cannot join that body; it is a separate static collider",
										 entity.GetName(), owner->Owner.GetName());
					}
					else
					{
						// Relative to the body's rotated, unscaled frame: the body entity's scale, then the chain below it.
						glm::vec3 position{0.0f};
						glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
						glm::vec3 scale{1.0f};
						if (!owner->Usable)
							return;
						if (!Math::DecomposeTransform(glm::scale(glm::mat4(1.0f), owner->Configuration.WorldScale) * place.Relative, position, rotation, scale))
						{
							if (FirstReport(id))
								PX_CORE_WARN("Physics: '{}' has a zero-scale or non-finite transform, so its collider is left out", entity.GetName());
							return;
						}
						Reported.erase(id);
						if (owner->Configuration.Parts.size() >= Specification.MaxCollidersPerBody)
						{
							if (overflowed.insert(place.Body).second && Advised.insert(place.Body).second)
								PX_CORE_WARN("Physics: '{}' has more than {} colliders; the rest are left out", owner->Owner.GetName(), Specification.MaxCollidersPerBody);
							return;
						}
						owner->Configuration.Parts.push_back(BodyPart{*collider, scale, position, rotation});
						return;
					}
				}

				const glm::mat4 world = scene.GetWorldTransform(entity);
				PendingBody body;
				body.Owner = entity;
				body.World = world;
				// Zero scale (say, a group hidden by scaling its parent to 0) or a non-finite transform: left out until
				// the transform is usable again.
				body.Usable = Math::DecomposeTransform(world, body.Position, body.Rotation, body.Configuration.WorldScale);
				if (!body.Usable && FirstReport(id))
					PX_CORE_WARN("Physics: '{}' has a zero-scale or non-finite transform, so it is not simulated", entity.GetName());
				if (rigidbody)
				{
					body.Configuration.Rigidbody = *rigidbody;
					body.Configuration.HasRigidbody = true;
				}
				body.Configuration.FollowsBody = owner && owner->Configuration.Type() != RigidbodyType::Static;
				if (collider)
					body.Configuration.Parts.push_back(BodyPart{*collider, body.Configuration.WorldScale});
				bodyIndex[id] = bodies.size();
				bodies.push_back(std::move(body));
			});
			return bodies;
		}

		// Brings the bodies in line with the scene. While running (movedKinematic set), kinematic bodies are moved
		// smoothly over the coming steps (stepSeconds; they are listed in movedKinematic, to be stopped afterwards) and
		// other bodies are teleported; at the start, moved bodies are simply placed.
		void Sync(Scene& scene, float stepSeconds = 0.0f, std::vector<JPH::BodyID>* movedKinematic = nullptr)
		{
			std::unordered_set<uint64_t> kept;
			for (PendingBody& body : GatherBodies(scene))
			{
				const uint64_t id = body.Owner.GetUUID();
				if (!body.Usable)
				{
					RemoveBody(id);
					continue;
				}
				if (body.Configuration.Parts.empty())
				{
					RemoveBody(id);
					if (FirstReport(id))
						PX_CORE_WARN("Physics: '{}' has a Rigidbody but no solid Collider (on it, or on a child without a Rigidbody of its own), so it is not simulated", body.Owner.GetName());
					continue;
				}
				kept.insert(id);
				const BodyConfiguration& configuration = body.Configuration;

				auto it = Records.find(id);
				// Rebuilt with new settings, a body that stays dynamic keeps its motion. Jolt's linear velocity is that of the
				// centre of mass, which moves when parts are added or removed: v' = v + w x (c' - c).
				struct CarriedMotion
				{
					JPH::Vec3 Linear;
					JPH::Vec3 Angular;
					JPH::RVec3 CenterOfMass;
				};
				std::optional<CarriedMotion> carried;
				if (it != Records.end() && !(it->second.Configuration == configuration))
				{
					if (it->second.Configuration.Type() == RigidbodyType::Dynamic && configuration.Type() == RigidbodyType::Dynamic)
						carried = CarriedMotion{Bodies().GetLinearVelocity(it->second.Body), Bodies().GetAngularVelocity(it->second.Body), Bodies().GetCenterOfMassPosition(it->second.Body)};
					RemoveBody(id);
					it = Records.end();
				}
				if (it == Records.end())
				{
					BodyRecord* created = CreateBody(body.Owner, configuration, body.World, body.Position, body.Rotation);
					if (created && carried)
					{
						const JPH::Vec3 shift(Bodies().GetCenterOfMassPosition(created->Body) - carried->CenterOfMass);
						Bodies().SetLinearAndAngularVelocity(created->Body, carried->Linear + carried->Angular.Cross(shift), carried->Angular);
					}
					continue;
				}
				BodyRecord& record = it->second;
				if (record.World == body.World)
					continue;
				// Moved by something other than the simulation (a script, the editor, a parent).
				const JPH::RVec3 position(body.Position.x, body.Position.y, body.Position.z);
				const JPH::Quat orientation = ToJolt(glm::normalize(body.Rotation));
				const RigidbodyType type = configuration.Type();
				if (type == RigidbodyType::Kinematic && movedKinematic)
				{
					// On an update without steps the move waits for the next one (record.World is kept, so it is seen
					// again): teleporting instead would drop what the body carries.
					if (stepSeconds <= 0.0f)
						continue;
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
				record.World = body.World;
			}

			// Bodies the scene no longer describes (collider or rigidbody removed, entity gone).
			std::vector<uint64_t> orphaned;
			for (const auto& [entity, record] : Records)
			{
				if (!kept.contains(entity))
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

			// Fixed updates (scripts' OnFixedUpdate) see each step's result and act before the next, so with any of them
			// the scene is synced and written back around every step. Without, once per update is enough.
			const bool fixedUpdates = steps > 0 && scene.WantsFixedUpdate();
			std::vector<JPH::BodyID> movedKinematic;
			// Kinematic bodies moved by the scene glide to their target over all of this update's steps.
			Sync(scene, static_cast<float>(steps) * step, &movedKinematic);
			for (uint32_t i = 0; i < steps; i++)
			{
				std::vector<JPH::BodyID> movedThisStep;
				if (fixedUpdates)
				{
					// What the fixed updates change is synced in before this step, and a kinematic body they move gets
					// there in this step.
					{
						FlagScope inFixedUpdate(InFixedUpdate);
						scene.OnFixedUpdateRuntime(Timestep(step));
					}
					if (!Physics)
						return;
					Sync(scene, step, &movedThisStep);
				}

				for (const auto& [entity, force] : StepForces)
					PendingMomentum[entity] += force * step;
				StepForces.clear();
				for (const auto& [entity, momentum] : PendingMomentum)
				{
					if (const BodyRecord* record = FindDynamic(UUID(entity)))
						Bodies().AddImpulse(record->Body, ToJolt(momentum));
				}
				PendingMomentum.clear();

				const JPH::EPhysicsUpdateError error = Physics->Update(step, 1, TempAllocator.get(), JobSystem.get());
				if (error != JPH::EPhysicsUpdateError::None && !WarnedCapacity)
				{
					// Once per run (see WarnedCapacity): Jolt drops the contacts beyond its caches and carries on.
					WarnedCapacity = true;
					PX_CORE_WARN("Physics: the simulation ran out of capacity (error {}); some contacts are ignored until the scene has fewer", static_cast<int>(error));
				}
				Steps++;
				UpdateContacts(Contacts.Take());
				for (const JPH::BodyID& body : movedThisStep)
					Bodies().SetLinearAndAngularVelocity(body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
				if (fixedUpdates)
					WriteBack(scene);
			}
			// MoveKinematic gives a body the velocity to reach its target over these steps; without stopping it there, it
			// would keep drifting in the simulation while the scene shows it at the target.
			for (const JPH::BodyID& body : movedKinematic)
				Bodies().SetLinearAndAngularVelocity(body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
			if (steps > 0 && !fixedUpdates)
				WriteBack(scene);
		}
	};

	PhysicsSystem::PhysicsSystem(PhysicsSystemSpecification specification)
		: m_Impl(CreateScope<Impl>(specification))
	{
		PX_CORE_VERIFY(std::isfinite(specification.FixedTimestep) && specification.FixedTimestep > 0.0f && specification.FixedTimestep <= 1.0f, "The physics time step must be in (0, 1] seconds");
		PX_CORE_VERIFY(specification.MaxStepsPerUpdate > 0 && specification.MaxStepsPerUpdate <= 1000, "MaxStepsPerUpdate must be in [1, 1000]");
		PX_CORE_VERIFY(IsFinite(specification.Gravity), "Gravity must be finite");
		PX_CORE_VERIFY(specification.MaxCollidersPerBody >= 1, "MaxCollidersPerBody must be at least 1");
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
		m_Impl->StepForces.erase(id);
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
		(m_Impl->InFixedUpdate ? m_Impl->StepForces : m_Impl->PendingForces)[entity] += force;
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

	uint64_t PhysicsSystem::GetBodiesCreated() const
	{
		return m_Impl->BodiesCreated;
	}

	uint64_t PhysicsSystem::GetStepCount() const
	{
		return m_Impl->Steps;
	}

	bool PhysicsSystem::JoltAssertionsEnabled()
	{
#ifdef JPH_ENABLE_ASSERTS
		return true;
#else
		return false;
#endif
	}

	bool PhysicsSystem::RaiseJoltAssertion([[maybe_unused]] const char* expression, [[maybe_unused]] const char* message)
	{
#ifdef JPH_ENABLE_ASSERTS
		// Through JPH::AssertFailed, so this exercises the handler actually installed.
		EnsureJoltInitialized();
		return JPH::AssertFailed(expression, message, __FILE__, __LINE__);
#else
		return false;
#endif
	}

}
