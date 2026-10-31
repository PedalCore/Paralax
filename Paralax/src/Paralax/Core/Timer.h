#pragma once

#include <chrono>

namespace Paralax {

	// Monotonic stopwatch.
	class Timer
	{
	public:
		Timer() { Reset(); }

		void Reset() { m_Start = Clock::now(); }

		float Elapsed() const { return std::chrono::duration<float>(Clock::now() - m_Start).count(); }
		float ElapsedMillis() const { return Elapsed() * 1000.0f; }

	private:
		using Clock = std::chrono::steady_clock;
		Clock::time_point m_Start;
	};

}
