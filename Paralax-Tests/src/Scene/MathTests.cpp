#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Math/Math.h>
#include <Paralax/Scene/SceneCamera.h>

#include <glm/gtc/epsilon.hpp>

#include <limits>

using namespace Paralax;

namespace {

	bool NearVec(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

	bool SameRotation(const glm::quat& a, const glm::quat& b)
	{
		return std::abs(glm::dot(a, b)) > 1.0f - 1e-5f;
	}

}

TEST_CASE("Math: compose and decompose round-trip translation, rotation and scale")
{
	const glm::vec3 translation(1.0f, -2.0f, 3.5f);
	const glm::quat rotation = Math::EulerToQuat(glm::radians(glm::vec3(30.0f, 45.0f, -60.0f)));
	const glm::vec3 scale(2.0f, 0.5f, 3.0f);

	glm::vec3 t, s;
	glm::quat r;
	REQUIRE(Math::DecomposeTransform(Math::ComposeTransform(translation, rotation, scale), t, r, s));
	CHECK(NearVec(t, translation));
	CHECK(NearVec(s, scale));
	CHECK(SameRotation(r, rotation));
}

TEST_CASE("Math: decompose rejects degenerate matrices and preserves reflections")
{
	glm::vec3 t(9.0f), s(9.0f);
	glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
	CHECK_FALSE(Math::DecomposeTransform(Math::ComposeTransform(glm::vec3(0.0f), r, glm::vec3(1.0f, 0.0f, 1.0f)), t, r, s));
	CHECK(t == glm::vec3(9.0f));

	REQUIRE(Math::DecomposeTransform(Math::ComposeTransform(glm::vec3(0.0f), r, glm::vec3(-2.0f, 1.0f, 1.0f)), t, r, s));
	CHECK(NearVec(s, glm::vec3(-2.0f, 1.0f, 1.0f)));
}

TEST_CASE("Math: Euler/quaternion conversion round-trips")
{
	const glm::vec3 euler = glm::radians(glm::vec3(10.0f, 20.0f, 30.0f));
	CHECK(NearVec(Math::QuatToEuler(Math::EulerToQuat(euler)), euler));
}

TEST_CASE("SceneCamera: perspective projection uses reversed Z in D3D clip space")
{
	SceneCamera camera;
	camera.PerspectiveNear = 0.5f;
	camera.PerspectiveFar = 100.0f;
	const glm::mat4 projection = camera.GetProjection(16.0f / 9.0f);

	auto depthAt = [&](float viewZ) {
		const glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, viewZ, 1.0f);
		return clip.z / clip.w;
	};
	CHECK(depthAt(-0.5f) == doctest::Approx(1.0).epsilon(1e-5));
	CHECK(depthAt(-100.0f) == doctest::Approx(0.0).epsilon(1e-5));
	CHECK(depthAt(-10.0f) > depthAt(-50.0f));

	// +Y in view space is +Y in clip space (up).
	const glm::vec4 up = projection * glm::vec4(0.0f, 1.0f, -5.0f, 1.0f);
	CHECK(up.y > 0.0f);
}

TEST_CASE("SceneCamera: orthographic projection maps the view volume and handles bad aspect ratios")
{
	SceneCamera camera;
	camera.Projection = SceneCamera::ProjectionType::Orthographic;
	camera.OrthographicSize = 10.0f;
	const glm::mat4 projection = camera.GetProjection(2.0f);
	const glm::vec4 corner = projection * glm::vec4(10.0f, 5.0f, 0.0f, 1.0f);
	CHECK(corner.x == doctest::Approx(1.0));
	CHECK(corner.y == doctest::Approx(1.0));

	// Zero/NaN aspect falls back to 1 instead of producing NaNs.
	const glm::mat4 fallback = camera.GetProjection(0.0f);
	CHECK(std::isfinite(fallback[0][0]));
}

TEST_CASE("SceneCamera: clip planes that cannot form a projection fall back to the defaults, warned once")
{
	Paralax::Testing::ScopedLogCapture log;
	auto allFinite = [](const glm::mat4& m) {
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (!std::isfinite(m[column][row]))
					return false;
			}
		}
		return true;
	};
	const SceneCamera defaults;

	// Each plane is within its reflected range; together they divide by zero.
	SceneCamera camera;
	camera.PerspectiveNear = 10.0f;
	camera.PerspectiveFar = 10.0f;
	CHECK_FALSE(camera.HasValidClipPlanes());
	CHECK(camera.GetEffectiveNear() == defaults.PerspectiveNear);
	CHECK(camera.GetEffectiveFar() == defaults.PerspectiveFar);
	CHECK(camera.GetProjection(1.5f) == defaults.GetProjection(1.5f));
	CHECK(allFinite(camera.GetProjection(1.5f)));
	CHECK(log.Contains(spdlog::level::warn, "cannot form a projection"));

	camera.PerspectiveFar = 11.0f;
	CHECK(camera.HasValidClipPlanes());
	CHECK(camera.GetEffectiveNear() == 10.0f);
	CHECK(camera.GetEffectiveFar() == 11.0f);
	CHECK(camera.GetProjection(1.5f) != defaults.GetProjection(1.5f));
	for (float nearPlane : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
	{
		CAPTURE(nearPlane);
		camera.PerspectiveNear = nearPlane;
		CHECK_FALSE(camera.HasValidClipPlanes());
		CHECK(allFinite(camera.GetProjection(1.5f)));
	}

	// Orthographic planes may be negative; only the order and finiteness matter.
	SceneCamera orthographic;
	orthographic.Projection = SceneCamera::ProjectionType::Orthographic;
	SceneCamera orthographicDefaults = orthographic;
	orthographic.OrthographicNear = -5.0f;
	orthographic.OrthographicFar = 5.0f;
	CHECK(orthographic.HasValidClipPlanes());
	orthographic.OrthographicFar = -5.0f;
	CHECK_FALSE(orthographic.HasValidClipPlanes());
	CHECK(orthographic.GetEffectiveNear() == defaults.OrthographicNear);
	CHECK(orthographic.GetEffectiveFar() == defaults.OrthographicFar);
	CHECK(orthographic.GetProjection(2.0f) == orthographicDefaults.GetProjection(2.0f));
	orthographic.OrthographicFar = std::numeric_limits<float>::infinity();
	CHECK_FALSE(orthographic.HasValidClipPlanes());
	CHECK(allFinite(orthographic.GetProjection(2.0f)));
}
