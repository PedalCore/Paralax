#include "pxpch.h"
#include "Paralax/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace Paralax::Math {

	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
	{
		return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
	}

	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale)
	{
		glm::vec3 columns[3] = {glm::vec3(transform[0]), glm::vec3(transform[1]), glm::vec3(transform[2])};
		glm::vec3 scale(glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]));

		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (!std::isfinite(transform[column][row]))
					return false;
			}
		}

		// Written as !(x >= epsilon) so NaN scales are rejected too.
		constexpr float epsilon = 1e-8f;
		if (!(scale.x >= epsilon) || !(scale.y >= epsilon) || !(scale.z >= epsilon))
			return false;

		// A reflection (negative determinant) is represented as a negative X scale.
		if (glm::dot(columns[0], glm::cross(columns[1], columns[2])) < 0.0f)
			scale.x = -scale.x;

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
