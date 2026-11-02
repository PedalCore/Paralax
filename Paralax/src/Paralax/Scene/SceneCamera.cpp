#include "pxpch.h"
#include "Paralax/Scene/SceneCamera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace Paralax {

	namespace {

		const SceneCamera s_Defaults{};

		// Cameras are projected every frame, so an authoring mistake is reported once rather than flooding the log.
		bool s_ReportedInvalidClipPlanes = false;

		bool ClipPlanesValid(float nearPlane, float farPlane, bool perspective)
		{
			return std::isfinite(nearPlane) && std::isfinite(farPlane) && farPlane > nearPlane && (!perspective || nearPlane > 0.0f);
		}

	}

	bool SceneCamera::HasValidClipPlanes() const
	{
		return Projection == ProjectionType::Perspective ? ClipPlanesValid(PerspectiveNear, PerspectiveFar, true) : ClipPlanesValid(OrthographicNear, OrthographicFar, false);
	}

	float SceneCamera::GetEffectiveNear() const
	{
		const SceneCamera& camera = HasValidClipPlanes() ? *this : s_Defaults;
		return Projection == ProjectionType::Perspective ? camera.PerspectiveNear : camera.OrthographicNear;
	}

	float SceneCamera::GetEffectiveFar() const
	{
		const SceneCamera& camera = HasValidClipPlanes() ? *this : s_Defaults;
		return Projection == ProjectionType::Perspective ? camera.PerspectiveFar : camera.OrthographicFar;
	}

	glm::mat4 SceneCamera::GetProjection(float aspectRatio) const
	{
		if (!(aspectRatio > 0.0f))
			aspectRatio = 1.0f;

		if (!HasValidClipPlanes() && !s_ReportedInvalidClipPlanes)
		{
			s_ReportedInvalidClipPlanes = true;
			const bool perspective = Projection == ProjectionType::Perspective;
			PX_CORE_WARN("Camera: {} clip planes near {} and far {} cannot form a projection (both must be finite{} and far must exceed near); using the defaults. Reported once.",
						 perspective ? "perspective" : "orthographic", perspective ? PerspectiveNear : OrthographicNear, perspective ? PerspectiveFar : OrthographicFar,
						 perspective ? ", near positive" : "");
		}
		const float nearPlane = GetEffectiveNear();
		const float farPlane = GetEffectiveFar();

		if (Projection == ProjectionType::Perspective)
		{
			// Reversed Z: swap near and far in a zero-to-one, right-handed projection.
			return glm::perspectiveRH_ZO(PerspectiveFOV, aspectRatio, farPlane, nearPlane);
		}

		const float halfHeight = OrthographicSize * 0.5f;
		const float halfWidth = halfHeight * aspectRatio;
		return glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, farPlane, nearPlane);
	}

}
