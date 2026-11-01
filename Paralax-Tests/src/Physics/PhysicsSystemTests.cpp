#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Physics/PhysicsSystem.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

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
	CHECK(CountLogs(log, "has a Rigidbody but no Collider") == 1);
	CHECK(CountLogs(log, "Colliders are not combined") == 1);
	CHECK(CountLogs(log, "body limit") == 4); // 20 bodies wanted, 16 allowed: one report each for the other 4.
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
