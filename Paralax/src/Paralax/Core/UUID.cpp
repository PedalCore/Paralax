#include "pxpch.h"
#include "Paralax/Core/UUID.h"

#include "Paralax/Core/UUIDEngine.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <random>
#include <thread>

namespace Paralax {

	namespace {

		// splitmix64's finaliser: spreads every input bit over the whole output, so near-equal inputs (consecutive
		// clock readings, nearby addresses) give unrelated seeds.
		uint64_t Mix(uint64_t value)
		{
			value ^= value >> 30;
			value *= 0xBF58476D1CE4E5B9ull;
			value ^= value >> 27;
			value *= 0x94D049BB133111EBull;
			value ^= value >> 31;
			return value;
		}

		uint64_t GenerateUUID()
		{
			// One engine per thread, seeded from the OS entropy source. The device is created inside the entropy call,
			// so a constructor that throws takes the same fallback as a read that does.
			thread_local std::mt19937_64 engine = [] {
				Scope<std::random_device> device;
				return CreateUUIDEngine([&device]() -> uint32_t {
					if (!device)
						device = CreateScope<std::random_device>();
					return (*device)();
				});
			}();
			thread_local std::uniform_int_distribution<uint64_t> distribution(1, UINT64_MAX);

			return distribution(engine);
		}

	}

	std::mt19937_64 CreateUUIDEngine(const std::function<uint32_t()>& entropy)
	{
		// std::random_device throws when the OS source is unavailable (getrandom and /dev/urandom denied by a seccomp
		// profile, say): an identifier generator is not the place to stop the process, so the weaker seed is used and
		// the condition is logged once.
		try
		{
			std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
			return std::mt19937_64(seed);
		}
		catch (const std::exception& e)
		{
			static std::atomic<bool> s_Reported = false;
			if (!s_Reported.exchange(true))
				PX_CORE_WARN("UUID: no OS entropy source ({}); identifiers are seeded from the clock instead", e.what());
			return std::mt19937_64(UUID::FallbackSeed());
		}
	}

	UUID::UUID()
		: m_UUID(GenerateUUID())
	{
	}

	uint64_t UUID::FallbackSeed()
	{
		static std::atomic<uint64_t> s_Counter = 0;
		const uint64_t counter = s_Counter.fetch_add(1) + 1;
		uint64_t seed = Mix(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
		seed = Mix(seed ^ static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
		seed = Mix(seed ^ reinterpret_cast<uintptr_t>(&counter));
		seed = Mix(seed ^ std::hash<std::thread::id>()(std::this_thread::get_id()));
		return Mix(seed ^ counter);
	}

	UUID UUIDSequence::Next()
	{
		// The finaliser is a bijection, so over the counter's 2^64 states every value comes out once; the one state
		// that maps to 0 (UUID 0 means "none") is stepped over.
		for (;;)
		{
			m_State += 0x9E3779B97F4A7C15ull;
			if (const uint64_t value = Mix(m_State); value != 0)
				return UUID(value);
		}
	}

}
