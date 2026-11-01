#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorCamera.h>
#include <EditorCore/FocusBounds.h>

#include <Paralax/Scene/Components.h>

#include <glm/gtc/epsilon.hpp>

using namespace Paralax;
using namespace Paralax::Editor;

TEST_CASE("EditorCamera: LookAt places the camera and aims it at the target")
{
	const glm::vec3 targets[] = {glm::vec3(0.0f), glm::vec3(3.0f, -1.0f, 2.0f)};
	const glm::vec3 positions[] = {glm::vec3(0.0f, 2.0f, 6.0f), glm::vec3(-4.0f, 5.0f, -3.0f), glm::vec3(5.0f, 0.0f, 0.0f)};
	for (const glm::vec3& target : targets)
	{
		for (const glm::vec3& position : positions)
		{
			EditorCamera camera;
			camera.LookAt(position, target);
			CHECK(glm::all(glm::epsilonEqual(camera.GetPosition(), position, 1e-3f)));
			CHECK(glm::all(glm::epsilonEqual(camera.GetFocalPoint(), target, 1e-5f)));
			CHECK(glm::all(glm::epsilonEqual(camera.GetForward(), glm::normalize(target - position), 1e-4f)));
		}
	}
}

TEST_CASE("EditorCamera: LookAt ignores coincident or non-finite points")
{
	EditorCamera camera;
	const glm::vec3 before = camera.GetPosition();
	camera.LookAt(glm::vec3(1.0f), glm::vec3(1.0f));
	camera.LookAt(glm::vec3(std::numeric_limits<float>::quiet_NaN()), glm::vec3(0.0f));
	CHECK(glm::all(glm::epsilonEqual(camera.GetPosition(), before, 1e-6f)));
}

TEST_CASE("EditorCamera: Focus frames a sphere in front of the camera")
{
	EditorCamera camera;
	camera.Focus(glm::vec3(10.0f, 0.0f, 0.0f), 2.0f);
	CHECK(glm::all(glm::epsilonEqual(camera.GetFocalPoint(), glm::vec3(10.0f, 0.0f, 0.0f), 1e-6f)));
	const float distance = glm::length(camera.GetPosition() - camera.GetFocalPoint());
	// The whole sphere fits in the vertical field of view.
	CHECK(std::asin(2.0f / distance) < camera.GetVerticalFOV() * 0.5f);
}

TEST_CASE("FocusBounds: an entity frames its own mesh and every mesh below it")
{
	Scene scene;
	AssetManager assets;
	// A group without a mesh (like an imported model's prefab root) holding two cubes, one nested a level deeper.
	Entity group = scene.CreateEntity("Group");
	group.GetComponent<TransformComponent>().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
	Entity left = scene.CreateEntity("Left");
	left.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
	left.GetComponent<TransformComponent>().Translation = glm::vec3(-2.0f, 0.0f, 0.0f);
	scene.SetParent(left, group, false);
	Entity holder = scene.CreateEntity("Holder");
	scene.SetParent(holder, group, false);
	Entity right = scene.CreateEntity("Right");
	right.AddComponent<MeshComponent>().Mesh = AssetHandle(AssetManager::BuiltinCube);
	right.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f, 1.0f, 0.0f);
	scene.SetParent(right, holder, false);

	const AABB bounds = Paralax::Editor::ComputeFocusBounds(scene, assets, group);
	CHECK(bounds.Min == glm::vec3(7.5f, -0.5f, -0.5f));
	CHECK(bounds.Max == glm::vec3(12.5f, 1.5f, 0.5f));

	// A leaf frames just itself; an entity with no mesh at or below it gets a 1 m box at its position.
	const AABB leaf = Paralax::Editor::ComputeFocusBounds(scene, assets, right);
	CHECK(leaf.Min == glm::vec3(11.5f, 0.5f, -0.5f));
	Entity empty = scene.CreateEntity("Empty");
	empty.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 3.0f, 0.0f);
	const AABB fallback = Paralax::Editor::ComputeFocusBounds(scene, assets, empty);
	CHECK(fallback.Min == glm::vec3(-0.5f, 2.5f, -0.5f));
	CHECK(fallback.Max == glm::vec3(0.5f, 3.5f, 0.5f));

	// A mesh that cannot be loaded is skipped rather than framing the origin.
	right.GetComponent<MeshComponent>().Mesh = AssetHandle(987654321);
	Paralax::Testing::ScopedLogCapture capture;
	const AABB skipped = Paralax::Editor::ComputeFocusBounds(scene, assets, group);
	CHECK(skipped.Max == glm::vec3(8.5f, 0.5f, 0.5f));
}
