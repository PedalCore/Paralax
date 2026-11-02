#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Physics/PhysicsSystem.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <functional>
#include <vector>

using namespace Paralax;

namespace {

	constexpr float Step = 1.0f / 60.0f;

	struct PhysicsFixture
	{
		explicit PhysicsFixture(PhysicsSystemSpecification specification = {})
		{
			Physics = static_cast<PhysicsSystem*>(World.AddSystem(CreateScope<PhysicsSystem>(specification)));
		}
		~PhysicsFixture() { World.OnRuntimeStop(); }

		Entity Ground()
		{
			Entity ground = World.CreateEntity("Ground");
			ground.AddComponent<ColliderComponent>().Size = glm::vec3(20.0f, 1.0f, 20.0f);
			ground.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -0.5f, 0.0f); // Top surface at y = 0.
			return ground;
		}

		Entity Body(const std::string& name, glm::vec3 position, ColliderShape shape = ColliderShape::Box, RigidbodyType type = RigidbodyType::Dynamic)
		{
			Entity entity = World.CreateEntity(name);
			entity.GetComponent<TransformComponent>().Translation = position;
			entity.AddComponent<ColliderComponent>().Shape = shape;
			entity.AddComponent<RigidbodyComponent>().Type = type;
			return entity;
		}

		void Run(float seconds)
		{
			const int frames = static_cast<int>(std::lround(seconds / Step));
			for (int i = 0; i < frames; i++)
				World.OnUpdateRuntime(Timestep(Step));
		}

		Scene World{"PhysicsTest"};
		PhysicsSystem* Physics = nullptr;
	};

	// Runs a function from every fixed update (before each physics step).
	class FixedUpdateHook final : public SceneSystem
	{
	public:
		explicit FixedUpdateHook(std::function<void(Scene&, float)> function)
			: m_Function(std::move(function))
		{
		}
		const char* GetName() const override { return "FixedUpdateHook"; }
		void OnFixedUpdate(Scene& scene, Timestep step) override { m_Function(scene, step.GetSeconds()); }
		bool WantsFixedUpdate() const override { return Wanted; }

		bool Wanted = true;

	private:
		std::function<void(Scene&, float)> m_Function;
	};

	glm::vec3 PositionOf(Entity entity)
	{
		return entity.GetComponent<TransformComponent>().Translation;
	}

	bool IsFinite(const glm::vec3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	size_t CountLogs(const Paralax::Testing::ScopedLogCapture& log, const std::string& substring)
	{
		size_t count = 0;
		for (const auto& entry : log.GetEntries())
			count += entry.Message.find(substring) != std::string::npos ? 1 : 0;
		return count;
	}

	bool HasEvent(const std::vector<ContactEvent>& events, ContactEventType type, UUID a, UUID b)
	{
		for (const ContactEvent& event : events)
		{
			if (event.Type == type && ((event.A == a && event.B == b) || (event.A == b && event.B == a)))
				return true;
		}
		return false;
	}

}

TEST_CASE("PhysicsSystem: a dynamic box falls under gravity and comes to rest on the ground")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity box = fixture.Body("Box", glm::vec3(0.0f, 5.0f, 0.0f));
	fixture.World.OnRuntimeStart();
	CHECK(fixture.Physics->HasBody(box.GetUUID()));

	fixture.Run(0.5f);
	const float midFall = PositionOf(box).y;
	CHECK(midFall < 5.0f);
	CHECK(midFall > 0.5f);
	// Free fall for half a second: y = 5 - g t^2 / 2 (discrete integration is close, not exact).
	CHECK(midFall == doctest::Approx(5.0 - 0.5 * 9.81 * 0.25).epsilon(0.05));

	fixture.Run(3.0f);
	CHECK(PositionOf(box).y == doctest::Approx(0.5).epsilon(0.02)); // Resting on top of the ground (half extent 0.5).
	CHECK(glm::length(*fixture.Physics->GetLinearVelocity(box.GetUUID())) < 0.05f);
	CHECK(fixture.Physics->GetStepCount() == 210);
}

TEST_CASE("PhysicsSystem: simulation is identical across runs and worker thread counts")
{
	auto simulate = [](uint32_t threads) {
		PhysicsSystemSpecification specification;
		specification.WorkerThreads = threads;
		PhysicsFixture fixture(specification);
		fixture.Ground();
		std::vector<Entity> bodies;
		for (int i = 0; i < 12; i++)
		{
			const ColliderShape shape = static_cast<ColliderShape>(i % 4);
			bodies.push_back(fixture.Body("B" + std::to_string(i), glm::vec3(0.3f * static_cast<float>(i % 3), 1.0f + 1.2f * static_cast<float>(i), 0.2f * static_cast<float>(i % 2)), shape));
		}
		fixture.World.OnRuntimeStart();
		fixture.Run(2.0f);
		std::vector<glm::vec3> positions;
		for (Entity body : bodies)
			positions.push_back(PositionOf(body));
		return positions;
	};
	const std::vector<glm::vec3> reference = simulate(0);
	CHECK(simulate(0) == reference);
	CHECK(simulate(3) == reference);
}

TEST_CASE("PhysicsSystem: static, kinematic and trigger bodies behave as their type says")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity wall = fixture.Body("Wall", glm::vec3(0.0f, 3.0f, 0.0f), ColliderShape::Box, RigidbodyType::Static);
	Entity platform = fixture.Body("Platform", glm::vec3(5.0f, 3.0f, 0.0f), ColliderShape::Box, RigidbodyType::Kinematic);
	Entity zone = fixture.World.CreateEntity("Zone");
	zone.AddComponent<ColliderComponent>().IsTrigger = true;
	zone.GetComponent<TransformComponent>().Translation = glm::vec3(-5.0f, 0.5f, 0.0f);
	Entity faller = fixture.Body("Faller", glm::vec3(-5.0f, 4.0f, 0.0f), ColliderShape::Sphere);
	fixture.World.OnRuntimeStart();

	fixture.Run(1.0f);
	CHECK(PositionOf(wall) == glm::vec3(0.0f, 3.0f, 0.0f));     // Static: unmoved by gravity.
	CHECK(PositionOf(platform) == glm::vec3(5.0f, 3.0f, 0.0f)); // Kinematic: unmoved by gravity.

	// Kinematic bodies follow their transform; the simulation keeps the position the scene asked for.
	platform.GetComponent<TransformComponent>().Translation = glm::vec3(6.0f, 3.0f, 0.0f);
	fixture.Run(0.1f);
	CHECK(glm::all(glm::epsilonEqual(PositionOf(platform), glm::vec3(6.0f, 3.0f, 0.0f), 1e-4f)));
	// The simulated body stops at the target too (it does not keep the velocity that moved it there).
	fixture.Run(1.0f);
	std::optional<RaycastHit> onPlatform = fixture.Physics->Raycast(glm::vec3(6.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
	REQUIRE(onPlatform);
	CHECK(onPlatform->Entity == platform.GetUUID());
	CHECK(onPlatform->Distance == doctest::Approx(6.5).epsilon(0.001));
	CHECK(glm::length(*fixture.Physics->GetLinearVelocity(platform.GetUUID())) < 1e-5f);

	// The faller passes through the trigger and lands on the ground.
	fixture.Run(2.0f);
	CHECK(PositionOf(faller).y == doctest::Approx(0.5).epsilon(0.02));
	bool entered = false;
	for (const ContactEvent& event : fixture.Physics->TakeContactEvents())
		entered |= event.Type == ContactEventType::TriggerEnter && (event.A == zone.GetUUID() || event.B == zone.GetUUID());
	CHECK(entered);
}

TEST_CASE("PhysicsSystem: contact changes are reported per pair, alternating enter and exit")
{
	PhysicsFixture fixture;
	Entity ground = fixture.Ground();
	Entity box = fixture.Body("Box", glm::vec3(0.0f, 2.0f, 0.0f));
	fixture.World.OnRuntimeStart();
	fixture.Run(2.0f);
	// The impact may bounce briefly (enter, exit, enter): every change is reported, and the box ends at rest in contact.
	std::vector<ContactEvent> events = fixture.Physics->TakeContactEvents();
	REQUIRE_FALSE(events.empty());
	CHECK(events.size() % 2 == 1);
	const uint64_t low = std::min<uint64_t>(ground.GetUUID(), box.GetUUID());
	for (size_t i = 0; i < events.size(); i++)
	{
		CAPTURE(i);
		CHECK(events[i].Type == (i % 2 == 0 ? ContactEventType::CollisionEnter : ContactEventType::CollisionExit));
		CHECK(static_cast<uint64_t>(events[i].A) == low);
	}
	fixture.Run(1.0f);
	CHECK(fixture.Physics->TakeContactEvents().empty()); // At rest: no further changes.

	// Lifting the box off the ground ends the contact.
	box.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 10.0f, 0.0f);
	fixture.Run(0.1f);
	events = fixture.Physics->TakeContactEvents();
	REQUIRE(events.size() == 1);
	CHECK(events[0].Type == ContactEventType::CollisionExit);
}

TEST_CASE("PhysicsSystem: forces, impulses and velocities act on dynamic bodies only")
{
	PhysicsSystemSpecification specification;
	specification.Gravity = glm::vec3(0.0f);
	PhysicsFixture fixture(specification);
	Entity body = fixture.Body("Body", glm::vec3(0.0f));
	body.GetComponent<RigidbodyComponent>().Mass = 2.0f;
	Entity wall = fixture.Body("Wall", glm::vec3(50.0f, 0.0f, 0.0f), ColliderShape::Box, RigidbodyType::Static);
	Entity mover = fixture.Body("Mover", glm::vec3(-50.0f, 0.0f, 0.0f), ColliderShape::Box, RigidbodyType::Kinematic);
	fixture.World.OnRuntimeStart();

	CHECK(fixture.Physics->AddImpulse(body.GetUUID(), glm::vec3(4.0f, 0.0f, 0.0f))); // v = J / m = 2 m/s
	fixture.Run(1.0f);
	CHECK(fixture.Physics->GetLinearVelocity(body.GetUUID())->x == doctest::Approx(2.0 * std::pow(1.0 - 0.05 * static_cast<double>(Step), 60.0)).epsilon(0.02));
	CHECK(PositionOf(body).x == doctest::Approx(1.95).epsilon(0.05));

	CHECK(fixture.Physics->SetLinearVelocity(body.GetUUID(), glm::vec3(0.0f, 3.0f, 0.0f)));
	CHECK(fixture.Physics->GetLinearVelocity(body.GetUUID())->y == doctest::Approx(3.0));
	CHECK(fixture.Physics->SetAngularVelocity(body.GetUUID(), glm::vec3(0.0f, 1.0f, 0.0f)));
	CHECK(fixture.Physics->GetAngularVelocity(body.GetUUID())->y == doctest::Approx(1.0));
	CHECK(fixture.Physics->AddForce(body.GetUUID(), glm::vec3(0.0f, 0.0f, 10.0f)));

	CHECK_FALSE(fixture.Physics->AddImpulse(wall.GetUUID(), glm::vec3(1.0f)));
	CHECK_FALSE(fixture.Physics->AddForce(wall.GetUUID(), glm::vec3(1.0f)));
	CHECK_FALSE(fixture.Physics->SetLinearVelocity(wall.GetUUID(), glm::vec3(1.0f)));
	// Kinematic bodies follow their Transform: velocities and forces set from outside would make them drift from it.
	CHECK_FALSE(fixture.Physics->SetLinearVelocity(mover.GetUUID(), glm::vec3(1.0f)));
	CHECK_FALSE(fixture.Physics->SetAngularVelocity(mover.GetUUID(), glm::vec3(1.0f)));
	CHECK_FALSE(fixture.Physics->AddForce(mover.GetUUID(), glm::vec3(1.0f)));
	CHECK_FALSE(fixture.Physics->AddImpulse(mover.GetUUID(), glm::vec3(1.0f)));
	CHECK(fixture.Physics->GetLinearVelocity(mover.GetUUID()) == glm::vec3(0.0f));
	CHECK_FALSE(fixture.Physics->AddImpulse(UUID(12345), glm::vec3(1.0f)));
	CHECK_FALSE(fixture.Physics->GetLinearVelocity(UUID(12345)));
	CHECK_FALSE(fixture.Physics->AddImpulse(body.GetUUID(), glm::vec3(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("PhysicsSystem: raycasts report the nearest solid collider and ignore triggers")
{
	PhysicsFixture fixture;
	Entity ground = fixture.Ground();
	Entity zone = fixture.World.CreateEntity("Zone");
	zone.AddComponent<ColliderComponent>().IsTrigger = true;
	zone.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 3.0f, 0.0f);
	fixture.World.OnRuntimeStart();

	std::optional<RaycastHit> hit = fixture.Physics->Raycast(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -2.0f, 0.0f), 100.0f);
	REQUIRE(hit);
	CHECK(hit->Entity == ground.GetUUID());
	CHECK(hit->Distance == doctest::Approx(10.0).epsilon(0.001));
	CHECK(glm::all(glm::epsilonEqual(hit->Point, glm::vec3(0.0f), 1e-3f)));
	CHECK(glm::all(glm::epsilonEqual(hit->Normal, glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f)));

	CHECK_FALSE(fixture.Physics->Raycast(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 5.0f));  // Too short.
	CHECK_FALSE(fixture.Physics->Raycast(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f)); // Away.
	CHECK_FALSE(fixture.Physics->Raycast(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), 100.0f));
	CHECK_FALSE(fixture.Physics->Raycast(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), -1.0f));
}

TEST_CASE("PhysicsSystem: bodies follow the scene as colliders and entities change")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity box = fixture.Body("Box", glm::vec3(0.0f, 3.0f, 0.0f));
	fixture.World.OnRuntimeStart();
	fixture.Run(0.1f);

	// Teleport by setting the transform.
	box.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 3.0f, 0.0f);
	fixture.Run(Step);
	CHECK(PositionOf(box).x == doctest::Approx(5.0).epsilon(0.001));

	// Changing the collider rebuilds the body (here, a bigger box rests higher).
	box.GetComponent<ColliderComponent>().Size = glm::vec3(2.0f);
	fixture.Run(3.0f);
	CHECK(PositionOf(box).y == doctest::Approx(1.0).epsilon(0.02));

	// Removing the collider removes the body; destroying the entity too.
	box.RemoveComponent<ColliderComponent>();
	fixture.Run(Step);
	CHECK_FALSE(fixture.Physics->HasBody(box.GetUUID()));
	Entity other = fixture.Body("Other", glm::vec3(0.0f, 5.0f, 0.0f));
	fixture.Run(Step);
	CHECK(fixture.Physics->HasBody(other.GetUUID()));
	const UUID otherId = other.GetUUID();
	fixture.World.DestroyEntity(other);
	CHECK_FALSE(fixture.Physics->HasBody(otherId));
}

TEST_CASE("PhysicsSystem: scale, offsets and child bodies are respected")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity parent = fixture.World.CreateEntity("Parent");
	parent.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
	Entity box = fixture.Body("Box", glm::vec3(0.0f, 4.0f, 0.0f));
	box.GetComponent<TransformComponent>().Scale = glm::vec3(3.0f);
	fixture.World.SetParent(box, parent, false);
	Entity offset = fixture.Body("Offset", glm::vec3(-5.0f, 4.0f, 0.0f), ColliderShape::Sphere);
	offset.GetComponent<ColliderComponent>().Offset = glm::vec3(0.0f, -1.0f, 0.0f);
	fixture.World.OnRuntimeStart();
	fixture.Run(4.0f);

	// The parent stays put; the child (scaled to 3 m) rests on the ground in world space.
	CHECK(PositionOf(parent) == glm::vec3(5.0f, 0.0f, 0.0f));
	const glm::vec3 world = glm::vec3(fixture.World.GetWorldTransform(box)[3]);
	CHECK(world.y == doctest::Approx(1.5).epsilon(0.02));
	CHECK(world.x == doctest::Approx(5.0).epsilon(0.01));
	// The offset sphere's shape sits 1 m below the entity, so the entity rests 1.5 m up.
	CHECK(PositionOf(offset).y == doctest::Approx(1.5).epsilon(0.02));
}

TEST_CASE("PhysicsSystem: hostile collider values are clamped instead of crashing")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity flat = fixture.Body("Flat", glm::vec3(0.0f, 2.0f, 0.0f));
	flat.GetComponent<ColliderComponent>().Size = glm::vec3(0.0f, -5.0f, std::numeric_limits<float>::infinity());
	Entity nan = fixture.Body("NaN", glm::vec3(std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.0f));
	Entity heavy = fixture.Body("Heavy", glm::vec3(3.0f, 2.0f, 0.0f), ColliderShape::Capsule);
	heavy.GetComponent<RigidbodyComponent>().Mass = -5.0f;
	Paralax::Testing::ScopedLogCapture log;
	fixture.World.OnRuntimeStart();
	CHECK_NOTHROW(fixture.Run(1.0f));
	CHECK(fixture.Physics->HasBody(flat.GetUUID()));
	CHECK_FALSE(fixture.Physics->HasBody(nan.GetUUID()));
	CHECK(fixture.Physics->HasBody(heavy.GetUUID()));
}

TEST_CASE("PhysicsSystem: time is accumulated into fixed steps and capped per update")
{
	PhysicsSystemSpecification specification;
	specification.MaxStepsPerUpdate = 4;
	PhysicsFixture fixture(specification);
	fixture.World.OnRuntimeStart();
	fixture.World.OnUpdateRuntime(Timestep(Step * 0.5f));
	CHECK(fixture.Physics->GetStepCount() == 0);
	fixture.World.OnUpdateRuntime(Timestep(Step * 0.6f));
	CHECK(fixture.Physics->GetStepCount() == 1);
	fixture.World.OnUpdateRuntime(Timestep(10.0f)); // A huge hitch: at most 4 steps, the rest dropped.
	CHECK(fixture.Physics->GetStepCount() == 5);
	fixture.World.OnUpdateRuntime(Timestep(Step));
	CHECK(fixture.Physics->GetStepCount() == 6);
	fixture.World.OnUpdateRuntime(Timestep(1e30f)); // Absurd and invalid times are capped or ignored.
	CHECK(fixture.Physics->GetStepCount() == 10);
	fixture.World.OnUpdateRuntime(Timestep(std::numeric_limits<float>::infinity()));
	fixture.World.OnUpdateRuntime(Timestep(-1.0f));
	CHECK(fixture.Physics->GetStepCount() == 10);

	fixture.Physics->SetGravity(glm::vec3(0.0f, -1.0f, 0.0f));
	CHECK(fixture.Physics->GetGravity() == glm::vec3(0.0f, -1.0f, 0.0f));
	fixture.Physics->SetGravity(glm::vec3(std::numeric_limits<float>::quiet_NaN()));
	CHECK(fixture.Physics->GetGravity() == glm::vec3(0.0f, -1.0f, 0.0f));
}

TEST_CASE("PhysicsSystem: invalid specifications fail verification")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	PhysicsSystemSpecification zeroStep;
	zeroStep.FixedTimestep = 0.0f;
	CHECK_THROWS_AS(PhysicsSystem{zeroStep}, Paralax::Testing::AssertionFailedError);
	PhysicsSystemSpecification noSteps;
	noSteps.MaxStepsPerUpdate = 0;
	CHECK_THROWS_AS(PhysicsSystem{noSteps}, Paralax::Testing::AssertionFailedError);
	PhysicsSystemSpecification tooManySteps;
	tooManySteps.MaxStepsPerUpdate = 1001;
	CHECK_THROWS_AS(PhysicsSystem{tooManySteps}, Paralax::Testing::AssertionFailedError);
	PhysicsSystemSpecification longStep;
	longStep.FixedTimestep = 2.0f;
	CHECK_THROWS_AS(PhysicsSystem{longStep}, Paralax::Testing::AssertionFailedError);
	PhysicsSystemSpecification nanStep;
	nanStep.FixedTimestep = std::numeric_limits<float>::quiet_NaN();
	CHECK_THROWS_AS(PhysicsSystem{nanStep}, Paralax::Testing::AssertionFailedError);
	PhysicsSystemSpecification badGravity;
	badGravity.Gravity = glm::vec3(0.0f, std::numeric_limits<float>::infinity(), 0.0f);
	CHECK_THROWS_AS(PhysicsSystem{badGravity}, Paralax::Testing::AssertionFailedError);
}

TEST_CASE("PhysicsSystem: an entity with a zero-scale or non-finite transform has no body until it is fixed")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity group = fixture.World.CreateEntity("Group");
	Entity box = fixture.Body("Box", glm::vec3(0.0f, 3.0f, 0.0f));
	fixture.World.SetParent(box, group, false);
	Entity other = fixture.Body("Other", glm::vec3(4.0f, 3.0f, 0.0f));
	fixture.World.OnRuntimeStart();
	fixture.Run(0.1f);

	// Hiding the group by scaling it to zero removes the body (rather than simulating a singular transform).
	Paralax::Testing::ScopedLogCapture log;
	group.GetComponent<TransformComponent>().Scale = glm::vec3(0.0f);
	other.GetComponent<TransformComponent>().Translation.x = std::numeric_limits<float>::infinity();
	fixture.Run(0.5f);
	CHECK_FALSE(fixture.Physics->HasBody(box.GetUUID()));
	CHECK_FALSE(fixture.Physics->HasBody(other.GetUUID()));
	CHECK(IsFinite(PositionOf(box)));
	CHECK(CountLogs(log, "zero-scale or non-finite") == 2); // Once per entity, not every update.

	// Restored, it is simulated again from where its transform says.
	group.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f);
	other.GetComponent<TransformComponent>().Translation.x = 4.0f;
	fixture.Run(3.0f);
	CHECK(fixture.Physics->HasBody(box.GetUUID()));
	CHECK(fixture.Physics->HasBody(other.GetUUID()));
	CHECK(PositionOf(box).y == doctest::Approx(0.5).epsilon(0.02));
	CHECK(PositionOf(other).y == doctest::Approx(0.5).epsilon(0.02));
}

TEST_CASE("PhysicsSystem: sleeping bodies wake and fall when their support moves or disappears")
{
	PhysicsFixture fixture;
	fixture.Ground();
	auto shelf = [&](const std::string& name, float x) {
		Entity entity = fixture.World.CreateEntity(name);
		entity.AddComponent<ColliderComponent>().Size = glm::vec3(2.0f, 0.2f, 2.0f);
		entity.GetComponent<TransformComponent>().Translation = glm::vec3(x, 2.0f, 0.0f); // Top at y = 2.1.
		return entity;
	};
	Entity moved = shelf("Moved", 0.0f);
	Entity removed = shelf("Removed", 5.0f);
	Entity first = fixture.Body("First", glm::vec3(0.0f, 3.0f, 0.0f));
	Entity second = fixture.Body("Second", glm::vec3(5.0f, 3.0f, 0.0f));
	fixture.World.OnRuntimeStart();
	fixture.Run(3.0f); // Long enough to come to rest and fall asleep.
	REQUIRE(PositionOf(first).y == doctest::Approx(2.6).epsilon(0.02));
	REQUIRE(PositionOf(second).y == doctest::Approx(2.6).epsilon(0.02));
	fixture.Physics->TakeContactEvents();

	moved.GetComponent<TransformComponent>().Translation.x = 20.0f;
	const UUID removedId = removed.GetUUID();
	fixture.World.DestroyEntity(removed);
	fixture.Run(2.0f);
	CHECK(PositionOf(first).y == doctest::Approx(0.5).epsilon(0.02));
	CHECK(PositionOf(second).y == doctest::Approx(0.5).epsilon(0.02));
	const std::vector<ContactEvent> events = fixture.Physics->TakeContactEvents();
	CHECK(HasEvent(events, ContactEventType::CollisionExit, first.GetUUID(), moved.GetUUID()));
	CHECK(HasEvent(events, ContactEventType::CollisionExit, second.GetUUID(), removedId));
}

TEST_CASE("PhysicsSystem: a force gives the same result at any frame rate")
{
	for (const int framesPerSecond : {30, 45, 60, 144})
	{
		CAPTURE(framesPerSecond);
		PhysicsSystemSpecification specification;
		specification.Gravity = glm::vec3(0.0f);
		PhysicsFixture fixture(specification);
		Entity body = fixture.Body("Body", glm::vec3(0.0f));
		auto& rigidbody = body.GetComponent<RigidbodyComponent>();
		rigidbody.Mass = 2.0f;
		rigidbody.LinearDamping = 0.0f;
		fixture.World.OnRuntimeStart();

		// 4 N on 2 kg for one second: v = F t / m = 2 m/s. At most the last step's share may still be pending.
		const float frame = 1.0f / static_cast<float>(framesPerSecond);
		for (int i = 0; i < framesPerSecond; i++)
		{
			CHECK(fixture.Physics->AddForce(body.GetUUID(), glm::vec3(4.0f, 0.0f, 0.0f)));
			fixture.World.OnUpdateRuntime(Timestep(frame));
		}
		CHECK(fixture.Physics->GetLinearVelocity(body.GetUUID())->x == doctest::Approx(2.0).epsilon(0.035));
	}
}

TEST_CASE("PhysicsSystem: a kinematic platform carries what rests on it at any frame rate")
{
	for (const int framesPerSecond : {60, 144})
	{
		CAPTURE(framesPerSecond);
		PhysicsFixture fixture;
		Entity platform = fixture.Body("Platform", glm::vec3(0.0f, 1.0f, 0.0f), ColliderShape::Box, RigidbodyType::Kinematic);
		platform.GetComponent<ColliderComponent>().Size = glm::vec3(4.0f, 0.2f, 4.0f);
		// Frames do not line up with the 60 Hz steps, so at 144 fps the platform moves by two or three frames' worth per
		// step; the rider holds on through that only with enough grip (here mu = 5).
		platform.GetComponent<ColliderComponent>().Friction = 5.0f;
		Entity rider = fixture.Body("Rider", glm::vec3(0.0f, 1.6f, 0.0f));
		rider.GetComponent<ColliderComponent>().Friction = 5.0f;
		fixture.World.OnRuntimeStart();
		fixture.Run(1.0f);
		REQUIRE(PositionOf(rider).y == doctest::Approx(1.6).epsilon(0.02));

		// One metre along x in one second, moved a little every frame.
		const float frame = 1.0f / static_cast<float>(framesPerSecond);
		for (int i = 0; i < framesPerSecond; i++)
		{
			platform.GetComponent<TransformComponent>().Translation.x += frame;
			fixture.World.OnUpdateRuntime(Timestep(frame));
		}
		CHECK(PositionOf(rider).x == doctest::Approx(1.0).epsilon(0.05));
		CHECK(PositionOf(rider).y == doctest::Approx(1.6).epsilon(0.02));
	}
}

TEST_CASE("PhysicsSystem: fixed updates run once before each step, with the step's length")
{
	for (const int framesPerSecond : {20, 60, 144})
	{
		CAPTURE(framesPerSecond);
		PhysicsFixture fixture;
		std::vector<float> steps;
		fixture.World.AddSystem(CreateScope<FixedUpdateHook>([&](Scene&, float step) { steps.push_back(step); }));
		fixture.World.OnRuntimeStart();
		const float frame = 1.0f / static_cast<float>(framesPerSecond);
		for (int i = 0; i < framesPerSecond; i++)
		{
			const uint64_t before = fixture.Physics->GetStepCount();
			const size_t calls = steps.size();
			fixture.World.OnUpdateRuntime(Timestep(frame));
			CHECK(steps.size() - calls == fixture.Physics->GetStepCount() - before);
		}
		CHECK(steps.size() == fixture.Physics->GetStepCount());
		CHECK(std::abs(static_cast<int>(steps.size()) - 60) <= 1);
		CHECK(std::all_of(steps.begin(), steps.end(), [](float step) { return step == Step; }));
	}
}

TEST_CASE("PhysicsSystem: a fixed update sees the previous step's result, and what it changes counts in the next step")
{
	PhysicsSystemSpecification specification;
	specification.Gravity = glm::vec3(0.0f, -10.0f, 0.0f);
	PhysicsFixture fixture(specification);
	Entity body = fixture.Body("Body", glm::vec3(0.0f, 10.0f, 0.0f));
	body.GetComponent<RigidbodyComponent>().LinearDamping = 0.0f;
	std::vector<float> heights;
	bool teleport = false;
	fixture.World.AddSystem(CreateScope<FixedUpdateHook>([&](Scene&, float) {
		heights.push_back(PositionOf(body).y);
		if (teleport)
		{
			body.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 20.0f, 0.0f);
			teleport = false;
		}
	}));
	fixture.World.OnRuntimeStart();

	// 20 fps: three steps per update, each seeing the one before it written back.
	fixture.World.OnUpdateRuntime(Timestep(3.0f * Step));
	REQUIRE(heights.size() == 3);
	CHECK(heights[0] == 10.0f);
	CHECK(heights[1] < heights[0]);
	CHECK(heights[2] < heights[1]);

	// A move made in a fixed update is synced before that step, so the step starts from there.
	teleport = true;
	fixture.World.OnUpdateRuntime(Timestep(Step));
	CHECK(PositionOf(body).x == doctest::Approx(5.0));
	CHECK(PositionOf(body).y < 20.0f);
	CHECK(PositionOf(body).y > 19.9f);
}

TEST_CASE("PhysicsSystem: a force added in a fixed update acts for that step, the same at any frame rate")
{
	std::vector<float> velocities;
	for (const int framesPerSecond : {20, 60, 144})
	{
		CAPTURE(framesPerSecond);
		PhysicsSystemSpecification specification;
		specification.Gravity = glm::vec3(0.0f);
		PhysicsFixture fixture(specification);
		Entity body = fixture.Body("Body", glm::vec3(0.0f));
		auto& rigidbody = body.GetComponent<RigidbodyComponent>();
		rigidbody.Mass = 2.0f;
		rigidbody.LinearDamping = 0.0f;
		int pushes = 0;
		fixture.World.AddSystem(CreateScope<FixedUpdateHook>([&](Scene&, float) {
			if (pushes++ < 60)
				CHECK(fixture.Physics->AddForce(body.GetUUID(), glm::vec3(4.0f, 0.0f, 0.0f)));
		}));
		fixture.World.OnRuntimeStart();
		// 4 N on 2 kg for 60 steps of 1/60 s: v = F t / m = 2 m/s, delivered completely (nothing waits for a frame).
		const float frame = 1.0f / static_cast<float>(framesPerSecond);
		while (pushes <= 60)
			fixture.World.OnUpdateRuntime(Timestep(frame));
		velocities.push_back(fixture.Physics->GetLinearVelocity(body.GetUUID())->x);
		CHECK(velocities.back() == doctest::Approx(2.0).epsilon(1e-4));
	}
	// Steps see identical inputs whatever the frame rate, so the results are identical too.
	CHECK(velocities[0] == velocities[1]);
	CHECK(velocities[1] == velocities[2]);
}

TEST_CASE("PhysicsSystem: a kinematic body moved in a fixed update gets there in that step, and stops there")
{
	PhysicsFixture fixture;
	Entity platform = fixture.Body("Platform", glm::vec3(0.0f, 1.0f, 0.0f), ColliderShape::Box, RigidbodyType::Kinematic);
	platform.GetComponent<ColliderComponent>().Size = glm::vec3(4.0f, 0.2f, 4.0f);
	platform.GetComponent<ColliderComponent>().Friction = 1.0f;
	Entity rider = fixture.Body("Rider", glm::vec3(0.0f, 1.6f, 0.0f));
	rider.GetComponent<ColliderComponent>().Friction = 1.0f;
	bool moving = false;
	int moves = 0;
	fixture.World.AddSystem(CreateScope<FixedUpdateHook>([&](Scene&, float step) {
		if (moving && moves < 60)
		{
			platform.GetComponent<TransformComponent>().Translation.x += step; // 1 m/s.
			moves++;
		}
	}));
	fixture.World.OnRuntimeStart();
	fixture.Run(1.0f);
	REQUIRE(PositionOf(rider).y == doctest::Approx(1.6).epsilon(0.02));

	// At 20 fps (three steps per update), moved one step's worth before each step.
	moving = true;
	for (int i = 0; i < 20; i++)
		fixture.World.OnUpdateRuntime(Timestep(3.0f * Step));
	REQUIRE(moves == 60);
	CHECK(PositionOf(rider).x == doctest::Approx(1.0).epsilon(0.05));
	// It moved at 1 m/s while carried, and is at rest once the moves stop.
	CHECK(glm::length(*fixture.Physics->GetLinearVelocity(platform.GetUUID())) == doctest::Approx(0.0));
	fixture.Run(0.5f);
	CHECK(PositionOf(rider).x == doctest::Approx(1.0).epsilon(0.05));
}

TEST_CASE("PhysicsSystem: an entity destroyed in a fixed update leaves the simulation at once, and the scene when the update ends")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity body = fixture.Body("Body", glm::vec3(0.0f, 5.0f, 0.0f));
	Entity child = fixture.World.CreateEntity("Child");
	child.AddComponent<ColliderComponent>().IsTrigger = true; // A body of its own, under the doomed one.
	fixture.World.SetParent(child, body, false);
	const UUID id = body.GetUUID();
	const UUID childId = child.GetUUID();
	std::vector<std::pair<bool, bool>> seen; // Entity exists, body exists.
	fixture.World.AddSystem(CreateScope<FixedUpdateHook>([&](Scene& scene, float) {
		if (seen.empty())
		{
			REQUIRE(fixture.Physics->HasBody(id));
			REQUIRE(fixture.Physics->HasBody(childId));
			scene.DestroyEntity(body);
		}
		seen.emplace_back(scene.TryGetEntityByUUID(id).has_value(), fixture.Physics->HasBody(id) || fixture.Physics->HasBody(childId));
	}));
	fixture.World.OnRuntimeStart();
	fixture.World.OnUpdateRuntime(Timestep(3.0f * Step));
	// Destroyed in the first call, before its step: from that step on it has no body, though the entity stays (and
	// can be inspected) until the update finishes.
	CHECK(seen == std::vector<std::pair<bool, bool>>{{true, true}, {true, false}, {true, false}});
	CHECK_FALSE(fixture.World.TryGetEntityByUUID(id).has_value());
	CHECK_FALSE(fixture.Physics->HasBody(id));
	fixture.World.OnUpdateRuntime(Timestep(Step));
}

TEST_CASE("PhysicsSystem: systems that do not want fixed updates get none")
{
	PhysicsFixture fixture;
	int calls = 0;
	auto* hook = static_cast<FixedUpdateHook*>(fixture.World.AddSystem(CreateScope<FixedUpdateHook>([&](Scene&, float) { calls++; })));
	hook->Wanted = false;
	fixture.World.OnRuntimeStart();
	fixture.Run(0.5f);
	CHECK(calls == 0);
	CHECK(fixture.Physics->GetStepCount() == 30);
	hook->Wanted = true;
	fixture.Run(0.5f);
	CHECK(calls == 30);
}

TEST_CASE("PhysicsSystem: a rebuilt dynamic body keeps its velocity")
{
	PhysicsSystemSpecification specification;
	specification.Gravity = glm::vec3(0.0f);
	PhysicsFixture fixture(specification);
	Entity body = fixture.Body("Body", glm::vec3(0.0f), ColliderShape::Sphere);
	body.GetComponent<RigidbodyComponent>().LinearDamping = 0.0f;
	body.GetComponent<RigidbodyComponent>().AngularDamping = 0.0f;
	fixture.World.OnRuntimeStart();
	REQUIRE(fixture.Physics->SetLinearVelocity(body.GetUUID(), glm::vec3(3.0f, 0.0f, 0.0f)));
	REQUIRE(fixture.Physics->SetAngularVelocity(body.GetUUID(), glm::vec3(0.0f, 2.0f, 0.0f)));
	fixture.Run(0.1f);

	body.GetComponent<ColliderComponent>().Friction = 0.9f;
	body.GetComponent<RigidbodyComponent>().Mass = 5.0f;
	body.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
	fixture.Run(Step);
	CHECK(fixture.Physics->GetLinearVelocity(body.GetUUID())->x == doctest::Approx(3.0).epsilon(0.001));
	CHECK(fixture.Physics->GetAngularVelocity(body.GetUUID())->y == doctest::Approx(2.0).epsilon(0.001));
	// Becoming kinematic stops it: kinematic bodies only follow their Transform.
	body.GetComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
	fixture.Run(Step);
	CHECK(glm::length(*fixture.Physics->GetLinearVelocity(body.GetUUID())) < 1e-6f);
}

TEST_CASE("PhysicsSystem: triggers detect moving kinematic bodies, and a collider turned trigger ends its collision")
{
	PhysicsFixture fixture;
	Entity ground = fixture.Ground();
	Entity zone = fixture.World.CreateEntity("Zone");
	zone.AddComponent<ColliderComponent>().IsTrigger = true;
	zone.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 3.0f, 0.0f);
	Entity mover = fixture.Body("Mover", glm::vec3(0.0f, 3.0f, 0.0f), ColliderShape::Sphere, RigidbodyType::Kinematic);
	Entity box = fixture.Body("Box", glm::vec3(-5.0f, 2.0f, 0.0f));
	fixture.World.OnRuntimeStart();
	fixture.Run(2.0f);
	fixture.Physics->TakeContactEvents();

	for (int i = 0; i < 60; i++)
	{
		mover.GetComponent<TransformComponent>().Translation.x += 0.15f; // Through the zone and out the other side.
		fixture.World.OnUpdateRuntime(Timestep(Step));
	}
	std::vector<ContactEvent> events = fixture.Physics->TakeContactEvents();
	CHECK(HasEvent(events, ContactEventType::TriggerEnter, zone.GetUUID(), mover.GetUUID()));
	CHECK(HasEvent(events, ContactEventType::TriggerExit, zone.GetUUID(), mover.GetUUID()));

	// The resting box is touching the ground; making the ground a trigger ends the collision, starts a trigger
	// contact, and the box falls through.
	ground.GetComponent<ColliderComponent>().IsTrigger = true;
	fixture.Run(0.3f);
	events = fixture.Physics->TakeContactEvents();
	REQUIRE(events.size() >= 2);
	CHECK(events[0].Type == ContactEventType::CollisionExit);
	CHECK(events[1].Type == ContactEventType::TriggerEnter);
	CHECK(PositionOf(box).y < 0.3f);
}

TEST_CASE("PhysicsSystem: gravity scale, locked rotation, restitution and friction act as specified")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity floating = fixture.Body("Floating", glm::vec3(0.0f, 10.0f, 0.0f));
	floating.GetComponent<RigidbodyComponent>().GravityScale = 0.0f;
	Entity heavy = fixture.Body("Heavy", glm::vec3(2.0f, 10.0f, 0.0f));
	heavy.GetComponent<RigidbodyComponent>().GravityScale = 2.0f;
	Entity locked = fixture.Body("Locked", glm::vec3(4.0f, 10.0f, 0.0f));
	locked.GetComponent<RigidbodyComponent>().LockRotation = true;
	locked.GetComponent<RigidbodyComponent>().GravityScale = 0.0f;
	Entity bouncy = fixture.Body("Bouncy", glm::vec3(-4.0f, 3.0f, 0.0f), ColliderShape::Sphere);
	bouncy.GetComponent<ColliderComponent>().Restitution = 1.0f;
	bouncy.GetComponent<RigidbodyComponent>().LinearDamping = 0.0f;
	Entity dull = fixture.Body("Dull", glm::vec3(-6.0f, 3.0f, 0.0f), ColliderShape::Sphere);
	fixture.World.OnRuntimeStart();
	REQUIRE(fixture.Physics->SetAngularVelocity(locked.GetUUID(), glm::vec3(0.0f, 0.0f, 5.0f)));

	fixture.Run(0.5f);
	CHECK(PositionOf(floating).y == doctest::Approx(10.0).epsilon(1e-4));
	CHECK(PositionOf(heavy).y == doctest::Approx(10.0 - 0.5 * 9.81 * 2.0 * 0.25).epsilon(0.05)); // Twice the fall.
	CHECK(glm::all(glm::epsilonEqual(glm::eulerAngles(locked.GetComponent<TransformComponent>().Rotation), glm::vec3(0.0f), 1e-4f)));

	// Dropped from 3 m (2.5 m above the ground), the bouncy ball comes back up most of the way; the dull one stays down.
	// (Restitution combines as the larger of the two colliders'; the ground's is 0.) Both land at about 0.71 s; the
	// bouncy one's next apex is at about 1.43 s.
	fixture.Run(0.5f);
	float bouncyPeak = 0.0f;
	float dullPeak = 0.0f;
	for (int i = 0; i < 60; i++)
	{
		fixture.Run(Step);
		bouncyPeak = std::max(bouncyPeak, PositionOf(bouncy).y);
		dullPeak = std::max(dullPeak, PositionOf(dull).y);
	}
	CHECK(bouncyPeak > 2.5f);
	CHECK(dullPeak < 0.7f);
}

TEST_CASE("PhysicsSystem: friction slows sliding bodies")
{
	auto slide = [](float friction) {
		PhysicsFixture fixture;
		Entity ground = fixture.Ground();
		ground.GetComponent<ColliderComponent>().Friction = friction;
		Entity box = fixture.Body("Box", glm::vec3(0.0f, 0.5f, 0.0f));
		box.GetComponent<ColliderComponent>().Friction = friction;
		box.GetComponent<RigidbodyComponent>().LinearDamping = 0.0f;
		fixture.World.OnRuntimeStart();
		fixture.Run(0.2f);
		REQUIRE(fixture.Physics->SetLinearVelocity(box.GetUUID(), glm::vec3(3.0f, 0.0f, 0.0f)));
		fixture.Run(1.0f);
		return PositionOf(box).x;
	};
	// Without friction it keeps going (3 m in 1 s); with mu = 1 it stops after v^2 / (2 mu g) = 0.46 m.
	CHECK(slide(0.0f) == doctest::Approx(3.0).epsilon(0.03));
	CHECK(slide(1.0f) == doctest::Approx(9.0 / (2.0 * 9.81)).epsilon(0.1));
}

TEST_CASE("PhysicsSystem: collider shapes have the dimensions of their size")
{
	PhysicsFixture fixture;
	auto place = [&](const std::string& name, float x, ColliderShape shape, glm::vec3 size) {
		Entity entity = fixture.World.CreateEntity(name);
		auto& collider = entity.AddComponent<ColliderComponent>();
		collider.Shape = shape;
		collider.Size = size;
		entity.GetComponent<TransformComponent>().Translation = glm::vec3(x, 0.0f, 0.0f);
		return entity;
	};
	place("Box", 0.0f, ColliderShape::Box, glm::vec3(2.0f, 4.0f, 2.0f));            // Top at 2.
	place("Sphere", 10.0f, ColliderShape::Sphere, glm::vec3(3.0f, 1.0f, 1.0f));     // Radius from the largest side: 1.5.
	place("Capsule", 20.0f, ColliderShape::Capsule, glm::vec3(1.0f, 3.0f, 1.0f));   // 3 m tall including caps.
	place("Cylinder", 30.0f, ColliderShape::Cylinder, glm::vec3(2.0f, 1.0f, 2.0f)); // Radius 1, top at 0.5.
	fixture.World.OnRuntimeStart();

	auto topOf = [&](float x) {
		std::optional<RaycastHit> hit = fixture.Physics->Raycast(glm::vec3(x, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
		REQUIRE(hit);
		return 10.0f - hit->Distance;
	};
	CHECK(topOf(0.0f) == doctest::Approx(2.0).epsilon(0.001));
	CHECK(topOf(10.0f) == doctest::Approx(1.5).epsilon(0.001));
	CHECK(topOf(20.0f) == doctest::Approx(1.5).epsilon(0.001));
	CHECK(topOf(30.0f) == doctest::Approx(0.5).epsilon(0.001));
	std::optional<RaycastHit> side = fixture.Physics->Raycast(glm::vec3(25.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f);
	REQUIRE(side);
	CHECK(side->Point.x == doctest::Approx(29.0).epsilon(0.001));
}

TEST_CASE("PhysicsSystem: misconfigured entities are reported once")
{
	PhysicsSystemSpecification specification;
	specification.MaxBodies = 16;
	PhysicsFixture fixture(specification);
	Paralax::Testing::ScopedLogCapture log;
	Entity loose = fixture.World.CreateEntity("Loose");
	loose.AddComponent<RigidbodyComponent>();
	Entity car = fixture.Body("Car", glm::vec3(0.0f, 50.0f, 0.0f));
	Entity wheel = fixture.World.CreateEntity("Wheel");
	wheel.AddComponent<ColliderComponent>();
	fixture.World.SetParent(wheel, car, false);
	for (int i = 0; i < 18; i++)
		fixture.Body("Crowd" + std::to_string(i), glm::vec3(5.0f + 2.0f * static_cast<float>(i), 50.0f, 0.0f));
	fixture.World.OnRuntimeStart();
	fixture.Run(0.5f);

	CHECK_FALSE(fixture.Physics->HasBody(loose.GetUUID()));
	CHECK(CountLogs(log, "has a Rigidbody but no solid Collider") == 1);
	CHECK_FALSE(fixture.Physics->HasBody(wheel.GetUUID())); // Part of the car's body.
	CHECK(CountLogs(log, "body limit") == 3);               // 19 bodies wanted (the wheel is no body), 16 allowed: one report each.
}

TEST_CASE("PhysicsSystem: contact events beyond the limit are dropped with a warning")
{
	PhysicsSystemSpecification specification;
	specification.MaxPendingContactEvents = 2;
	PhysicsFixture fixture(specification);
	fixture.Ground();
	for (int i = 0; i < 5; i++)
		fixture.Body("Box" + std::to_string(i), glm::vec3(2.0f * static_cast<float>(i), 0.6f, 0.0f));
	Paralax::Testing::ScopedLogCapture log;
	fixture.World.OnRuntimeStart();
	fixture.Run(0.5f);
	CHECK(fixture.Physics->TakeContactEvents().size() == 2);
	CHECK(CountLogs(log, "contact events are waiting") == 1);
}

// Only where Jolt's own asserts are compiled out, which Jolt does exactly when NDEBUG is defined (Release and Dist; CI
// runs both): with them, Jolt asserts that an update reported no error, and its default assert handler breaks into
// the debugger, so in Debug this condition stops the process before the engine can warn.
#if defined(NDEBUG)
TEST_CASE("PhysicsSystem: running out of contact capacity is reported once per run")
{
	PhysicsFixture fixture;
	// Spheres of radius 0.5 on a 0.1 grid inside a 0.5 cube: every pair overlaps (the cube's diagonal is 0.87), so
	// 200 of them make 19900 contacts, more than Jolt's contact constraint cache holds.
	std::vector<Entity> spheres;
	for (int i = 0; i < 200; i++)
		spheres.push_back(fixture.Body("Sphere" + std::to_string(i), glm::vec3(0.0f), ColliderShape::Sphere));
	auto pile = [&] {
		for (size_t i = 0; i < spheres.size(); i++)
			spheres[i].GetComponent<TransformComponent>().Translation = 0.1f * glm::vec3(static_cast<float>(i % 6), static_cast<float>((i / 6) % 6), static_cast<float>(i / 36));
	};
	Paralax::Testing::ScopedLogCapture log;

	pile();
	fixture.World.OnRuntimeStart();
	fixture.Run(5 * Step);
	CHECK(CountLogs(log, "ran out of capacity") == 1);
	for (Entity sphere : spheres)
		CHECK(IsFinite(PositionOf(sphere)));

	// A new run reports it again.
	fixture.World.OnRuntimeStop();
	pile();
	fixture.World.OnRuntimeStart();
	fixture.Run(5 * Step);
	CHECK(CountLogs(log, "ran out of capacity") == 2);
}
#endif

TEST_CASE("PhysicsSystem: colliders on children without a Rigidbody become parts of the body above them")
{
	PhysicsFixture fixture;
	fixture.Ground();
	// A dynamic body with a box of its own and a sphere 1 m below it on a child: it rests on the sphere.
	Entity cart = fixture.Body("Cart", glm::vec3(0.0f, 3.0f, 0.0f));
	Entity wheel = fixture.World.CreateEntity("Wheel");
	wheel.AddComponent<ColliderComponent>().Shape = ColliderShape::Sphere;
	wheel.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -1.0f, 0.0f);
	fixture.World.SetParent(wheel, cart, false);
	// A Rigidbody whose only colliders are on its children (two boxes side by side, scaled through the child).
	Entity frame = fixture.World.CreateEntity("Frame");
	frame.AddComponent<RigidbodyComponent>();
	frame.GetComponent<TransformComponent>().Translation = glm::vec3(6.0f, 3.0f, 0.0f);
	for (float x : {-1.0f, 1.0f})
	{
		Entity side = fixture.World.CreateEntity("Side");
		side.AddComponent<ColliderComponent>();
		side.GetComponent<TransformComponent>().Translation = glm::vec3(x, 0.0f, 0.0f);
		side.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f, 2.0f, 1.0f);
		fixture.World.SetParent(side, frame, false);
	}
	// A trigger child stays a separate (sensor) body.
	Entity sensor = fixture.World.CreateEntity("Sensor");
	sensor.AddComponent<ColliderComponent>().IsTrigger = true;
	fixture.World.SetParent(sensor, cart, false);
	fixture.World.OnRuntimeStart();
	CHECK(fixture.Physics->HasBody(cart.GetUUID()));
	CHECK_FALSE(fixture.Physics->HasBody(wheel.GetUUID()));
	CHECK(fixture.Physics->HasBody(frame.GetUUID()));
	CHECK(fixture.Physics->HasBody(sensor.GetUUID()));
	fixture.Run(3.0f);

	// The wheel (radius 0.5, 1 m below the cart's centre) holds the cart 1.5 m up; it did not collide with its own
	// body, so the cart stayed put sideways.
	CHECK(PositionOf(cart).y == doctest::Approx(1.5).epsilon(0.02));
	CHECK(std::abs(PositionOf(cart).x) < 0.01f);
	// The frame rests on its 2 m tall sides.
	CHECK(PositionOf(frame).y == doctest::Approx(1.0).epsilon(0.02));
	// Raycasts and contacts report the body's entity.
	std::optional<RaycastHit> hit = fixture.Physics->Raycast(glm::vec3(0.0f, 0.2f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f), 10.0f);
	REQUIRE(hit);
	CHECK(hit->Entity == cart.GetUUID());

	// Moving a part relative to its body reshapes the body: with the wheel 2 m down, the cart rests 2.5 m up. (Lifted
	// first: a part reshaped into the ground would be pushed out through it.)
	wheel.GetComponent<TransformComponent>().Translation.y = -2.0f;
	cart.GetComponent<TransformComponent>().Translation.y = 4.0f;
	fixture.Run(3.0f);
	CHECK(PositionOf(cart).y == doctest::Approx(2.5).epsilon(0.02));
	// Removing the wheel's collider leaves the cart on its own box.
	wheel.RemoveComponent<ColliderComponent>();
	fixture.Run(3.0f);
	CHECK(PositionOf(cart).y == doctest::Approx(0.5).epsilon(0.02));
	// A child given its own Rigidbody becomes a body of its own.
	Entity side = *fixture.World.FindEntityByName("Side");
	side.AddComponent<RigidbodyComponent>();
	fixture.Run(Step);
	CHECK(fixture.Physics->HasBody(side.GetUUID()));
}

TEST_CASE("PhysicsSystem: a part with an unusable transform is left out of its body")
{
	PhysicsFixture fixture;
	fixture.Ground();
	Entity cart = fixture.Body("Cart", glm::vec3(0.0f, 3.0f, 0.0f));
	Entity wheel = fixture.World.CreateEntity("Wheel");
	wheel.AddComponent<ColliderComponent>().Shape = ColliderShape::Sphere;
	wheel.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -1.0f, 0.0f);
	wheel.GetComponent<TransformComponent>().Scale = glm::vec3(0.0f);
	fixture.World.SetParent(wheel, cart, false);
	Paralax::Testing::ScopedLogCapture log;
	fixture.World.OnRuntimeStart();
	fixture.Run(3.0f);
	CHECK(PositionOf(cart).y == doctest::Approx(0.5).epsilon(0.02)); // On its own box only.
	CHECK(CountLogs(log, "zero-scale or non-finite") == 1);
	wheel.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f);
	cart.GetComponent<TransformComponent>().Translation.y = 3.0f;
	fixture.Run(3.0f);
	CHECK(PositionOf(cart).y == doctest::Approx(1.5).epsilon(0.02));
}

TEST_CASE("PhysicsSystem: rotated and offset parts are placed in the body's frame")
{
	PhysicsSystemSpecification specification;
	specification.Gravity = glm::vec3(0.0f);
	PhysicsFixture fixture(specification);
	// The body is turned a quarter about Y; its part sits 2 m along the body's local +X (world -Z after the turn), is
	// itself turned, and has a collider offset of 1 m along its own local +X.
	Entity body = fixture.Body("Body", glm::vec3(10.0f, 0.0f, 0.0f), ColliderShape::Sphere, RigidbodyType::Static);
	body.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	body.GetComponent<ColliderComponent>().Size = glm::vec3(0.2f);
	Entity part = fixture.World.CreateEntity("Part");
	auto& collider = part.AddComponent<ColliderComponent>();
	collider.Size = glm::vec3(0.5f);
	collider.Offset = glm::vec3(1.0f, 0.0f, 0.0f);
	part.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f, 0.0f, 0.0f);
	part.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	fixture.World.SetParent(part, body, false);
	fixture.World.OnRuntimeStart();

	// Where the part's box should be: the scene's own world transform of the collider's offset point.
	const glm::vec3 expected = glm::vec3(fixture.World.GetWorldTransform(part) * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	std::optional<RaycastHit> hit = fixture.Physics->Raycast(expected + glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
	REQUIRE(hit);
	CHECK(hit->Entity == body.GetUUID());
	CHECK(hit->Distance == doctest::Approx(4.75).epsilon(0.001));                                             // Top of the 0.5 m box.
	CHECK_FALSE(fixture.Physics->Raycast(glm::vec3(12.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f)); // Not unrotated.
}

TEST_CASE("PhysicsSystem: a compound body far from the origin keeps its shape and is not rebuilt")
{
	PhysicsSystemSpecification specification;
	specification.Gravity = glm::vec3(0.0f);
	PhysicsFixture fixture(specification);
	Entity body = fixture.Body("Far", glm::vec3(5000.0f, 300.0f, -2000.0f));
	body.GetComponent<RigidbodyComponent>().AngularDamping = 0.0f;
	body.GetComponent<RigidbodyComponent>().LinearDamping = 0.0f;
	for (float x : {-1.5f, 1.5f})
	{
		Entity arm = fixture.World.CreateEntity("Arm");
		arm.AddComponent<ColliderComponent>().Size = glm::vec3(1.0f, 0.2f, 0.2f);
		arm.GetComponent<TransformComponent>().Translation = glm::vec3(x, 0.0f, 0.0f);
		fixture.World.SetParent(arm, body, false);
	}
	fixture.World.OnRuntimeStart();
	REQUIRE(fixture.Physics->SetAngularVelocity(body.GetUUID(), glm::vec3(0.3f, 1.0f, 0.2f)));
	REQUIRE(fixture.Physics->SetLinearVelocity(body.GetUUID(), glm::vec3(3.0f, 0.0f, 0.0f)));
	const uint64_t created = fixture.Physics->GetBodiesCreated();
	fixture.Run(2.0f);
	CHECK(fixture.Physics->GetBodiesCreated() == created); // Spinning and moving, yet the same body throughout.
	CHECK(glm::length(*fixture.Physics->GetAngularVelocity(body.GetUUID()) - glm::vec3(0.3f, 1.0f, 0.2f)) < 0.2f);
}

TEST_CASE("PhysicsSystem: an off-centre compound writes back its entity's origin, and keeps its motion when reshaped")
{
	PhysicsSystemSpecification specification;
	specification.Gravity = glm::vec3(0.0f);
	PhysicsFixture fixture(specification);
	// The only part sits 2 m to the side, so the centre of mass is not at the entity's origin.
	Entity body = fixture.World.CreateEntity("Lopsided");
	body.AddComponent<RigidbodyComponent>().LinearDamping = 0.0f;
	body.GetComponent<RigidbodyComponent>().AngularDamping = 0.0f;
	Entity weight = fixture.World.CreateEntity("Weight");
	weight.AddComponent<ColliderComponent>();
	weight.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f, 0.0f, 0.0f);
	fixture.World.SetParent(weight, body, false);
	Entity balance = fixture.World.CreateEntity("Balance");
	balance.AddComponent<ColliderComponent>();
	balance.GetComponent<TransformComponent>().Translation = glm::vec3(-2.0f, 0.0f, 0.0f);
	fixture.World.SetParent(balance, body, false);
	fixture.World.OnRuntimeStart();

	// Moving without spin, the entity's origin travels with the body: write-back is the origin, not the centre of mass.
	REQUIRE(fixture.Physics->SetLinearVelocity(body.GetUUID(), glm::vec3(0.0f, 0.0f, 1.0f)));
	fixture.Run(1.0f);
	CHECK(glm::all(glm::epsilonEqual(PositionOf(body), glm::vec3(0.0f, 0.0f, 1.0f), 2e-3f)));

	// Spinning about its centre (the origin, while balanced), then losing one side: the centre of mass jumps to the
	// remaining part, but every point of the body keeps its velocity, so the entity's origin keeps circling it.
	REQUIRE(fixture.Physics->SetLinearVelocity(body.GetUUID(), glm::vec3(0.0f)));
	REQUIRE(fixture.Physics->SetAngularVelocity(body.GetUUID(), glm::vec3(0.0f, 1.0f, 0.0f)));
	fixture.Run(Step);
	balance.RemoveComponent<ColliderComponent>();
	fixture.Run(Step);
	const glm::vec3 before = PositionOf(body);
	fixture.Run(Step);
	const glm::vec3 after = PositionOf(body);
	// Without the correction the origin would freeze (the new centre of mass at rest, the origin on its rim moving
	// at |w| * 2 m = 2 m/s). With it the origin stays where it was, since it was the old centre and at rest.
	CHECK(glm::length(after - before) < 1e-3f);
}

TEST_CASE("PhysicsSystem: kinematic compounds carry bodies, and trigger children follow moving bodies")
{
	PhysicsFixture fixture;
	// A kinematic platform of two boards (no collider of its own), moved by its transform.
	Entity platform = fixture.World.CreateEntity("Platform");
	platform.AddComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
	platform.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
	for (float x : {-1.0f, 1.0f})
	{
		Entity board = fixture.World.CreateEntity("Board");
		auto& collider = board.AddComponent<ColliderComponent>();
		collider.Size = glm::vec3(2.0f, 0.2f, 4.0f);
		collider.Friction = 5.0f;
		board.GetComponent<TransformComponent>().Translation = glm::vec3(x, 0.0f, 0.0f);
		fixture.World.SetParent(board, platform, false);
	}
	// A trigger child on the platform: a sensor that follows it.
	Entity sensor = fixture.World.CreateEntity("Sensor");
	sensor.AddComponent<ColliderComponent>().IsTrigger = true;
	sensor.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -1.0f, 0.0f);
	fixture.World.SetParent(sensor, platform, false);
	Entity post = fixture.World.CreateEntity("Post"); // Static, in the sensor's path.
	post.AddComponent<ColliderComponent>().Size = glm::vec3(0.5f);
	post.GetComponent<TransformComponent>().Translation = glm::vec3(3.0f, 0.0f, 0.0f);
	Entity rider = fixture.Body("Rider", glm::vec3(0.0f, 1.6f, 0.0f));
	rider.GetComponent<ColliderComponent>().Friction = 5.0f;
	fixture.World.OnRuntimeStart();
	fixture.Run(1.0f);
	fixture.Physics->TakeContactEvents();

	for (int i = 0; i < 60; i++)
	{
		platform.GetComponent<TransformComponent>().Translation.x += 1.0f / 20.0f; // 3 m in a second.
		fixture.World.OnUpdateRuntime(Timestep(Step));
	}
	CHECK(PositionOf(rider).x == doctest::Approx(3.0).epsilon(0.05));
	CHECK(PositionOf(rider).y == doctest::Approx(1.6).epsilon(0.02));
	CHECK(HasEvent(fixture.Physics->TakeContactEvents(), ContactEventType::TriggerEnter, sensor.GetUUID(), post.GetUUID()));
}

TEST_CASE("PhysicsSystem: colliders per body are capped, and colliders that cannot join a body say why")
{
	PhysicsSystemSpecification specification;
	specification.MaxCollidersPerBody = 2;
	PhysicsFixture fixture(specification);
	Paralax::Testing::ScopedLogCapture log;
	Entity body = fixture.Body("Many", glm::vec3(0.0f, 5.0f, 0.0f));
	for (int i = 0; i < 3; i++)
	{
		Entity child = fixture.World.CreateEntity("Child");
		child.AddComponent<ColliderComponent>();
		child.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(i + 1), 0.0f, 0.0f);
		fixture.World.SetParent(child, body, false);
	}
	// A body whose own collider is a trigger cannot take solid parts.
	Entity zone = fixture.Body("Zone", glm::vec3(10.0f, 5.0f, 0.0f));
	zone.GetComponent<ColliderComponent>().IsTrigger = true;
	Entity solid = fixture.World.CreateEntity("Solid");
	solid.AddComponent<ColliderComponent>();
	fixture.World.SetParent(solid, zone, false);
	fixture.World.OnRuntimeStart();
	fixture.Run(0.5f);
	CHECK(fixture.Physics->HasBody(body.GetUUID()));
	CHECK(CountLogs(log, "has more than 2 colliders") == 1);
	CHECK(fixture.Physics->HasBody(solid.GetUUID()));
	CHECK(CountLogs(log, "whose own collider is a trigger") == 1);

	Paralax::Testing::ScopedAssertionCapture capture;
	PhysicsSystemSpecification none;
	none.MaxCollidersPerBody = 0;
	CHECK_THROWS_AS(PhysicsSystem{none}, Paralax::Testing::AssertionFailedError);
}
