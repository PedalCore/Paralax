#pragma once

#include "Paralax/Asset/Asset.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Paralax {

	enum class AnimationProperty : uint8_t
	{
		Translation,
		Rotation,
		Scale
	};

	enum class AnimationInterpolation : uint8_t
	{
		Step,
		Linear,     // Rotations are interpolated spherically.
		CubicSpline // Hermite, with an in-tangent and out-tangent per key (glTF's CUBICSPLINE).
	};

	// Keyframes of one transform property of one entity.
	struct AnimationChannel
	{
		// The animated entity, as names of entities below the entity that plays the clip (empty: that entity itself).
		std::vector<std::string> Target;
		AnimationProperty Property = AnimationProperty::Translation;
		AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
		// Key times in seconds, non-decreasing.
		std::vector<float> Times;
		// One value per key (x, y, z for translation and scale; a quaternion as x, y, z, w for rotation); CubicSpline
		// keys hold three: in-tangent, value, out-tangent.
		std::vector<glm::vec4> Values;
	};

	// Keyframed transform animation (from a glTF model's animations), played by an AnimationComponent. Main-thread-only.
	class AnimationClipAsset : public Asset
	{
	public:
		AssetType GetType() const override { return AssetType::AnimationClip; }

		std::string Name;
		float Duration = 0.0f; // Seconds: the last key time of any channel.
		std::vector<AnimationChannel> Channels;

		// Checks every channel: keys present, finite, non-decreasing times, and the number of values matching the
		// keys and interpolation. Returns false and describes the first problem in outError.
		bool Validate(std::string& outError) const;

		// The channel's value at a time (clamped to its first and last keys): x, y, z for translation and scale, and a
		// normalised quaternion as x, y, z, w for rotation. The channel must be valid.
		static glm::vec4 Sample(const AnimationChannel& channel, float time);
	};

}
