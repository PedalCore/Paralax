#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Animation/AnimationClip.h>
#include <Paralax/Animation/AnimationSystem.h>
#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace Paralax;

namespace {

	AnimationChannel Channel(AnimationProperty property, AnimationInterpolation interpolation, std::vector<float> times, std::vector<glm::vec4> values, std::vector<std::string> target = {})
	{
		AnimationChannel channel;
		channel.Target = std::move(target);
		channel.Property = property;
		channel.Interpolation = interpolation;
		channel.Times = std::move(times);
		channel.Values = std::move(values);
		return channel;
	}

	glm::vec4 Quat(const glm::quat& q)
	{
		return glm::vec4(q.x, q.y, q.z, q.w);
	}

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-5f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

	// A scene with an animation system and one clip (handle 5000) in memory.
	struct AnimationFixture
	{
		explicit AnimationFixture(std::vector<AnimationChannel> channels, float duration)
		{
			auto clip = CreateRef<AnimationClipAsset>();
			clip->Handle = AssetHandle(5000);
			clip->Name = "Test";
			clip->Duration = duration;
			clip->Channels = std::move(channels);
			std::string error;
			REQUIRE_MESSAGE(clip->Validate(error), error);
			Assets.AddMemoryAsset(clip);
			Clip = clip;
			World.AddSystem(CreateScope<AnimationSystem>(Assets));
		}
		~AnimationFixture() { World.OnRuntimeStop(); }

		Entity Animated(const std::string& name = "Animated")
		{
			Entity entity = World.CreateEntity(name);
			entity.AddComponent<AnimationComponent>().Clip = Clip->Handle;
			return entity;
		}

		void Update(int frames, float dt)
		{
			for (int i = 0; i < frames; i++)
				World.OnUpdateRuntime(Timestep(dt));
		}

		AssetManager Assets;
		Scene World{"AnimationTest"};
		Ref<AnimationClipAsset> Clip;
	};

}

TEST_CASE("AnimationClip: step, linear and cubic spline channels sample their keys, clamped at both ends")
{
	const AnimationChannel step = Channel(AnimationProperty::Translation, AnimationInterpolation::Step, {0.0f, 1.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(10.0f), glm::vec4(20.0f)});
	CHECK(AnimationClipAsset::Sample(step, -1.0f).x == 0.0f);
	CHECK(AnimationClipAsset::Sample(step, 0.99f).x == 0.0f);
	CHECK(AnimationClipAsset::Sample(step, 1.0f).x == 10.0f);
	CHECK(AnimationClipAsset::Sample(step, 5.0f).x == 20.0f);

	const AnimationChannel linear = Channel(AnimationProperty::Scale, AnimationInterpolation::Linear, {1.0f, 3.0f}, {glm::vec4(1.0f, 2.0f, 3.0f, 0.0f), glm::vec4(3.0f, 2.0f, 1.0f, 0.0f)});
	CHECK(Near(glm::vec3(AnimationClipAsset::Sample(linear, 2.0f)), glm::vec3(2.0f, 2.0f, 2.0f)));
	CHECK(Near(glm::vec3(AnimationClipAsset::Sample(linear, 0.0f)), glm::vec3(1.0f, 2.0f, 3.0f)));

	// Cubic spline (in-tangent, value, out-tangent per key): with zero tangents it eases; tangents are per second.
	const AnimationChannel eased = Channel(AnimationProperty::Translation, AnimationInterpolation::CubicSpline, {0.0f, 2.0f},
										   {glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(8.0f), glm::vec4(0.0f)});
	CHECK(AnimationClipAsset::Sample(eased, 1.0f).x == doctest::Approx(4.0));  // Half way, by symmetry.
	CHECK(AnimationClipAsset::Sample(eased, 0.5f).x == doctest::Approx(1.25)); // 3t^2 - 2t^3 at t = 0.25, times 8.
	const AnimationChannel straight = Channel(AnimationProperty::Translation, AnimationInterpolation::CubicSpline, {0.0f, 2.0f},
											  {glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(4.0f), glm::vec4(4.0f), glm::vec4(8.0f), glm::vec4(0.0f)});
	CHECK(AnimationClipAsset::Sample(straight, 0.5f).x == doctest::Approx(2.0)); // Tangents matching the slope: a straight line.
	// Which tangent is used where: the first key's out-tangent (index 2) and the second key's in-tangent (index 3); the
	// unused first in-tangent and last out-tangent are set to nonsense to show they are ignored.
	const AnimationChannel asymmetric = Channel(AnimationProperty::Translation, AnimationInterpolation::CubicSpline, {0.0f, 1.0f},
												{glm::vec4(99.0f), glm::vec4(0.0f), glm::vec4(1.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(-99.0f)});
	// h(t) = (t^3 - 2t^2 + t) * m0 with m0 = 1: at t = 0.5, 0.125.
	CHECK(AnimationClipAsset::Sample(asymmetric, 0.5f).x == doctest::Approx(0.125));

	// A repeated time is a jump.
	const AnimationChannel jump = Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f, 1.0f, 2.0f},
										  {glm::vec4(0.0f), glm::vec4(1.0f), glm::vec4(5.0f), glm::vec4(6.0f)});
	CHECK(AnimationClipAsset::Sample(jump, 1.5f).x == doctest::Approx(5.5));
	CHECK(AnimationClipAsset::Sample(jump, 0.5f).x == doctest::Approx(0.5));
}

TEST_CASE("AnimationClip: rotations interpolate spherically and come back normalised")
{
	const glm::quat a = glm::angleAxis(0.0f, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::quat b = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	const AnimationChannel rotation = Channel(AnimationProperty::Rotation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {Quat(a), Quat(b)});
	const glm::vec4 half = AnimationClipAsset::Sample(rotation, 0.5f);
	CHECK(glm::length(half) == doctest::Approx(1.0));
	const glm::quat sampled(half.w, half.x, half.y, half.z);
	CHECK(Near(sampled * glm::vec3(1.0f, 0.0f, 0.0f), glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec3(1.0f, 0.0f, 0.0f)));
	// Quaternions on opposite hemispheres (dot < 0) take the shorter arc: q and -q are the same rotation.
	const AnimationChannel flipped = Channel(AnimationProperty::Rotation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {Quat(a), -Quat(b)});
	const glm::vec4 flippedHalf = AnimationClipAsset::Sample(flipped, 0.5f);
	const glm::quat flippedRotation(flippedHalf.w, flippedHalf.x, flippedHalf.y, flippedHalf.z);
	CHECK(Near(flippedRotation * glm::vec3(1.0f, 0.0f, 0.0f), glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec3(1.0f, 0.0f, 0.0f)));
	// A cubic spline overshooting the unit sphere is normalised too.
	const AnimationChannel cubic = Channel(AnimationProperty::Rotation, AnimationInterpolation::CubicSpline, {0.0f, 1.0f},
										   {glm::vec4(0.0f), Quat(a), glm::vec4(0.0f, 3.0f, 0.0f, 0.0f), glm::vec4(0.0f), Quat(b), glm::vec4(0.0f)});
	CHECK(glm::length(AnimationClipAsset::Sample(cubic, 0.5f)) == doctest::Approx(1.0));
}

TEST_CASE("AnimationClip: validation rejects mismatched, unordered and non-finite keys")
{
	AnimationClipAsset clip;
	clip.Duration = 1.0f;
	std::string error;
	clip.Channels = {Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f)})};
	CHECK(clip.Validate(error));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	for (const AnimationChannel& bad : {Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {}, {}),
										Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {glm::vec4(0.0f)}),
										Channel(AnimationProperty::Translation, AnimationInterpolation::CubicSpline, {0.0f}, {glm::vec4(0.0f)}),
										Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {1.0f, 0.5f}, {glm::vec4(0.0f), glm::vec4(1.0f)}),
										Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {-1.0f, 0.5f}, {glm::vec4(0.0f), glm::vec4(1.0f)}),
										Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, nan}, {glm::vec4(0.0f), glm::vec4(1.0f)}),
										Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(nan)})})
	{
		clip.Channels = {bad};
		CHECK_FALSE(clip.Validate(error));
		CHECK_FALSE(error.empty());
	}
	clip.Channels = {};
	clip.Duration = -1.0f;
	CHECK_FALSE(clip.Validate(error));
}

TEST_CASE("AnimationSystem: a playing clip advances with time, loops, and poses its targets by path")
{
	// Moves the entity itself along x over 2 s, and its grandchild "Arm/Hand" up in y.
	AnimationFixture fixture({Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(4.0f, 0.0f, 0.0f, 0.0f)}),
							  Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 2.0f}, {glm::vec4(0.0f), glm::vec4(0.0f, 2.0f, 0.0f, 0.0f)}, {"Arm", "Hand"})},
							 2.0f);
	Entity body = fixture.Animated("Body");
	Entity arm = fixture.World.CreateEntity("Arm");
	Entity hand = fixture.World.CreateEntity("Hand");
	fixture.World.SetParent(arm, body, false);
	fixture.World.SetParent(hand, arm, false);
	body.GetComponent<TransformComponent>().Translation = glm::vec3(9.0f);

	fixture.World.OnRuntimeStart();
	CHECK(body.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f)); // Posed at time 0 from the start.
	fixture.Update(30, 1.0f / 60.0f);
	CHECK(body.GetComponent<AnimationComponent>().Time == doctest::Approx(0.5));
	CHECK(Near(body.GetComponent<TransformComponent>().Translation, glm::vec3(1.0f, 0.0f, 0.0f), 1e-4f));
	CHECK(Near(hand.GetComponent<TransformComponent>().Translation, glm::vec3(0.0f, 0.5f, 0.0f), 1e-4f));
	CHECK(arm.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f)); // Not animated.

	fixture.Update(120, 1.0f / 60.0f); // 2.5 s: looped once.
	CHECK(body.GetComponent<AnimationComponent>().Time == doctest::Approx(0.5).epsilon(1e-3));
	CHECK(body.GetComponent<AnimationComponent>().Playing);
}

TEST_CASE("AnimationSystem: a clip that does not loop stops at its end, either way, and a paused clip poses on a seek")
{
	AnimationFixture fixture({Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)})}, 1.0f);
	Entity forwards = fixture.Animated("Forwards");
	forwards.GetComponent<AnimationComponent>().Loop = false;
	Entity backwards = fixture.Animated("Backwards");
	auto& reverse = backwards.GetComponent<AnimationComponent>();
	reverse.Loop = false;
	reverse.Speed = -2.0f;
	reverse.Time = 0.8f;

	fixture.World.OnRuntimeStart();
	fixture.Update(90, 1.0f / 60.0f);
	CHECK_FALSE(forwards.GetComponent<AnimationComponent>().Playing);
	CHECK(forwards.GetComponent<AnimationComponent>().Time == 1.0f);
	CHECK(forwards.GetComponent<TransformComponent>().Translation.x == doctest::Approx(1.0));
	CHECK_FALSE(reverse.Playing);
	CHECK(reverse.Time == 0.0f);
	CHECK(backwards.GetComponent<TransformComponent>().Translation.x == 0.0f);

	// Stopped: moving the entity by hand sticks, until the clip is seeked.
	forwards.GetComponent<TransformComponent>().Translation.x = 7.0f;
	fixture.Update(1, 1.0f / 60.0f);
	CHECK(forwards.GetComponent<TransformComponent>().Translation.x == 7.0f);
	forwards.GetComponent<AnimationComponent>().Time = 0.25f;
	fixture.Update(1, 1.0f / 60.0f);
	CHECK(forwards.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.25));
	CHECK(forwards.GetComponent<AnimationComponent>().Time == 0.25f); // Paused: time does not advance.

	// Playing again from there.
	forwards.GetComponent<AnimationComponent>().Playing = true;
	fixture.Update(15, 1.0f / 60.0f);
	CHECK(forwards.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.5).epsilon(1e-3));
}

TEST_CASE("AnimationSystem: rotation and scale channels, and playback independent of the frame rate")
{
	const glm::quat quarter = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	std::vector<glm::vec3> positions;
	for (const int framesPerSecond : {30, 60, 144})
	{
		CAPTURE(framesPerSecond);
		AnimationFixture fixture({Channel(AnimationProperty::Rotation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {Quat(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), Quat(quarter)}),
								  Channel(AnimationProperty::Scale, AnimationInterpolation::Step, {0.0f, 0.5f}, {glm::vec4(1.0f), glm::vec4(2.0f)})},
								 1.0f);
		Entity entity = fixture.Animated();
		entity.GetComponent<AnimationComponent>().Loop = false;
		fixture.World.OnRuntimeStart();
		fixture.Update(framesPerSecond, 1.0f / static_cast<float>(framesPerSecond));
		const auto& transform = entity.GetComponent<TransformComponent>();
		CHECK(Near(transform.Rotation * glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 1e-4f));
		CHECK(transform.Scale == glm::vec3(2.0f));
		positions.push_back(transform.Rotation * glm::vec3(1.0f, 0.0f, 0.0f));
	}
	CHECK(Near(positions[0], positions[1], 1e-5f));
	CHECK(Near(positions[1], positions[2], 1e-5f));
}

TEST_CASE("AnimationSystem: missing clips and targets are reported once and skipped; bad numbers are made safe")
{
	AnimationFixture fixture({Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)}, {"Missing"})}, 1.0f);
	Entity lost = fixture.Animated("Lost");
	Entity unclipped = fixture.World.CreateEntity("Unclipped");
	unclipped.AddComponent<AnimationComponent>().Clip = AssetHandle(987654);
	Entity wrongType = fixture.World.CreateEntity("WrongType");
	wrongType.AddComponent<AnimationComponent>().Clip = AssetHandle(AssetManager::BuiltinCube);
	Entity strange = fixture.Animated("Strange");
	strange.GetComponent<AnimationComponent>().Speed = std::numeric_limits<float>::infinity();
	strange.GetComponent<AnimationComponent>().Time = std::numeric_limits<float>::quiet_NaN();

	Paralax::Testing::ScopedLogCapture capture;
	fixture.World.OnRuntimeStart();
	fixture.Update(10, 1.0f / 60.0f);
	size_t warnings = 0;
	for (const auto& entry : capture.GetEntries())
		warnings += entry.Message.find("Animation:") != std::string::npos ? 1 : 0;
	CHECK(warnings == 4); // Each problem once: the missing target (for both entities playing the clip), the unknown
						  // clip, and a mesh given as a clip.
	CHECK(std::isfinite(strange.GetComponent<AnimationComponent>().Time));
	CHECK(lost.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));

	// A target that appears later is found then.
	Entity found = fixture.World.CreateEntity("Missing");
	fixture.World.SetParent(found, lost, false);
	fixture.Update(30, 1.0f / 60.0f);
	CHECK(found.GetComponent<TransformComponent>().Translation.x > 0.0f);
	fixture.World.DestroyEntity(found);
	CHECK_NOTHROW(fixture.Update(2, 1.0f / 60.0f));
}

TEST_CASE("AnimationSystem: looping backwards wraps, seeks past the end wrap like playback, and single poses end at once")
{
	AnimationFixture fixture({Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)})}, 1.0f);
	Entity backwards = fixture.Animated("Backwards");
	backwards.GetComponent<AnimationComponent>().Speed = -1.0f;
	Entity seeking = fixture.Animated("Seeking");
	seeking.GetComponent<AnimationComponent>().Playing = false;
	fixture.World.OnRuntimeStart();
	fixture.Update(15, 1.0f / 60.0f); // 0.25 s backwards from 0: wraps to 0.75.
	CHECK(backwards.GetComponent<AnimationComponent>().Time == doctest::Approx(0.75).epsilon(1e-3));
	CHECK(backwards.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.75).epsilon(1e-3));

	// Paused and looping: a seek to 1.3 s shows the pose at 0.3 s, as playing on from there does.
	seeking.GetComponent<AnimationComponent>().Time = 1.3f;
	fixture.Update(1, 1.0f / 60.0f);
	CHECK(seeking.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.3).epsilon(1e-3));
	seeking.GetComponent<AnimationComponent>().Playing = true;
	fixture.Update(1, 0.0f);
	CHECK(seeking.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.3).epsilon(1e-3));

	// A single pose (zero duration) that does not loop ends at once, so scripts waiting for it to finish go on.
	AnimationFixture still({Channel(AnimationProperty::Translation, AnimationInterpolation::Step, {0.0f}, {glm::vec4(2.0f, 0.0f, 0.0f, 0.0f)})}, 0.0f);
	Entity pose = still.Animated("Pose");
	pose.GetComponent<AnimationComponent>().Loop = false;
	still.World.OnRuntimeStart();
	still.Update(1, 1.0f / 60.0f);
	CHECK_FALSE(pose.GetComponent<AnimationComponent>().Playing);
	CHECK(pose.GetComponent<TransformComponent>().Translation.x == 2.0f);
}

TEST_CASE("AnimationSystem: a renamed or moved target is let go, and a problem fixed and back is reported again")
{
	AnimationFixture fixture({Channel(AnimationProperty::Translation, AnimationInterpolation::Linear, {0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)}, {"Arm"})}, 1.0f);
	Entity body = fixture.Animated("Body");
	Entity arm = fixture.World.CreateEntity("Arm");
	fixture.World.SetParent(arm, body, false);
	Paralax::Testing::ScopedLogCapture capture;
	auto warnings = [&] {
		size_t count = 0;
		for (const auto& entry : capture.GetEntries())
			count += entry.Message.find("not below it") != std::string::npos ? 1 : 0;
		return count;
	};
	fixture.World.OnRuntimeStart();
	fixture.Update(15, 1.0f / 60.0f);
	CHECK(arm.GetComponent<TransformComponent>().Translation.x > 0.0f);
	CHECK(warnings() == 0);

	// Renamed: no longer the "Arm" the clip names, so it is left where it is.
	arm.GetComponent<TagComponent>().Name = "Elbow";
	fixture.Update(1, 1.0f / 60.0f);
	const float left = arm.GetComponent<TransformComponent>().Translation.x;
	fixture.Update(15, 1.0f / 60.0f);
	CHECK(arm.GetComponent<TransformComponent>().Translation.x == left);
	CHECK(warnings() == 1);

	// Named back: animated again; moved out from under the body: let go, and reported anew.
	arm.GetComponent<TagComponent>().Name = "Arm";
	fixture.Update(5, 1.0f / 60.0f);
	CHECK(arm.GetComponent<TransformComponent>().Translation.x != left);
	fixture.World.SetParent(arm, std::nullopt, false);
	fixture.Update(1, 1.0f / 60.0f);
	const float detached = arm.GetComponent<TransformComponent>().Translation.x;
	fixture.Update(15, 1.0f / 60.0f);
	CHECK(arm.GetComponent<TransformComponent>().Translation.x == detached);
	CHECK(warnings() == 2);
}
