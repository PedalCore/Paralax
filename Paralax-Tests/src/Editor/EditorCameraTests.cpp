#include <doctest/doctest.h>

#include <EditorCore/EditorCamera.h>

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
