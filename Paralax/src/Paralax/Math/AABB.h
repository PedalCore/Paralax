#pragma once

#include <glm/glm.hpp>

#include <limits>

namespace Paralax {

	// Axis-aligned bounding box. A default-constructed box is empty (invalid) and grows as points are added.
	struct AABB
	{
		glm::vec3 Min{std::numeric_limits<float>::max()};
		glm::vec3 Max{std::numeric_limits<float>::lowest()};

		bool IsValid() const { return Min.x <= Max.x && Min.y <= Max.y && Min.z <= Max.z; }

		void Expand(const glm::vec3& point)
		{
			Min = glm::min(Min, point);
			Max = glm::max(Max, point);
		}

		void Expand(const AABB& other)
		{
			if (other.IsValid())
			{
				Expand(other.Min);
				Expand(other.Max);
			}
		}

		glm::vec3 GetCenter() const { return (Min + Max) * 0.5f; }
		glm::vec3 GetExtents() const { return (Max - Min) * 0.5f; }

		// Bounds of this box after an affine transform (transforms all 8 corners).
		AABB Transformed(const glm::mat4& transform) const
		{
			AABB result;
			if (!IsValid())
				return result;
			for (int i = 0; i < 8; i++)
			{
				const glm::vec3 corner((i & 1) ? Max.x : Min.x, (i & 2) ? Max.y : Min.y, (i & 4) ? Max.z : Min.z);
				result.Expand(glm::vec3(transform * glm::vec4(corner, 1.0f)));
			}
			return result;
		}
	};

}
