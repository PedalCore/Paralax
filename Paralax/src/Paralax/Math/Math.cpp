#include "pxpch.h"
#include "Paralax/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>
#include <cmath>

namespace Paralax::Math {

	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
	{
		return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
	}

	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale)
	{
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (!std::isfinite(transform[column][row]))
					return false;
			}
		}

		// Lengths and the determinant are taken in double: in float, squaring an entry above about 1.8e19 overflows,
		// so a finite scale such as 2e19 came out infinite (which a scene cannot store). A length no float can hold is
		// a degenerate matrix.
		const glm::dvec3 exact[3] = {glm::dvec3(transform[0]), glm::dvec3(transform[1]), glm::dvec3(transform[2])};
		const glm::dvec3 lengths(glm::length(exact[0]), glm::length(exact[1]), glm::length(exact[2]));
		if (lengths.x > static_cast<double>(FLT_MAX) || lengths.y > static_cast<double>(FLT_MAX) || lengths.z > static_cast<double>(FLT_MAX))
			return false;
		glm::vec3 scale(lengths);

		// Written as !(x >= epsilon) so NaN scales are rejected too.
		constexpr float epsilon = 1e-8f;
		if (!(scale.x >= epsilon) || !(scale.y >= epsilon) || !(scale.z >= epsilon))
			return false;

		// A reflection (negative determinant) is represented as a negative X scale.
		if (glm::dot(exact[0], glm::cross(exact[1], exact[2])) < 0.0)
			scale.x = -scale.x;

		const glm::vec3 columns[3] = {glm::vec3(transform[0]), glm::vec3(transform[1]), glm::vec3(transform[2])};

		const glm::mat3 rotationMatrix(columns[0] / scale.x, columns[1] / scale.y, columns[2] / scale.z);
		outRotation = glm::normalize(glm::quat_cast(rotationMatrix));
		outTranslation = glm::vec3(transform[3]);
		outScale = scale;
		return true;
	}

	glm::quat EulerToQuat(const glm::vec3& eulerRadians)
	{
		return glm::quat(eulerRadians);
	}

	glm::vec3 QuatToEuler(const glm::quat& rotation)
	{
		return glm::eulerAngles(rotation);
	}

}
