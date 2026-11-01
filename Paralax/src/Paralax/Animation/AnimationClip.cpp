#include "pxpch.h"
#include "Paralax/Animation/AnimationClip.h"

#include <algorithm>
#include <cmath>

namespace Paralax {

	namespace {

		bool IsFinite(const glm::vec4& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
		}

		glm::vec4 Normalized(const glm::vec4& q)
		{
			const float length = glm::length(q);
			return length > 1e-12f ? q / length : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		}

		glm::vec4 Slerp(const glm::vec4& a, const glm::vec4& b, float t)
		{
			const glm::quat from(a.w, a.x, a.y, a.z);
			const glm::quat to(b.w, b.x, b.y, b.z);
			const glm::quat result = glm::slerp(from, to, t); // Takes the shorter arc.
			return Normalized(glm::vec4(result.x, result.y, result.z, result.w));
		}

	}

	bool AnimationClipAsset::Validate(std::string& outError) const
	{
		if (!std::isfinite(Duration) || Duration < 0.0f)
		{
			outError = "the duration is not a finite, non-negative number";
			return false;
		}
		for (size_t i = 0; i < Channels.size(); i++)
		{
			const AnimationChannel& channel = Channels[i];
			const size_t valuesPerKey = channel.Interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
			if (channel.Times.empty() || channel.Values.size() != channel.Times.size() * valuesPerKey)
			{
				outError = fmt::format("channel {} has {} keys and {} values", i, channel.Times.size(), channel.Values.size());
				return false;
			}
			for (size_t k = 0; k < channel.Times.size(); k++)
			{
				if (!std::isfinite(channel.Times[k]) || channel.Times[k] < 0.0f || (k > 0 && channel.Times[k] < channel.Times[k - 1]))
				{
					outError = fmt::format("channel {} has key times that are negative, not finite or out of order", i);
					return false;
				}
			}
			if (!std::all_of(channel.Values.begin(), channel.Values.end(), [](const glm::vec4& value) { return IsFinite(value); }))
			{
				outError = fmt::format("channel {} has values that are not finite", i);
				return false;
			}
		}
		return true;
	}

	glm::vec4 AnimationClipAsset::Sample(const AnimationChannel& channel, float time)
	{
		const std::vector<float>& times = channel.Times;
		const bool cubic = channel.Interpolation == AnimationInterpolation::CubicSpline;
		const bool rotation = channel.Property == AnimationProperty::Rotation;
		auto value = [&](size_t key) { return channel.Values[cubic ? key * 3 + 1 : key]; };
		auto finish = [&](const glm::vec4& result) { return rotation ? Normalized(result) : result; };

		if (time <= times.front() || times.size() == 1)
			return finish(value(0));
		if (time >= times.back())
			return finish(value(times.size() - 1));

		// The last key at or before time (keys may repeat a time: a jump).
		const size_t next = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin());
		const size_t key = next - 1;
		const float span = times[next] - times[key];
		const float t = span > 0.0f ? (time - times[key]) / span : 0.0f;

		switch (channel.Interpolation)
		{
			case AnimationInterpolation::Step:
				return finish(value(key));
			case AnimationInterpolation::Linear:
				return rotation ? Slerp(value(key), value(next), t) : glm::mix(value(key), value(next), t);
			case AnimationInterpolation::CubicSpline:
			{
				// Hermite basis; the tangents are per second, so they scale with the span.
				const glm::vec4 p0 = value(key);
				const glm::vec4 m0 = channel.Values[key * 3 + 2] * span;
				const glm::vec4 p1 = value(next);
				const glm::vec4 m1 = channel.Values[next * 3] * span;
				const float t2 = t * t;
				const float t3 = t2 * t;
				return finish((2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * m0 + (-2.0f * t3 + 3.0f * t2) * p1 + (t3 - t2) * m1);
			}
		}
		return finish(value(key));
	}

}
