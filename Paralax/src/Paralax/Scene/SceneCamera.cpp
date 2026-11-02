#include "pxpch.h"
#include "Paralax/Scene/SceneCamera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Paralax {

	namespace {

		const SceneCamera s_Defaults{};

		bool ClipPlanesValid(float nearPlane, float farPlane, bool perspective)
		{
			return std::isfinite(nearPlane) && std::isfinite(farPlane) && farPlane > nearPlane && (!perspective || nearPlane > 0.0f);
		}

		// NaN planes are one setting, not a new one on every comparison.
		bool SamePlane(float a, float b)
		{
			return a == b || (std::isnan(a) && std::isnan(b));
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
		// An infinite ratio would make the orthographic extents infinite (and the projection NaN).
		if (!(aspectRatio > 0.0f) || !std::isfinite(aspectRatio))
			aspectRatio = 1.0f;
		// Past these, the orthographic extents (huge ratios) or the perspective's horizontal scale (tiny ones)
		// overflow; no viewport is that far from square.
		aspectRatio = std::clamp(aspectRatio, 1e-4f, 1e4f);

		if (HasValidClipPlanes())
			m_ReportedClipPlanes.reset();
		else
		{
			const bool perspective = Projection == ProjectionType::Perspective;
			const ReportedClipPlanes planes{Projection, perspective ? PerspectiveNear : OrthographicNear, perspective ? PerspectiveFar : OrthographicFar};
			if (!m_ReportedClipPlanes || m_ReportedClipPlanes->Projection != planes.Projection || !SamePlane(m_ReportedClipPlanes->Near, planes.Near) || !SamePlane(m_ReportedClipPlanes->Far, planes.Far))
			{
				m_ReportedClipPlanes = planes;
				PX_CORE_WARN("Camera: {} clip planes near {} and far {} cannot form a projection (both must be finite{} and far must exceed near); using the defaults until they change",
							 perspective ? "perspective" : "orthographic", planes.Near, planes.Far, perspective ? ", near positive" : "");
			}
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
