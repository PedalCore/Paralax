#include "pxpch.h"
#include "Paralax/Core/UUID.h"

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

		std::mt19937_64 CreateEngine()
		{
			// std::random_device throws when the OS source is unavailable (getrandom and /dev/urandom denied by a
			// seccomp profile, say): an identifier generator is not the place to stop the process, so the weaker
			// seed is used and the condition is logged once.
			try
			{
				std::random_device device;
				std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
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

		uint64_t GenerateUUID()
		{
			// One engine per thread.
			thread_local std::mt19937_64 engine = CreateEngine();
			thread_local std::uniform_int_distribution<uint64_t> distribution(1, UINT64_MAX);

			return distribution(engine);
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

}
