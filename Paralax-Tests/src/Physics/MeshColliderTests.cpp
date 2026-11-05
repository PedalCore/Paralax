#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Physics/PhysicsSystem.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <limits>
#include <tuple>

using namespace Paralax;

namespace {

	constexpr float Step = 1.0f / 60.0f;

	struct MeshFixture
	{
		explicit MeshFixture(std::filesystem::path directory = {})
			: Assets(std::move(directory))
		{
			PhysicsSystemSpecification specification;
			specification.WorkerThreads = 0;
			Physics = static_cast<PhysicsSystem*>(World.AddSystem(CreateScope<PhysicsSystem>(Assets, specification)));
		}
		~MeshFixture() { World.OnRuntimeStop(); }

		AssetHandle AddMesh(Ref<MeshAsset> mesh)
		{
			mesh->Handle = NextHandle++;
			Assets.AddMemoryAsset(mesh);
			return mesh->Handle;
		}

		Entity Surface(AssetHandle mesh)
		{
			Entity surface = World.CreateEntity("Surface");
			auto& collider = surface.AddComponent<ColliderComponent>();
			collider.Shape = ColliderShape::Mesh;
			collider.Mesh = mesh;
			return surface;
		}

		Entity Ball(glm::vec3 position)
		{
			Entity ball = World.CreateEntity("Ball");
			ball.GetComponent<TransformComponent>().Translation = position;
			ball.AddComponent<ColliderComponent>().Shape = ColliderShape::Sphere;
			ball.AddComponent<RigidbodyComponent>().ContinuousCollision = true;
			return ball;
		}

		void Run(int frames = 1)
		{
			for (int frame = 0; frame < frames; frame++)
				World.OnUpdateRuntime(Timestep(Step));
		}

		AssetManager Assets;
		Scene World;
		PhysicsSystem* Physics;
		uint64_t NextHandle = 10000;
	};

	bool ContactWith(const std::vector<ContactEvent>& events, UUID a, UUID b)
	{
		return std::any_of(events.begin(), events.end(), [&](const ContactEvent& event) {
			return event.Type == ContactEventType::CollisionEnter && ((event.A == a && event.B == b) || (event.A == b && event.B == a));
		});
	}

	// Two disconnected ledges leave a real gap that a convex hull or bounding box would close.
	Ref<MeshAsset> Ledges()
	{
		Ref<MeshAsset> mesh = MeshPrimitives::CreatePlane(2.0f);
		mesh->Submeshes[0].Transform = glm::translate(glm::mat4(1.0f), glm::vec3(-2.0f, 0.0f, 0.0f));
		Submesh right = mesh->Submeshes[0];
		right.Transform = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));
		mesh->Submeshes.push_back(right);
		return mesh;
	}
}

TEST_CASE("MeshCollider: authored concavity supports a ball while a gap remains empty")
{
	MeshFixture fixture;
	Entity surface = fixture.Surface(fixture.AddMesh(Ledges()));
	Entity supported = fixture.Ball({-2.0f, 2.0f, 0.0f});
	Entity falling = fixture.Ball({0.0f, 2.0f, 0.0f});
	fixture.World.OnRuntimeStart();
	CHECK_FALSE(fixture.Physics->Raycast({0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 2.0f));
	auto hit = fixture.Physics->Raycast({2.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 2.0f);
	REQUIRE(hit);
	CHECK(hit->Entity == surface.GetUUID());
	CHECK(hit->Distance == doctest::Approx(1.0));
	CHECK(hit->Normal.y > 0.99f);
	fixture.Run(120);
	CHECK(supported.GetComponent<TransformComponent>().Translation.y == doctest::Approx(0.5).epsilon(0.02));
	CHECK(falling.GetComponent<TransformComponent>().Translation.y < -10.0f);
	CHECK(ContactWith(fixture.Physics->TakeContactEvents(), surface.GetUUID(), supported.GetUUID()));
}

TEST_CASE("MeshCollider: reflected fields survive scene round trips and reject invalid input")
{
	Scene scene;
	Entity surface = scene.CreateEntity("Surface");
	auto& collider = surface.AddComponent<ColliderComponent>();
	const auto& info = *ComponentRegistry::Find("Collider");
	std::string error;
	REQUIRE(ComponentRegistry::SetField(*info.FindField("Shape"), &collider, "Mesh", error));
	REQUIRE(ComponentRegistry::SetField(*info.FindField("Mesh"), &collider, "18446744073709551615", error));
	REQUIRE(ComponentRegistry::SetField(*info.FindField("BackfaceCollision"), &collider, true, error));
	CHECK_FALSE(ComponentRegistry::SetField(*info.FindField("Mesh"), &collider, -1, error));
	CHECK_FALSE(ComponentRegistry::SetField(*info.FindField("BackfaceCollision"), &collider, "yes", error));
	Scene loaded;
	REQUIRE(SceneSerializer::Deserialize(loaded, SceneSerializer::Serialize(scene), error));
	const auto& restored = loaded.FindEntityByName("Surface")->GetComponent<ColliderComponent>();
	CHECK(restored.Shape == ColliderShape::Mesh);
	CHECK(static_cast<uint64_t>(restored.Mesh) == UINT64_MAX);
	CHECK(restored.BackfaceCollision);
}

TEST_CASE("MeshCollider: mirrored submesh and entity transforms preserve front faces and local offsets")
{
	MeshFixture fixture;
	Ref<MeshAsset> mesh = MeshPrimitives::CreatePlane(2.0f);
	mesh->Submeshes[0].Transform = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.0f));
	SUBCASE("ordinary submesh") {}
	SUBCASE("mirrored submesh")
	{
		mesh->Submeshes[0].Transform = glm::scale(mesh->Submeshes[0].Transform, glm::vec3(-1.0f, 1.0f, 1.0f));
	}
	Entity surface = fixture.Surface(fixture.AddMesh(mesh));
	surface.GetComponent<TransformComponent>().Scale = {-2.0f, 3.0f, 1.0f};
	surface.GetComponent<ColliderComponent>().Offset = {0.0f, 1.0f, 0.0f};
	surface.GetComponent<ColliderComponent>().Size = glm::vec3(100.0f); // Mesh uses authored dimensions.
	fixture.World.OnRuntimeStart();
	auto hit = fixture.Physics->Raycast({-4.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
	REQUIRE(hit);
	CHECK(hit->Point.y == doctest::Approx(6.0));
	CHECK(hit->Normal.y > 0.99f);
	CHECK_FALSE(fixture.Physics->Raycast({4.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f));
}

TEST_CASE("MeshCollider: kinematic compound rotation preserves geometry and reports the owning body")
{
	MeshFixture fixture;
	Entity parent = fixture.World.CreateEntity("Kinematic");
	parent.AddComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
	parent.GetComponent<TransformComponent>().Scale = {2.0f, 1.0f, 0.5f};
	Entity surface = fixture.Surface(fixture.AddMesh(MeshPrimitives::CreatePlane(4.0f)));
	REQUIRE(fixture.World.SetParent(surface, parent, false));
	surface.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.35f, glm::vec3(0, 0, 1));
	Entity extra = fixture.World.CreateEntity("Primitive child");
	extra.AddComponent<ColliderComponent>();
	REQUIRE(fixture.World.SetParent(extra, parent, false));
	extra.GetComponent<TransformComponent>().Translation = {20.0f, 0.0f, 0.0f};
	Entity ball = fixture.Ball({0.0f, 2.0f, 0.0f});
	fixture.World.OnRuntimeStart();
	CHECK(fixture.Physics->HasBody(parent.GetUUID()));
	CHECK_FALSE(fixture.Physics->HasBody(surface.GetUUID()));
	fixture.Run(45);
	CHECK(ContactWith(fixture.Physics->TakeContactEvents(), parent.GetUUID(), ball.GetUUID()));
	const uint64_t created = fixture.Physics->GetBodiesCreated();
	for (int frame = 0; frame < 30; frame++)
	{
		parent.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.005f * static_cast<float>(frame + 1), glm::vec3(0, 0, 1));
		fixture.Run();
	}
	CHECK(fixture.Physics->GetBodiesCreated() == created);
	const glm::mat4 world = fixture.World.GetWorldTransform(surface);
	const glm::vec3 point = glm::vec3(world * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	const glm::vec3 normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(world))) * glm::vec3(0, 1, 0));
	auto hit = fixture.Physics->Raycast(point + normal, -normal, 2.0f);
	REQUIRE(hit);
	CHECK(hit->Entity == parent.GetUUID());
	CHECK(glm::length(hit->Point - point) < 1e-4f);
	CHECK(glm::dot(hit->Normal, normal) > 0.999f);
}

TEST_CASE("MeshCollider: two sided faces catch a fast ball approaching from below")
{
	for (bool backfaces : {false, true})
	{
		CAPTURE(backfaces);
		MeshFixture fixture;
		fixture.Physics->SetGravity({0.0f, 0.0f, 0.0f});
		Entity surface = fixture.Surface(fixture.AddMesh(MeshPrimitives::CreatePlane(10.0f)));
		surface.GetComponent<ColliderComponent>().BackfaceCollision = backfaces;
		Entity ball = fixture.Ball({0.0f, -2.0f, 0.0f});
		fixture.World.OnRuntimeStart();
		fixture.Physics->SetGravity({0.0f, 0.0f, 0.0f});
		REQUIRE(fixture.Physics->SetLinearVelocity(ball.GetUUID(), {0.0f, 100.0f, 0.0f}));
		fixture.Run(10);
		const float y = ball.GetComponent<TransformComponent>().Translation.y;
		CHECK((backfaces ? y < -0.45f : y > 5.0f));
		CHECK(ContactWith(fixture.Physics->TakeContactEvents(), surface.GetUUID(), ball.GetUUID()) == backfaces);
	}
}

TEST_CASE("MeshCollider: dynamic bodies with mesh children are rejected and recover when made kinematic")
{
	MeshFixture fixture;
	Entity parent = fixture.World.CreateEntity("Parent");
	parent.AddComponent<RigidbodyComponent>();
	parent.AddComponent<ColliderComponent>();
	Entity surface = fixture.Surface(fixture.AddMesh(MeshPrimitives::CreatePlane()));
	SUBCASE("own mesh")
	{
		parent.GetComponent<ColliderComponent>() = surface.GetComponent<ColliderComponent>();
		fixture.World.DestroyEntity(surface);
	}
	SUBCASE("compound child")
	{
		REQUIRE(fixture.World.SetParent(surface, parent, false));
	}
	Paralax::Testing::ScopedLogCapture logs;
	fixture.World.OnRuntimeStart();
	fixture.Run(5);
	CHECK_FALSE(fixture.Physics->HasBody(parent.GetUUID()));
	size_t errors = 0;
	for (const auto& log : logs.GetEntries())
		errors += log.Message.find("Dynamic bodies cannot contain Mesh") != std::string::npos;
	CHECK(errors == 1);
	parent.GetComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
	fixture.Run();
	CHECK(fixture.Physics->HasBody(parent.GetUUID()));
	parent.GetComponent<RigidbodyComponent>().Type = RigidbodyType::Dynamic;
	fixture.Run();
	CHECK_FALSE(fixture.Physics->HasBody(parent.GetUUID()));
}

TEST_CASE("MeshCollider: hostile geometry fails safely and corrected versions recover")
{
	MeshFixture fixture;
	Ref<MeshAsset> mesh = MeshPrimitives::CreatePlane();
	const Ref<MeshAsset> original = CreateRef<MeshAsset>(*mesh);
	SUBCASE("out of range index")
	{
		mesh->Indices[0] = UINT32_MAX;
	}
	SUBCASE("overflowing index range")
	{
		mesh->Submeshes[0].IndexOffset = UINT32_MAX;
	}
	SUBCASE("overflowing vertex range")
	{
		mesh->Submeshes[0].VertexOffset = UINT32_MAX;
	}
	SUBCASE("partial triangle")
	{
		mesh->Submeshes[0].IndexCount = 1;
	}
	SUBCASE("nan position")
	{
		mesh->Vertices[0].Position.x = std::numeric_limits<float>::quiet_NaN();
	}
	SUBCASE("infinite transform")
	{
		mesh->Submeshes[0].Transform[0][0] = std::numeric_limits<float>::infinity();
	}
	SUBCASE("projective transform")
	{
		mesh->Submeshes[0].Transform[0][3] = 1.0f;
	}
	SUBCASE("singular transform")
	{
		mesh->Submeshes[0].Transform[0][0] = 0.0f;
	}
	SUBCASE("unbounded position")
	{
		mesh->Vertices[0].Position.x = 1e30f;
	}
	SUBCASE("all degenerate")
	{
		for (auto& vertex : mesh->Vertices)
			vertex.Position = glm::vec3(0.0f);
	}
	SUBCASE("empty mesh")
	{
		mesh->Submeshes.clear();
	}
	SUBCASE("submesh limit")
	{
		mesh->Submeshes.resize(65537, mesh->Submeshes[0]);
	}
	SUBCASE("aggregate triangle limit")
	{
		mesh = MeshPrimitives::CreatePlane(1.0f, 4);
		mesh->Submeshes.resize(31251, mesh->Submeshes[0]);
	}
	SUBCASE("index buffer limit")
	{
		mesh->Indices.resize(3000003);
	}
	Entity surface = fixture.Surface(fixture.AddMesh(mesh));
	Paralax::Testing::ScopedLogCapture logs;
	fixture.World.OnRuntimeStart();
	fixture.Run(5);
	CHECK_FALSE(fixture.Physics->HasBody(surface.GetUUID()));
	size_t errors = 0;
	for (const auto& log : logs.GetEntries())
		errors += log.Message.find("cannot create the collider") != std::string::npos;
	CHECK(errors == 1);
	const AssetHandle handle = mesh->Handle;
	const uint32_t version = mesh->Version;
	*mesh = *original;
	mesh->Handle = handle;
	mesh->Version = version + 1;
	fixture.Run();
	CHECK(fixture.Physics->HasBody(surface.GetUUID()));
}

TEST_CASE("MeshCollider: missing assets and changed handles never leave stale collision geometry")
{
	MeshFixture fixture;
	Entity surface = fixture.Surface(20000);
	fixture.World.OnRuntimeStart();
	CHECK_FALSE(fixture.Physics->HasBody(surface.GetUUID()));
	Ref<MeshAsset> mesh = MeshPrimitives::CreatePlane(4.0f);
	mesh->Handle = 20000;
	fixture.Assets.AddMemoryAsset(mesh);
	fixture.Run();
	CHECK(fixture.Physics->HasBody(surface.GetUUID()));
	const uint64_t created = fixture.Physics->GetBodiesCreated();
	fixture.Run(5);
	CHECK(fixture.Physics->GetBodiesCreated() == created);
	mesh->Submeshes[0].Transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f));
	mesh->Version++;
	fixture.Run();
	auto hit = fixture.Physics->Raycast({0, 5, 0}, {0, -1, 0}, 10);
	REQUIRE(hit);
	CHECK(hit->Point.y == doctest::Approx(2.0));
	CHECK(fixture.Physics->GetBodiesCreated() == created + 1);
	surface.GetComponent<ColliderComponent>().Mesh = 20001;
	fixture.Run();
	CHECK_FALSE(fixture.Physics->HasBody(surface.GetUUID()));
	CHECK_FALSE(fixture.Physics->Raycast({0, 5, 0}, {0, -1, 0}, 10));
}

TEST_CASE("MeshCollider: file reload and removal follow the asset manager lifetime")
{
	const auto directory = Paralax::Testing::CreateTestDirectory("MeshColliderReload");
	auto document = Paralax::Testing::MakeTriangleDocument(directory);
	document["nodes"] = {{{"mesh", 0}}};
	document["scenes"][0]["nodes"] = {0};
	Paralax::Testing::WriteGltf(directory, document);
	MeshFixture fixture(directory);
	const AssetHandle handle = fixture.Assets.ImportAsset("triangle.gltf");
	REQUIRE(handle.IsValid());
	Entity surface = fixture.Surface(handle);
	fixture.World.OnRuntimeStart();
	REQUIRE(fixture.Physics->HasBody(surface.GetUUID()));
	const uint64_t created = fixture.Physics->GetBodiesCreated();
	document["nodes"][0]["translation"] = {0, 0, 3};
	Paralax::Testing::WriteGltf(directory, document);
	REQUIRE(fixture.Assets.ReloadAsset(handle));
	fixture.Run();
	CHECK(fixture.Physics->GetBodiesCreated() == created + 1);
	auto hit = fixture.Physics->Raycast({0.2f, 0.2f, 5.0f}, {0, 0, -1}, 10);
	REQUIRE(hit);
	CHECK(hit->Point.z == doctest::Approx(3.0));
	REQUIRE(WriteFileAtomically(directory / "triangle.gltf", "invalid"));
	CHECK_FALSE(fixture.Assets.ReloadAsset(handle));
	fixture.Run();
	CHECK(fixture.Physics->HasBody(surface.GetUUID())); // A failed reload preserves the asset, just as rendering does.
	Paralax::Testing::WriteGltf(directory, document);
	REQUIRE(fixture.Assets.ReloadAsset(handle));
	fixture.Run();
	CHECK(fixture.Physics->HasBody(surface.GetUUID()));
	REQUIRE(fixture.Assets.RemoveAsset(handle));
	fixture.Run();
	CHECK_FALSE(fixture.Physics->HasBody(surface.GetUUID()));
}

TEST_CASE("MeshCollider: a system without assets refuses mesh geometry without assertions")
{
	Scene scene;
	Entity surface = scene.CreateEntity("Surface");
	surface.AddComponent<ColliderComponent>().Shape = ColliderShape::Mesh;
	auto* physics = static_cast<PhysicsSystem*>(scene.AddSystem(CreateScope<PhysicsSystem>()));
	scene.OnRuntimeStart();
	CHECK_FALSE(physics->HasBody(surface.GetUUID()));
	scene.OnRuntimeStop();
}

TEST_CASE("MeshCollider: moving concave contacts repeat deterministically")
{
	struct Trace
	{
		std::vector<glm::vec3> Positions;
		std::vector<std::tuple<int, ContactEventType, uint64_t, uint64_t>> Contacts;
	};
	auto run = [] {
		MeshFixture fixture;
		fixture.World.SetEntityIdSequence(UUIDSequence(123));
		Entity surface = fixture.Surface(fixture.AddMesh(Ledges()));
		surface.AddComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
		Entity ball = fixture.Ball({-2.0f, 1.0f, 0.0f});
		fixture.World.OnRuntimeStart();
		Trace trace;
		for (int frame = 0; frame < 180; frame++)
		{
			surface.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.05f * std::sin(static_cast<float>(frame) * Step), glm::vec3(0, 0, 1));
			fixture.Run();
			trace.Positions.push_back(ball.GetComponent<TransformComponent>().Translation);
			for (const ContactEvent& event : fixture.Physics->TakeContactEvents())
				trace.Contacts.emplace_back(frame, event.Type, static_cast<uint64_t>(event.A), static_cast<uint64_t>(event.B));
		}
		return trace;
	};
	const Trace first = run();
	const Trace second = run();
	CHECK_FALSE(first.Contacts.empty());
	CHECK(first.Contacts == second.Contacts);
	CHECK(first.Positions == second.Positions);
}

TEST_CASE("MeshCollider: raycasts skip backfaces and return the nearest front face with its normal")
{
	for (bool backfaces : {false, true})
	{
		CAPTURE(backfaces);
		MeshFixture fixture;
		const AssetHandle mesh = fixture.AddMesh(MeshPrimitives::CreatePlane(10.0f));
		Entity nearSurface = fixture.Surface(mesh);
		nearSurface.GetComponent<ColliderComponent>().BackfaceCollision = backfaces;
		Entity farSurface = fixture.Surface(mesh);
		farSurface.GetComponent<TransformComponent>().Translation.y = 2.0f;
		farSurface.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(1, 0, 0));
		fixture.World.OnRuntimeStart();
		auto nearHit = fixture.Physics->Raycast({0, -1, 0}, {0, 1, 0}, 2);
		CHECK(nearHit.has_value() == backfaces);
		auto hit = fixture.Physics->Raycast({0, -1, 0}, {0, 1, 0}, 5);
		REQUIRE(hit);
		CHECK(hit->Entity == (backfaces ? nearSurface : farSurface).GetUUID());
		CHECK(hit->Distance == doctest::Approx(backfaces ? 1.0 : 3.0));
		CHECK(hit->Normal.y < -0.99f);
	}
}

TEST_CASE("MeshCollider: large rotating meshes keep their cooked shape")
{
	MeshFixture fixture;
	Entity surface = fixture.Surface(fixture.AddMesh(MeshPrimitives::CreatePlane()));
	surface.AddComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
	surface.GetComponent<TransformComponent>().Scale = glm::vec3(1000.0f);
	SUBCASE("root mesh") {}
	SUBCASE("translated compound child")
	{
		Entity child = fixture.Surface(surface.GetComponent<ColliderComponent>().Mesh);
		surface.RemoveComponent<ColliderComponent>();
		REQUIRE(fixture.World.SetParent(child, surface, false));
		child.GetComponent<TransformComponent>().Translation.x = 50.0f;
	}
	fixture.World.OnRuntimeStart();
	for (int frame = 0; frame < 90; frame++)
	{
		surface.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.01f * static_cast<float>(frame), glm::normalize(glm::vec3(1, 2, 3)));
		fixture.Run();
	}
	CHECK(fixture.Physics->GetBodiesCreated() == 1);
}

TEST_CASE("MeshCollider: a kinematic triangle platform carries a resting ball upward")
{
	MeshFixture fixture;
	Entity surface = fixture.Surface(fixture.AddMesh(MeshPrimitives::CreatePlane(10.0f)));
	surface.AddComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
	Entity ball = fixture.Ball({0, 1, 0});
	fixture.World.OnRuntimeStart();
	fixture.Run(120);
	fixture.Physics->TakeContactEvents();
	for (int frame = 1; frame <= 60; frame++)
	{
		surface.GetComponent<TransformComponent>().Translation.y = static_cast<float>(frame) * Step;
		fixture.Run();
	}
	CHECK(ball.GetComponent<TransformComponent>().Translation.y == doctest::Approx(1.5).epsilon(0.02));
	CHECK(fixture.Physics->GetBodiesCreated() == 2);
	CHECK(fixture.Physics->TakeContactEvents().empty()); // Continuous contact, rather than destroy/recreate each frame.
}

TEST_CASE("MeshCollider: a missing compound child rejects its whole body and repairs cleanly")
{
	MeshFixture fixture;
	Entity owner = fixture.World.CreateEntity("Owner");
	owner.AddComponent<RigidbodyComponent>().Type = RigidbodyType::Kinematic;
	owner.AddComponent<ColliderComponent>();
	Entity child = fixture.Surface(AssetManager::BuiltinDefaultMaterial); // Registered, but not a mesh.
	REQUIRE(fixture.World.SetParent(child, owner, false));
	fixture.World.OnRuntimeStart();
	CHECK_FALSE(fixture.Physics->HasBody(owner.GetUUID()));
	CHECK_FALSE(fixture.Physics->HasBody(child.GetUUID()));
	child.GetComponent<ColliderComponent>().Mesh = AssetManager::BuiltinPlane;
	fixture.Run();
	CHECK(fixture.Physics->HasBody(owner.GetUUID()));
	CHECK_FALSE(fixture.Physics->HasBody(child.GetUUID()));
}

TEST_CASE("MeshCollider: collapsed triangles are omitted when usable triangles remain")
{
	MeshFixture fixture;
	Ref<MeshAsset> mesh = MeshPrimitives::CreatePlane();
	mesh->Indices.insert(mesh->Indices.end(), {0, 0, 0});
	mesh->Submeshes[0].IndexCount += 3;
	Entity surface = fixture.Surface(fixture.AddMesh(mesh));
	fixture.World.OnRuntimeStart();
	CHECK(fixture.Physics->HasBody(surface.GetUUID()));
	CHECK(fixture.Physics->Raycast({0, 1, 0}, {0, -1, 0}, 2).has_value());
}
