#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Renderer/RenderProfiler.h>

#include <limits>
#include <vector>
#include <type_traits>

using namespace Paralax;
static_assert(!std::is_copy_constructible_v<RenderProfiler>);
static_assert(!std::is_copy_assignable_v<RenderProfiler>);
static_assert(!std::is_move_constructible_v<RenderProfiler>);
static_assert(!std::is_move_assignable_v<RenderProfiler>);

namespace {

	class FakeTimer : public nvrhi::RefCounter<nvrhi::ITimerQuery>
	{
	public:
		explicit FakeTimer(int& live)
			: m_Live(live)
		{
			++m_Live;
		}
		~FakeTimer() override { --m_Live; }
		std::optional<float> Seconds;
		bool Ready = true;

	private:
		int& m_Live;
	};

	class FakeCompletion : public nvrhi::RefCounter<nvrhi::IEventQuery>
	{
	public:
		bool Complete = false, Armed = false;
	};

	class FakeQueries : public RenderQueryFactory
	{
	public:
		int LiveTimers = 0;
		uint32_t Allocations = 0, FailAt = 0, TimerPolls = 0, Reads = 0, Resets = 0, Begins = 0;
		bool FailEvent = false, TimersReady = true;
		float Seconds = 0.001f;
		std::vector<FakeCompletion*> Events;
		std::vector<FakeTimer*> Timers;
		nvrhi::TimerQueryHandle CreateTimer(nvrhi::IDevice*) override
		{
			if (++Allocations == FailAt)
				return nullptr;
			auto* timer = new FakeTimer(LiveTimers);
			Timers.push_back(timer);
			return nvrhi::TimerQueryHandle::Create(timer);
		}
		nvrhi::EventQueryHandle CreateEvent(nvrhi::IDevice*) override
		{
			if (FailEvent)
				return nullptr;
			auto* event = new FakeCompletion;
			Events.push_back(event);
			return nvrhi::EventQueryHandle::Create(event);
		}
		void Begin(nvrhi::ICommandList*, nvrhi::ITimerQuery*) override { ++Begins; }
		void End(nvrhi::ICommandList*, nvrhi::ITimerQuery*) override {}
		void SetEvent(nvrhi::IDevice*, nvrhi::IEventQuery* event) override
		{
			auto* completion = static_cast<FakeCompletion*>(event);
			CHECK_FALSE(completion->Armed);
			completion->Armed = true;
			completion->Complete = false;
		}
		void ResetEvent(nvrhi::IDevice*, nvrhi::IEventQuery* event) override
		{
			auto* completion = static_cast<FakeCompletion*>(event);
			CHECK(completion->Complete);
			completion->Armed = false;
		}
		bool PollEvent(nvrhi::IDevice*, nvrhi::IEventQuery* event) override { return static_cast<FakeCompletion*>(event)->Complete; }
		bool PollTimer(nvrhi::IDevice*, nvrhi::ITimerQuery* query) override
		{
			++TimerPolls;
			return TimersReady && static_cast<FakeTimer*>(query)->Ready;
		}
		float ReadSeconds(nvrhi::IDevice*, nvrhi::ITimerQuery* query) override
		{
			++Reads;
			return static_cast<FakeTimer*>(query)->Seconds.value_or(Seconds);
		}
		void Reset(nvrhi::IDevice*, nvrhi::ITimerQuery*) override { ++Resets; }
		void CompleteAll()
		{
			for (auto* event : Events)
				event->Complete = true;
		}
	};

	RenderTimingContext Context(uint64_t frame)
	{
		RenderTimingContext context;
		context.Frame = frame;
		context.InternalWidth = 97;
		context.InternalHeight = 65;
		context.OutputWidth = 129;
		context.OutputHeight = 87;
		context.Settings = "{\"SSAOEnabled\":true}";
		return context;
	}

}

TEST_CASE("RenderProfiler: reused available timestamps wait for the exact new submission and full rings skip")
{
	FakeQueries queries;
	{
		RenderProfiler profiler(nullptr, 64, 1, &queries);
		for (uint64_t frame = 1; frame <= RenderProfiler::SlotCount; ++frame)
		{
			REQUIRE(profiler.BeginFrame(true, nullptr, Context(frame), 0));
			profiler.EndFrame(nullptr, 2);
			profiler.Submitted(0);
		}
		CHECK_FALSE(profiler.BeginFrame(true, nullptr, Context(5), 0.1));
		CHECK(profiler.GetSkippedCount() == 1);
		CHECK(profiler.GetPendingCount() == RenderProfiler::SlotCount);
		// Timer availability is deliberately true, as with stale timestamps before the new GPU reset.
		CHECK(queries.TimerPolls == 0);
		CHECK(queries.Reads == 0);
		queries.CompleteAll();
		profiler.Poll(0.2);
		REQUIRE(profiler.GetLatest());
		CHECK(profiler.GetLatest()->Context.Frame == 4);
		CHECK(profiler.GetLatest()->Context.EffectivePath == 2);
		CHECK(profiler.GetLatest()->Context.InternalWidth == 97);
		CHECK(profiler.GetLatest()->Context.Settings == Context(4).Settings);
		CHECK(*profiler.GetLatest()->Milliseconds[0] == doctest::Approx(1.0));
		CHECK_FALSE(profiler.GetLatest()->Milliseconds[1]);
		CHECK(profiler.GetPendingCount() == 0);
		REQUIRE(profiler.BeginFrame(true, nullptr, Context(5), 0.3));
		profiler.EndFrame(nullptr, 1);
		profiler.Submitted(0.3);
		const auto reads = queries.Reads;
		CHECK_FALSE(profiler.BeginFrame(false, nullptr, Context(6), 0.4));
		CHECK(profiler.GetPendingCount() == 1);
		CHECK(queries.Reads == reads);
		CHECK(profiler.GetLatest()->Context.Frame == 4);
		queries.CompleteAll();
		CHECK_FALSE(profiler.BeginFrame(false, nullptr, Context(7), 0.5));
		CHECK(profiler.GetLatest()->Context.Frame == 5);
		CHECK(std::string(profiler.GetStatus()) == "disabled");
		REQUIRE(profiler.BeginFrame(true, nullptr, Context(8), 0.6));
		profiler.EndFrame(nullptr, 0);
		profiler.Submitted(0.6);
		queries.CompleteAll();
		profiler.Poll(0.7);
		CHECK(profiler.GetLatest()->Context.Frame == 8);
	}
	CHECK(queries.LiveTimers == 0);
}

TEST_CASE("RenderProfiler: allocation and capability failures recover without recording partial queries")
{
	FakeQueries queries;
	RenderProfiler unsupported(nullptr, 16, 1, &queries);
	CHECK_FALSE(unsupported.BeginFrame(true, nullptr, Context(1), 0));
	CHECK(queries.Allocations == 0);
	for (bool eventFailure : {false, true})
	{
		queries.FailAt = eventFailure ? 0 : queries.Allocations + 3;
		queries.FailEvent = eventFailure;
		RenderProfiler profiler(nullptr, 32, 1, &queries);
		CHECK_FALSE(profiler.BeginFrame(true, nullptr, Context(1), 0));
		CHECK(queries.LiveTimers == 0);
		CHECK(profiler.GetPendingCount() == 0);
		queries.FailAt = 0;
		queries.FailEvent = false;
		REQUIRE(profiler.BeginFrame(true, nullptr, Context(2), 0));
		profiler.EndFrame(nullptr, 0);
		profiler.Submitted(0);
		queries.Events.back()->Complete = true;
		profiler.Poll(0.1);
		REQUIRE(profiler.GetLatest());
	}
}

TEST_CASE("RenderProfiler: pending queries never expire into reuse and ambiguous or invalid results are rejected")
{
	FakeQueries queries;
	RenderProfiler profiler(nullptr, 32, 1, &queries);
	REQUIRE(profiler.BeginFrame(true, nullptr, Context(1), 0));
	profiler.BeginSection(nullptr, RenderTimingSection::AntiAliasing);
	profiler.EndSection(nullptr, RenderTimingSection::AntiAliasing);
	profiler.EndFrame(nullptr, 0, false, false, 2);
	profiler.Submitted(0);
	profiler.Poll(100);
	CHECK(profiler.GetPendingCount() == 1);
	CHECK(queries.Reads == 0);
	queries.CompleteAll();
	queries.TimersReady = false;
	profiler.Poll(100);
	CHECK(queries.Reads == 0);
	CHECK(profiler.GetPendingCount() == 1);
	queries.TimersReady = true;
	profiler.Poll(100);
	CHECK(profiler.GetPendingCount() == 0);
	CHECK_FALSE(profiler.GetLatest());
	CHECK(std::string(profiler.GetStatus()) == "invalid: timestamp interval");
	for (float invalid : {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		REQUIRE(profiler.BeginFrame(true, nullptr, Context(2), 100));
		profiler.BeginSection(nullptr, RenderTimingSection::AntiAliasing);
		profiler.EndSection(nullptr, RenderTimingSection::AntiAliasing);
		profiler.EndFrame(nullptr, 0, false, false, 2);
		profiler.Submitted(100);
		queries.Seconds = invalid;
		queries.CompleteAll();
		profiler.Poll(100.1);
		CHECK_FALSE(profiler.GetLatest());
	}
	CHECK(profiler.GetInvalidCount() == 4);
	queries.Seconds = 0.002f;
	REQUIRE(profiler.BeginFrame(true, nullptr, Context(3), 101));
	profiler.BeginSection(nullptr, RenderTimingSection::AntiAliasing);
	profiler.EndSection(nullptr, RenderTimingSection::AntiAliasing);
	profiler.EndFrame(nullptr, 0, false, false, 2);
	profiler.Submitted(101);
	queries.CompleteAll();
	profiler.Poll(101.1);
	REQUIRE(profiler.GetLatest());
	CHECK(profiler.GetLatest()->Context.Frame == 3);
	CHECK(profiler.GetLatest()->Context.EffectiveAA == 2);
	REQUIRE(profiler.GetLatest()->Milliseconds[static_cast<size_t>(RenderTimingSection::AntiAliasing)]);
	CHECK(*profiler.GetLatest()->Milliseconds[static_cast<size_t>(RenderTimingSection::AntiAliasing)] == doctest::Approx(2.0));
	CHECK(profiler.GetInvalidCount() == 4);
	CHECK(std::string(profiler.GetLastResultStatus()) == "ready");
	CHECK(profiler.GetPendingCount() == 0);
}

TEST_CASE("RenderProfiler: abandoned recordings retain query indices and invalid section order is verified")
{
	FakeQueries queries;
	{
		RenderProfiler profiler(nullptr, 64, 1, &queries);
		Testing::ScopedAssertionCapture capture;
		for (uint64_t frame = 1; frame <= RenderProfiler::SlotCount; ++frame)
		{
			REQUIRE(profiler.BeginFrame(true, nullptr, Context(frame), 0));
			CHECK_THROWS_AS(profiler.BeginFrame(true, nullptr, Context(frame), 0), Testing::AssertionFailedError);
			CHECK_THROWS_AS(profiler.EndSection(nullptr, RenderTimingSection::Shadows), Testing::AssertionFailedError);
			CHECK_THROWS_AS(profiler.BeginSection(nullptr, RenderTimingSection::Total), Testing::AssertionFailedError);
			CHECK_THROWS_AS(profiler.BeginSection(nullptr, RenderTimingSection::Count), Testing::AssertionFailedError);
			CHECK_THROWS_AS(profiler.EndSection(nullptr, RenderTimingSection::Count), Testing::AssertionFailedError);
			CHECK_THROWS_AS(profiler.Submitted(0), Testing::AssertionFailedError);
			profiler.AbandonFrame();
		}
		CHECK(queries.LiveTimers == int(RenderProfiler::SlotCount * RenderProfiler::SectionCount));
		CHECK_FALSE(profiler.BeginFrame(true, nullptr, Context(5), 1));
		CHECK(queries.Reads == 0);
	}
	CHECK(queries.LiveTimers == 0);
}

TEST_CASE("RenderProfiler: nested sections wait for every used timer and retain distinct durations")
{
	FakeQueries queries;
	RenderProfiler profiler(nullptr, 64, 1, &queries);
	REQUIRE(profiler.BeginFrame(true, nullptr, Context(1), 0));
	profiler.BeginSection(nullptr, RenderTimingSection::Scene);
	profiler.BeginSection(nullptr, RenderTimingSection::AmbientOcclusion);
	profiler.EndSection(nullptr, RenderTimingSection::AmbientOcclusion);
	profiler.EndSection(nullptr, RenderTimingSection::Scene);
	profiler.BeginSection(nullptr, RenderTimingSection::AntiAliasing);
	profiler.EndSection(nullptr, RenderTimingSection::AntiAliasing);
	profiler.EndFrame(nullptr, 2, true, false, 2);
	profiler.Submitted(0);
	const auto total = static_cast<size_t>(RenderTimingSection::Total);
	const auto scene = static_cast<size_t>(RenderTimingSection::Scene);
	const auto ao = static_cast<size_t>(RenderTimingSection::AmbientOcclusion);
	const auto aa = static_cast<size_t>(RenderTimingSection::AntiAliasing);
	queries.Timers[total]->Seconds = 0.015f;
	queries.Timers[scene]->Seconds = 0.010f;
	queries.Timers[ao]->Seconds = 0.003f;
	queries.Timers[aa]->Seconds = 0.002f;
	queries.Timers[scene]->Ready = false;
	queries.CompleteAll();
	profiler.Poll(0.1);
	CHECK_FALSE(profiler.GetLatest());
	CHECK(queries.Reads == 0);
	CHECK(queries.Resets == 0);
	queries.Timers[scene]->Ready = true;
	profiler.Poll(0.2);
	REQUIRE(profiler.GetLatest());
	CHECK(profiler.GetLatest()->Context.AmbientOcclusionActive);
	CHECK_FALSE(profiler.GetLatest()->Context.BloomActive);
	CHECK(profiler.GetLatest()->Context.EffectiveAA == 2);
	REQUIRE(profiler.GetLatest()->Milliseconds[total]);
	REQUIRE(profiler.GetLatest()->Milliseconds[scene]);
	REQUIRE(profiler.GetLatest()->Milliseconds[ao]);
	REQUIRE(profiler.GetLatest()->Milliseconds[aa]);
	CHECK(*profiler.GetLatest()->Milliseconds[total] == doctest::Approx(15.0));
	CHECK(*profiler.GetLatest()->Milliseconds[scene] == doctest::Approx(10.0));
	CHECK(*profiler.GetLatest()->Milliseconds[ao] == doctest::Approx(3.0));
	CHECK(*profiler.GetLatest()->Milliseconds[aa] == doctest::Approx(2.0));
	CHECK_FALSE(profiler.GetLatest()->Milliseconds[static_cast<size_t>(RenderTimingSection::Shadows)]);
	CHECK_FALSE(profiler.GetLatest()->Milliseconds[static_cast<size_t>(RenderTimingSection::Bloom)]);
	CHECK(queries.Reads == 4);
	CHECK(queries.Resets == 4);
}
