#include <doctest/doctest.h>

#include <Paralax/Core/Timer.h>
#include <Paralax/Core/Timestep.h>

#include <chrono>
#include <thread>

using namespace Paralax;

TEST_CASE("Timestep: converts between seconds and milliseconds")
{
	constexpr Timestep ts(0.25f);
	static_assert(ts.GetSeconds() == 0.25f);
	static_assert(ts.GetMilliseconds() == 250.0f);
	CHECK(static_cast<float>(ts) == 0.25f);
	CHECK(Timestep().GetSeconds() == 0.0f);
}

TEST_CASE("Timer: measures at least the slept duration and is monotonic")
{
	// Only lower bounds are checked: loaded CI machines (sanitizers, shared runners) may stall arbitrarily.
	Timer timer;
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const float first = timer.ElapsedMillis();
	CHECK(first >= 19.0f);
	CHECK(timer.ElapsedMillis() >= first);
	CHECK(timer.Elapsed() * 1000.0f >= first);
}

TEST_CASE("Timer: Reset restarts measurement")
{
	Timer timer;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	const float beforeReset = timer.ElapsedMillis();
	timer.Reset();
	const float afterReset = timer.ElapsedMillis();
	// Reading right after Reset is a near-zero interval; any stall shorter than the 100 ms sleep keeps this true.
	CHECK(afterReset <= beforeReset);
	CHECK(afterReset >= 0.0f);
}
