#pragma once

#include <glm/glm.hpp>

#include <optional>

namespace Paralax {

	// Camera projection parameters stored on a CameraComponent. View transform comes from the entity. Main-thread-only:
	// GetProjection records which invalid planes it has reported.
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

		// Whether the active projection's clip planes can form a projection: both finite, far greater than near, and
		// near positive for perspective. Each plane's reflected range allows pairs that are not (near == far), which
		// would divide by zero; GetProjection and the effective planes then use the defaults instead.
		bool HasValidClipPlanes() const;
		// The clip planes GetProjection uses: the camera's own when they are valid, otherwise the defaults of the
		// active projection. Read these, not the fields, wherever the planes must agree with the projection.
		float GetEffectiveNear() const;
		float GetEffectiveFar() const;

		// Projection for the given aspect ratio (width / height), D3D-style clip space with reversed-Z depth:
		// the near plane maps to depth 1 and the far plane to 0, for precision at distance. Finite for any aspect ratio
		// and clip planes: a ratio that is not a positive finite number is treated as 1, and planes that cannot form a
		// projection as the defaults (with a warning, once per camera until the planes change). The field of view and
		// orthographic size are not checked here; their reflected ranges keep them usable.
		glm::mat4 GetProjection(float aspectRatio) const;

	private:
		// The invalid planes this camera last warned about: cameras are projected every frame, so a bad setting is
		// reported once, and again only after the planes change. Not a setting (never reflected or serialized); a
		// copy of the camera has already reported what the original has.
		struct ReportedClipPlanes
		{
			ProjectionType Projection;
			float Near;
			float Far;
		};
		mutable std::optional<ReportedClipPlanes> m_ReportedClipPlanes;
	};

}
