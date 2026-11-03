#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Renderer/TemporalState.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>

using namespace Paralax;

namespace {

	TemporalFrameSnapshot Frame()
	{
		TemporalFrameSnapshot frame;
		frame.Scene = CreateRef<int>(0);
		frame.InternalSize = {65, 49};
		frame.OutputSize = {129, 97};
		frame.Projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 129.0f / 97, 100.0f, 0.1f);
		return frame;
	}

	TemporalDrawSnapshot Draw(uint64_t entity)
	{
		TemporalDrawSnapshot draw;
		draw.Key.Entity = entity;
		draw.Key.Mesh = 1;
		return draw;
	}

}

TEST_CASE("TemporalState: jitter is an internal raster displacement at every depth for both projection types")
{
	const glm::uvec2 size(65, 49);
	const glm::vec2 jitter(0.25f, -0.375f);
	for (const auto& projection : {Frame().Projection, glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 10.0f, -1.0f)})
	{
		const auto raster = TemporalState::JitterProjection(projection, size, jitter);
		for (float depth : {-0.2f, -1.0f, -9.0f})
		{
			const auto a = projection * glm::vec4(0.1f, 0.1f, depth, 1);
			const auto b = raster * glm::vec4(0.1f, 0.1f, depth, 1);
			const glm::vec2 displacement = (glm::vec2(b) / b.w - glm::vec2(a) / a.w) * glm::vec2(size) * glm::vec2(0.5f, -0.5f);
			CHECK(glm::length(displacement - jitter) < 1e-5f);
		}
	}
	Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(TemporalState::JitterProjection(glm::mat4(1), {0, 1}, {}), Testing::AssertionFailedError);
	CHECK_THROWS_AS(TemporalState::JitterProjection(glm::mat4(1), size, {std::numeric_limits<float>::quiet_NaN(), 0}), Testing::AssertionFailedError);
}

TEST_CASE("TemporalState: only submission commits transforms and deterministic sample advancement")
{
	TemporalState state;
	auto frame = Frame();
	std::array draws{Draw(1)};
	REQUIRE(state.Stage(frame, draws));
	CHECK_FALSE(state.CanUseHistory());
	CHECK(state.GetJitter() == glm::vec2(0));
	CHECK(state.GetDrawRecords()[0].Reactive);
	CHECK(state.GetCompletedFrames() == 0);
	state.Commit();
	CHECK(state.GetCompletedFrames() == 1);
	CHECK(state.GetSampleIndex() == 1);
	const auto previous = draws[0].World;
	draws[0].World[3].x = 1;
	REQUIRE(state.Stage(frame, draws));
	CHECK(state.CanUseHistory());
	CHECK_FALSE(state.GetDrawRecords()[0].Reactive);
	CHECK(state.GetDrawRecords()[0].PreviousWorld == previous);
	CHECK(state.GetCompletedFrames() == 1);
	state.Abort("injected allocation failure");
	CHECK_FALSE(state.IsValid());
	CHECK(state.GetCompletedFrames() == 1);
	CHECK(state.GetSampleIndex() == 0);
	REQUIRE(state.Stage(frame, draws));
	CHECK_FALSE(state.CanUseHistory());
	CHECK(state.GetDrawRecords()[0].Reactive);
	state.Commit();
	for (int frameIndex = 1; frameIndex < 8; ++frameIndex)
	{
		CHECK(glm::all(glm::lessThanEqual(glm::abs(state.GetJitter()), glm::vec2(0.5f))));
		REQUIRE(state.Stage(frame, draws));
		state.Commit();
	}
	CHECK(state.GetSampleIndex() == 0);
	CHECK(state.GetJitter() == glm::vec2(0));
	Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(state.Commit(), Testing::AssertionFailedError);
	REQUIRE(state.Stage(frame, draws));
	CHECK_THROWS_AS(state.Stage(frame, draws), Testing::AssertionFailedError);
}

TEST_CASE("TemporalState: surface mutation rotation deformation and ID retirement reject stale history")
{
	TemporalState state;
	auto frame = Frame();
	std::vector draws{Draw(1), Draw(2)};
	REQUIRE(state.Stage(frame, draws));
	state.Commit();
	const auto original = draws[0];
	for (int mutation = 0; mutation < 4; ++mutation)
	{
		draws[0] = original;
		if (mutation == 0)
			++draws[0].Version;
		else if (mutation == 1)
			draws[0].World = glm::rotate(glm::mat4(1), 0.1f, glm::vec3(0, 1, 0));
		else if (mutation == 2)
			draws[0].World[0][0] = -1;
		else
			draws[0].Deformed = true;
		REQUIRE(state.Stage(frame, draws));
		CHECK(state.GetDrawRecords()[0].Reactive);
		CHECK_FALSE(state.GetDrawRecords()[1].Reactive);
		state.Commit();
	}
	draws.erase(draws.begin());
	REQUIRE(state.Stage(frame, draws));
	CHECK_FALSE(state.CanUseHistory());
	CHECK(state.GetDrawRecords()[0].Reactive);
	CHECK(state.GetSampleIndex() == 0);
	state.Commit();
	draws.push_back(Draw(3));
	REQUIRE(state.Stage(frame, draws));
	CHECK_FALSE(state.GetDrawRecords()[0].Reactive);
	CHECK(state.GetDrawRecords()[1].Reactive);
	CHECK(state.GetDrawRecords()[0].ID != state.GetDrawRecords()[1].ID);
}

TEST_CASE("TemporalState: independent owners retain scene identity and reset only their own incompatible view")
{
	TemporalState viewport, capture;
	auto frame = Frame();
	std::array draws{Draw(1)};
	REQUIRE(viewport.Stage(frame, draws));
	viewport.Commit();
	auto captureFrame = frame;
	captureFrame.InternalSize = {17, 13};
	for (int i = 0; i < 3; ++i)
	{
		REQUIRE(capture.Stage(captureFrame, draws));
		capture.Commit();
	}
	CHECK(viewport.GetCompletedFrames() == 1);
	CHECK(viewport.GetSampleIndex() == 1);
	CHECK(capture.GetSampleIndex() == 3);
	for (int change = 0; change < 6; ++change)
	{
		auto changed = frame;
		if (change == 0)
			changed.Scene = CreateRef<int>(0);
		else if (change == 1)
			++changed.InternalSize.x;
		else if (change == 2)
			++changed.Settings;
		else if (change == 3)
			changed.Projection[0][0] += 0.01f;
		else if (change == 4)
			changed.CameraCut = true;
		else
			changed.CameraIdentity = 42;
		REQUIRE(viewport.Stage(changed, draws));
		CHECK_FALSE(viewport.CanUseHistory());
		viewport.Commit();
		viewport.Reset("test baseline");
		REQUIRE(viewport.Stage(frame, draws));
		viewport.Commit();
	}
	std::weak_ptr<const void> weak = frame.Scene;
	frame.Scene.reset();
	captureFrame.Scene.reset();
	CHECK_FALSE(weak.expired());
}

TEST_CASE("TemporalState: invalid input and capacity overflow fall back without dropping geometry or advancing history")
{
	TemporalState state;
	auto frame = Frame();
	std::vector<TemporalDrawSnapshot> draws;
	draws.reserve(TemporalState::MaxDraws + 1);
	for (uint32_t id = 0; id <= TemporalState::MaxDraws; ++id)
		draws.push_back(Draw(id + 1));
	CHECK_FALSE(state.Stage(frame, draws));
	CHECK(state.GetCompletedFrames() == 0);
	draws.pop_back();
	REQUIRE(state.Stage(frame, draws));
	CHECK(state.GetDrawRecords().size() == TemporalState::MaxDraws);
	state.Commit();
	draws.resize(2);
	draws[1] = draws[0];
	CHECK_FALSE(state.Stage(frame, draws));
	CHECK(state.GetCompletedFrames() == 1);
	frame.Projection = glm::mat4(0);
	CHECK_FALSE(state.Stage(frame, {}));
}

TEST_CASE("TemporalState: finite matrix entries with overflowing determinant or inverse reject before jitter")
{
	TemporalState state;
	const auto good = Frame();
	REQUIRE(state.Stage(good, {}));
	state.Commit();
	glm::mat4 mixed(1);
	mixed[0][0] = 1e30f;
	mixed[1][1] = 1e30f;
	mixed[2][2] = 1e-30f;
	mixed[3][3] = 1e-30f;
	for (const auto& invalid : {glm::mat4(1e30f), mixed})
		for (bool view : {true, false})
		{
			auto frame = good;
			(view ? frame.View : frame.Projection) = invalid;
			CHECK_FALSE(state.Stage(frame, {}));
			CHECK_FALSE(state.IsValid());
			CHECK_FALSE(state.IsStaged());
			CHECK(state.GetCompletedFrames() == 1);
			CHECK(state.GetSampleIndex() == 0);
		}
}

TEST_CASE("TemporalState: large valid orthographic views retain finite invertible projections")
{
	TemporalState state;
	auto frame = Frame();
	frame.Projection = glm::orthoRH_ZO(-1000000.0f, 1000000.0f, -1000000.0f, 1000000.0f, 1000000.0f, -1000000.0f);
	REQUIRE(std::abs(glm::determinant(frame.Projection)) < 1e-12f);
	REQUIRE(state.Stage(frame, {}));
	state.Commit();
	REQUIRE(state.Stage(frame, {}));
	CHECK(state.CanUseHistory());
}
