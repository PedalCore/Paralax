#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Paralax::Math {

	// Builds a TRS matrix (scale, then rotate, then translate).
	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);

	// Splits an affine matrix into translation, rotation and scale. Returns false for degenerate matrices (zero scale
	// on any axis, a scale too large for a float, or non-finite elements), leaving the outputs unchanged. Negative
	// scale is folded into the X axis.
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale);

	// Euler angles (radians, XYZ intrinsic as used by glm's eulerAngles) <-> quaternion.
	glm::quat EulerToQuat(const glm::vec3& eulerRadians);
	glm::vec3 QuatToEuler(const glm::quat& rotation);

}
