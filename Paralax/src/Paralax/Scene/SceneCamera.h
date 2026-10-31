#pragma once

#include <glm/glm.hpp>

namespace Paralax {

	// Camera projection parameters stored on a CameraComponent. View transform comes from the entity.
	struct SceneCamera
	{
		enum class ProjectionType
		{
			Perspective = 0,
			Orthographic = 1
		};

		ProjectionType Projection = ProjectionType::Perspective;

		float PerspectiveFOV = glm::radians(60.0f); // Vertical field of view, radians.
		float PerspectiveNear = 0.1f;
		float PerspectiveFar = 1000.0f;

		float OrthographicSize = 10.0f; // Full height of the view volume.
		float OrthographicNear = -100.0f;
		float OrthographicFar = 100.0f;

		// Projection for the given aspect ratio (width / height), D3D-style clip space with reversed-Z depth:
		// the near plane maps to depth 1 and the far plane to 0, for precision at distance.
		glm::mat4 GetProjection(float aspectRatio) const;
	};

}
