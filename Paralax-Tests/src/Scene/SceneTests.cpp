#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <glm/gtc/epsilon.hpp>

using namespace Paralax;
using Paralax::Testing::AssertionFailedError;
using Paralax::Testing::ScopedAssertionCapture;

namespace {

	std::vector<std::string> NamesInOrder(Scene& scene)
	{
		std::vector<std::string> names;
		scene.ForEachEntity([&](Entity entity) { names.push_back(entity.GetName()); });
		return names;
	}

	// The UUIDs a view over these components visits, in its order.
	template<typename... Components>
	std::vector<UUID> ViewOrder(Scene& scene)
	{
		std::vector<UUID> order;
		for (entt::entity handle : scene.GetAllEntitiesWith<Components...>())
			order.push_back(scene.GetRegistry().get<IDComponent>(handle).ID);
		return order;
	}

	// Every view the engine uses (and those over the core components) visits both scenes' entities in one order.
	void CheckSameViewOrders(Scene& copy, Scene& source)
	{
		CHECK(ViewOrder<IDComponent>(copy) == ViewOrder<IDComponent>(source));
		CHECK(ViewOrder<TagComponent>(copy) == ViewOrder<TagComponent>(source));
		CHECK(ViewOrder<TransformComponent>(copy) == ViewOrder<TransformComponent>(source));
		CHECK(ViewOrder<RelationshipComponent>(copy) == ViewOrder<RelationshipComponent>(source));
		CHECK(ViewOrder<DirectionalLightComponent>(copy) == ViewOrder<DirectionalLightComponent>(source));
		CHECK(ViewOrder<SkyLightComponent>(copy) == ViewOrder<SkyLightComponent>(source));
		CHECK(ViewOrder<PointLightComponent>(copy) == ViewOrder<PointLightComponent>(source));
		CHECK(ViewOrder<TransformComponent, PointLightComponent>(copy) == ViewOrder<TransformComponent, PointLightComponent>(source));
		CHECK(ViewOrder<DirectionalLightComponent, SkyLightComponent>(copy) == ViewOrder<DirectionalLightComponent, SkyLightComponent>(source));
	}

	bool NearMatrix(const glm::mat4& a, const glm::mat4& b)
	{
		for (int i = 0; i < 4; i++)
		{
			if (!glm::all(glm::epsilonEqual(a[i], b[i], 1e-4f)))
				return false;
		}
		return true;
	}

}

TEST_CASE("Scene: new entities have core components, unique IDs and are roots")
{
	Scene scene("Test");
	Entity a = scene.CreateEntity("A");
	Entity b = scene.CreateEntity();

	CHECK(a.HasComponent<IDComponent, TagComponent, TransformComponent, RelationshipComponent>());
	CHECK(a.GetUUID() != b.GetUUID());
	CHECK(a.GetName() == "A");
	CHECK(b.GetName() == "Entity");
	CHECK(scene.GetEntityCount() == 2);
	CHECK(scene.GetRootEntities() == std::vector<UUID>{a.GetUUID(), b.GetUUID()});
	CHECK(scene.GetEntityByUUID(a.GetUUID()) == a);
	CHECK_FALSE(scene.TryGetEntityByUUID(UUID(12345)).has_value());
}

TEST_CASE("Scene: invalid creation and stale handles are verified")
{
	Scene scene;
	Entity a = scene.CreateEntityWithUUID(UUID(42), "A");
	ScopedAssertionCapture capture;
	CHECK_THROWS_AS(scene.CreateEntityWithUUID(UUID(42)), AssertionFailedError);
	CHECK_THROWS_AS(scene.CreateEntityWithUUID(UUID(0)), AssertionFailedError);
	CHECK_THROWS_AS(scene.GetEntityByUUID(UUID(7)), AssertionFailedError);

	scene.DestroyEntity(a);
	CHECK_FALSE(scene.IsValid(a));
	CHECK_THROWS_AS(a.GetName(), AssertionFailedError);
	CHECK_THROWS_AS(Entity().GetName(), AssertionFailedError);
	CHECK_THROWS_AS(scene.CreateEntity().AddComponent<TagComponent>(), AssertionFailedError);

	// Every hierarchy operation verifies its entity rather than accepting a destroyed one.
	CHECK_THROWS_AS(scene.DuplicateEntity(a), AssertionFailedError);
	CHECK_THROWS_AS(scene.SetParent(a, std::nullopt), AssertionFailedError);
	CHECK_THROWS_AS(scene.SetSiblingIndex(a, 0), AssertionFailedError);
	Scene other;
	Entity otherRoot = other.CreateEntity("OtherRoot");
	CHECK_THROWS_AS(other.CopySubtreeFrom(scene, a), AssertionFailedError);
	CHECK_THROWS_AS(other.CopySubtreeFrom(scene, a, otherRoot), AssertionFailedError);
	CHECK(other.GetEntityCount() == 1);
	CHECK(scene.GetEntityCount() == 1);

	// Component access verifies presence; TryGetComponent is the query.
	Entity live = scene.CreateEntity("Live");
	CHECK_THROWS_AS(live.GetComponent<PointLightComponent>(), AssertionFailedError);
	CHECK_THROWS_AS(live.RemoveComponent<PointLightComponent>(), AssertionFailedError);
	CHECK(live.TryGetComponent<PointLightComponent>() == nullptr);
}

TEST_CASE("Scene: components can be added, queried and removed")
{
	Scene scene;
	Entity entity = scene.CreateEntity();
	CHECK_FALSE(entity.HasComponent<PointLightComponent>());
	entity.AddComponent<PointLightComponent>().Intensity = 5.0f;
	CHECK(entity.GetComponent<PointLightComponent>().Intensity == 5.0f);
	CHECK(entity.TryGetComponent<SpotLightComponent>() == nullptr);
	entity.AddOrReplaceComponent<PointLightComponent>().Intensity = 7.0f;
	CHECK(entity.GetComponent<PointLightComponent>().Intensity == 7.0f);
	entity.RemoveComponent<PointLightComponent>();
	CHECK_FALSE(entity.HasComponent<PointLightComponent>());
}

TEST_CASE("Scene: hierarchy order, reparenting and cycle rejection")
{
	Paralax::Testing::ScopedLogCapture logs;
	Scene scene;
	Entity root = scene.CreateEntity("Root");
	Entity child1 = scene.CreateEntity("Child1");
	Entity child2 = scene.CreateEntity("Child2");
	Entity grandchild = scene.CreateEntity("Grandchild");
	Entity other = scene.CreateEntity("Other");

	REQUIRE(scene.SetParent(child1, root));
	REQUIRE(scene.SetParent(child2, root));
	REQUIRE(scene.SetParent(grandchild, child1));

	CHECK(NamesInOrder(scene) == std::vector<std::string>{"Root", "Child1", "Grandchild", "Child2", "Other"});
	CHECK(scene.GetParent(grandchild) == child1);
	CHECK(scene.IsAncestorOf(root, grandchild));
	CHECK_FALSE(scene.IsAncestorOf(grandchild, root));

	CHECK_FALSE(scene.SetParent(root, grandchild));
	CHECK_FALSE(scene.SetParent(root, root));
	CHECK(logs.Contains(spdlog::level::err, "descendants"));

	scene.SetSiblingIndex(child2, 0);
	scene.SetSiblingIndex(other, 0);
	CHECK(NamesInOrder(scene) == std::vector<std::string>{"Other", "Root", "Child2", "Child1", "Grandchild"});

	REQUIRE(scene.SetParent(child1, std::nullopt));
	CHECK_FALSE(scene.GetParent(child1).has_value());
	CHECK(root.GetComponent<RelationshipComponent>().Children == std::vector<UUID>{child2.GetUUID()});
}

TEST_CASE("Scene: world transforms compose through the hierarchy and reparenting keeps world placement")
{
	Scene scene;
	Entity parent = scene.CreateEntity("Parent");
	Entity child = scene.CreateEntity("Child");
	parent.GetTransform().Translation = {10.0f, 0.0f, 0.0f};
	parent.GetTransform().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	parent.GetTransform().Scale = glm::vec3(2.0f);
	child.GetTransform().Translation = {1.0f, 2.0f, 3.0f};

	const glm::mat4 childWorldBefore = scene.GetWorldTransform(child);
	REQUIRE(scene.SetParent(child, parent, true));
	CHECK(NearMatrix(scene.GetWorldTransform(child), childWorldBefore));

	// Without keepWorldTransform, the local transform is kept and the world one changes.
	Entity other = scene.CreateEntity("Other");
	other.GetTransform().Translation = {1.0f, 0.0f, 0.0f};
	REQUIRE(scene.SetParent(other, parent, false));
	const glm::vec3 world = glm::vec3(scene.GetWorldTransform(other)[3]);
	CHECK(world.x == doctest::Approx(10.0));
	CHECK(world.z == doctest::Approx(-2.0));
}

TEST_CASE("Scene: destroying an entity destroys its descendants and detaches it from its parent")
{
	Scene scene;
	Entity root = scene.CreateEntity("Root");
	Entity child = scene.CreateEntity("Child");
	Entity grandchild = scene.CreateEntity("Grandchild");
	Entity sibling = scene.CreateEntity("Sibling");
	scene.SetParent(child, root);
	scene.SetParent(grandchild, child);
	scene.SetParent(sibling, root);

	scene.DestroyEntity(child);
	CHECK_FALSE(scene.IsValid(child));
	CHECK_FALSE(scene.IsValid(grandchild));
	CHECK(scene.IsValid(sibling));
	CHECK(scene.GetEntityCount() == 2);
	CHECK(root.GetComponent<RelationshipComponent>().Children == std::vector<UUID>{sibling.GetUUID()});

	scene.DestroyEntity(root);
	CHECK(scene.GetEntityCount() == 0);
	CHECK(scene.GetRootEntities().empty());

	// Destroying null or already-destroyed entities is a no-op.
	scene.DestroyEntity(Entity());
	scene.DestroyEntity(root);
}

TEST_CASE("Scene: duplicating copies components and the subtree with new IDs, right after the original")
{
	Scene scene;
	Entity first = scene.CreateEntity("First");
	Entity original = scene.CreateEntity("Original");
	Entity last = scene.CreateEntity("Last");
	Entity child = scene.CreateEntity("Child");
	scene.SetParent(child, original);
	original.AddComponent<PointLightComponent>().Intensity = 3.0f;
	original.GetTransform().Translation = {1.0f, 2.0f, 3.0f};
	original.GetComponent<TagComponent>().Tag = "Lamp";

	Entity copy = scene.DuplicateEntity(original);
	CHECK(copy.GetUUID() != original.GetUUID());
	CHECK(copy.GetName() == "Original");
	CHECK(copy.GetComponent<TagComponent>().Tag == "Lamp");
	CHECK(copy.GetComponent<PointLightComponent>().Intensity == 3.0f);
	CHECK(copy.GetTransform().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
	REQUIRE(copy.GetComponent<RelationshipComponent>().Children.size() == 1);
	CHECK(copy.GetComponent<RelationshipComponent>().Children[0] != child.GetUUID());

	CHECK(NamesInOrder(scene) == std::vector<std::string>{"First", "Original", "Child", "Original", "Child", "Last"});
	CHECK(scene.GetEntityCount() == 6);
	(void)first;
	(void)last;
}

TEST_CASE("Scene: Copy produces an independent scene with identical IDs, components and order")
{
	Scene source("Source");
	Entity parent = source.CreateEntity("Parent");
	Entity child = source.CreateEntity("Child");
	source.SetParent(child, parent);
	parent.AddComponent<CameraComponent>().Primary = true;
	child.AddComponent<MeshComponent>().Mesh = UUID(99);

	Ref<Scene> copy = Scene::Copy(source);
	CHECK(copy->GetName() == "Source");
	CHECK(copy->GetEntityCount() == 2);
	Entity copiedChild = copy->GetEntityByUUID(child.GetUUID());
	CHECK(copiedChild.GetComponent<MeshComponent>().Mesh == UUID(99));
	CHECK(copy->GetParent(copiedChild)->GetUUID() == parent.GetUUID());
	CHECK(NamesInOrder(*copy) == NamesInOrder(source));

	copiedChild.GetComponent<MeshComponent>().Mesh = UUID(5);
	copy->DestroyEntity(copy->GetEntityByUUID(parent.GetUUID()));
	CHECK(child.GetComponent<MeshComponent>().Mesh == UUID(99));
	CHECK(source.GetEntityCount() == 2);
}

TEST_CASE("Scene: lookup by name and tag, and primary camera selection")
{
	Scene scene;
	Entity a = scene.CreateEntity("Enemy");
	Entity b = scene.CreateEntity("Enemy");
	Entity cameraA = scene.CreateEntity("CameraA");
	Entity cameraB = scene.CreateEntity("CameraB");
	a.GetComponent<TagComponent>().Tag = "Bad";
	b.GetComponent<TagComponent>().Tag = "Bad";
	cameraA.AddComponent<CameraComponent>().Primary = false;
	cameraB.AddComponent<CameraComponent>().Primary = true;

	CHECK(scene.FindEntityByName("Enemy") == a);
	CHECK_FALSE(scene.FindEntityByName("Nobody").has_value());
	CHECK(scene.FindEntitiesByTag("Bad").size() == 2);
	CHECK(scene.GetPrimaryCameraEntity() == cameraB);

	cameraB.GetComponent<CameraComponent>().Primary = false;
	CHECK_FALSE(scene.GetPrimaryCameraEntity().has_value());
}

TEST_CASE("Scene: ForEachEntity tolerates destruction of the visited entity")
{
	Scene scene;
	Entity a = scene.CreateEntity("A");
	scene.SetParent(scene.CreateEntity("AChild"), a);
	scene.CreateEntity("B");

	std::vector<std::string> visited;
	scene.ForEachEntity([&](Entity entity) {
		visited.push_back(entity.GetName());
		if (entity.GetName() == "A")
			scene.DestroyEntity(entity);
	});
	CHECK(visited == std::vector<std::string>{"A", "B"});
	CHECK(scene.GetEntityCount() == 1);
}

TEST_CASE("Scene: very deep hierarchies are handled iteratively (no stack overflow)")
{
	// Built through the linear-time loader: building it with SetParent would be quadratic (each call walks the
	// ancestor chain to reject cycles), which is too slow under sanitizers for this depth.
	constexpr uint64_t depth = 20000;
	nlohmann::json entities = nlohmann::json::array();
	for (uint64_t i = 1; i <= depth + 1; i++)
		entities.push_back({{"ID", std::to_string(i)}, {"Parent", std::to_string(i - 1)}});
	Scene scene;
	std::string error;
	REQUIRE(SceneSerializer::Deserialize(scene, {{"Paralax", {{"Type", "Scene"}, {"Version", 1}}}, {"Entities", entities}}, error));
	Entity root = scene.GetEntityByUUID(UUID(1));

	size_t visited = 0;
	scene.ForEachEntity([&](Entity) { visited++; });
	CHECK(visited == depth + 1);

	Entity copy = scene.DuplicateEntity(root);
	CHECK(scene.GetEntityCount() == 2 * (depth + 1));
	scene.DestroyEntity(copy);
	scene.DestroyEntity(root);
	CHECK(scene.GetEntityCount() == 0);
}

TEST_CASE("Scene: a copy's views visit entities in the source's order, even after edits reordered its storage")
{
	// The renderer takes "the first" directional or sky light from a view and sums lights in view order, so play mode
	// (a copy) must iterate exactly as the editor's scene does, both for a freshly loaded scene and after editing.
	Scene authored("Lights");
	Entity previous;
	for (int i = 0; i < 16; i++)
	{
		Entity light = authored.CreateEntity("Light" + std::to_string(i));
		light.AddComponent<DirectionalLightComponent>().Intensity = static_cast<float>(i);
		if (i % 2 == 0)
			light.AddComponent<SkyLightComponent>();
		if (i % 3 == 1)
			REQUIRE(authored.SetParent(light, previous));
		previous = light;
	}
	Scene source;
	std::string error;
	REQUIRE(SceneSerializer::Deserialize(source, SceneSerializer::Serialize(authored), error));
	{
		Ref<Scene> copy = Scene::Copy(source);
		CheckSameViewOrders(*copy, source);
	}

	// Edits that leave the pools out of hierarchy order: a duplicate (appended to every pool, placed mid-hierarchy),
	// reparenting and moving siblings (the hierarchy changes, the pools do not), a component added to a later entity
	// before an earlier one, and a destruction (each pool moves its last entry into the hole).
	Entity first = *source.FindEntityByName("Light0");
	Entity middle = *source.FindEntityByName("Light8");
	Entity last = *source.FindEntityByName("Light14");
	source.DuplicateEntity(*source.FindEntityByName("Light3"));
	REQUIRE(source.SetParent(first, last));
	source.SetSiblingIndex(last, 0);
	middle.AddComponent<PointLightComponent>(); // After the edits the hierarchy runs last, first, ..., middle.
	last.AddComponent<PointLightComponent>();
	first.AddComponent<PointLightComponent>();
	source.DestroyEntity(*source.FindEntityByName("Light6"));
	// The edits really did move storage away from hierarchy order: the same scene loaded from a file (stored in
	// hierarchy order, as a copy made in hierarchy order would be) visits its lights differently.
	Scene reloaded;
	REQUIRE(SceneSerializer::Deserialize(reloaded, SceneSerializer::Serialize(source), error));
	REQUIRE(ViewOrder<DirectionalLightComponent>(source) != ViewOrder<DirectionalLightComponent>(reloaded));
	REQUIRE(ViewOrder<PointLightComponent>(source) != ViewOrder<PointLightComponent>(reloaded));

	Ref<Scene> copy = Scene::Copy(source);
	CheckSameViewOrders(*copy, source);
	CHECK(NamesInOrder(*copy) == NamesInOrder(source));
	// A copy of the copy (a scene switch back to the level) is the same again.
	Ref<Scene> again = Scene::Copy(*copy);
	CheckSameViewOrders(*again, source);
}

TEST_CASE("Scene: Copy and Duplicate preserve component values exactly, even outside editor ranges")
{
	Scene scene;
	Entity lamp = scene.CreateEntity("Lamp");
	lamp.AddComponent<PointLightComponent>().Intensity = 200000.0f;
	lamp.AddComponent<CameraComponent>().Camera.Projection = static_cast<SceneCamera::ProjectionType>(1);

	Entity duplicate = scene.DuplicateEntity(lamp);
	CHECK(duplicate.GetComponent<PointLightComponent>().Intensity == 200000.0f);

	Ref<Scene> copy = Scene::Copy(scene);
	CHECK(copy->GetEntityByUUID(lamp.GetUUID()).GetComponent<PointLightComponent>().Intensity == 200000.0f);
}

TEST_CASE("Scene: duplicating a child keeps it under the same parent, after the original")
{
	Scene scene;
	Entity parent = scene.CreateEntity("Parent");
	Entity a = scene.CreateEntity("A");
	Entity b = scene.CreateEntity("B");
	scene.SetParent(a, parent);
	scene.SetParent(b, parent);

	Entity copy = scene.DuplicateEntity(a);
	CHECK(scene.GetParent(copy) == parent);
	CHECK(parent.GetComponent<RelationshipComponent>().Children == std::vector<UUID>{a.GetUUID(), copy.GetUUID(), b.GetUUID()});

	scene.SetSiblingIndex(b, 0);
	CHECK(parent.GetComponent<RelationshipComponent>().Children == std::vector<UUID>{b.GetUUID(), a.GetUUID(), copy.GetUUID()});
}

TEST_CASE("Scene: reparenting under a degenerate (zero-scale) parent never writes NaN")
{
	Paralax::Testing::ScopedLogCapture logs;
	Scene scene;
	Entity parent = scene.CreateEntity("Flat");
	parent.GetTransform().Scale = {1.0f, 0.0f, 1.0f};
	Entity child = scene.CreateEntity("Child");
	child.GetTransform().Translation = {1.0f, 2.0f, 3.0f};

	REQUIRE(scene.SetParent(child, parent, true));
	const TransformComponent& transform = child.GetTransform();
	for (int i = 0; i < 3; i++)
	{
		CHECK(std::isfinite(transform.Translation[i]));
		CHECK(std::isfinite(transform.Scale[i]));
	}
	CHECK(logs.Contains(spdlog::level::warn, "degenerate"));
}
