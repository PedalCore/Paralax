#include "pxpch.h"
#include "Paralax/Scene/SceneCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Paralax {

	glm::mat4 SceneCamera::GetProjection(float aspectRatio) const
	{
		if (!(aspectRatio > 0.0f))
			aspectRatio = 1.0f;

		if (Projection == ProjectionType::Perspective)
		{
			// Reversed Z: swap near and far in a zero-to-one, right-handed projection.
			return glm::perspectiveRH_ZO(PerspectiveFOV, aspectRatio, PerspectiveFar, PerspectiveNear);
		}

		const float halfHeight = OrthographicSize * 0.5f;
		const float halfWidth = halfHeight * aspectRatio;
		return glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, OrthographicFar, OrthographicNear);
	}

}
