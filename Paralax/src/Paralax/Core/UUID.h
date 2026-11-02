#pragma once

#include <cstdint>
#include <functional>

namespace Paralax {

	// 64-bit randomly generated identifier, used to identify entities and assets persistently.
	// The value 0 is reserved as "invalid" and is never generated. Generation is thread-safe. The generator is
	// seeded from the OS entropy source; where there is none (std::random_device throws, e.g. in a hardened
	// container) it falls back to FallbackSeed and logs that once, rather than failing.
	class UUID
	{
	public:
		UUID();
		constexpr UUID(uint64_t uuid)
			: m_UUID(uuid)
		{
		}

		constexpr bool IsValid() const { return m_UUID != 0; }
		constexpr operator uint64_t() const { return m_UUID; }

		// The seed used without an OS entropy source: the clocks, a stack address, the thread and a counter, mixed.
		// Weaker than the OS source, but different on every call. Thread-safe. Public so tests can check it.
		static uint64_t FallbackSeed();

	private:
		uint64_t m_UUID;
	};

	// A reproducible sequence of UUIDs: splitmix64 (a counter stepped by an odd constant, then mixed), so a seed gives
	// the same values in the same order on every platform and build. Next never returns 0 and repeats no value within
	// 2^64 - 1 calls (every nonzero value once). A plain value: a copy continues the sequence independently. Not
	// thread-safe (it has no shared state; guard an instance like the object that holds it). Ids of entities created
	// while a game runs come from one (see Scene::SetEntityIdSequence).
	class UUIDSequence
	{
	public:
		explicit constexpr UUIDSequence(uint64_t seed)
			: m_State(seed)
		{
		}

		UUID Next();

	private:
		uint64_t m_State;
	};

}

template<>
struct std::hash<Paralax::UUID>
{
	std::size_t operator()(const Paralax::UUID& uuid) const noexcept
	{
		return std::hash<uint64_t>()(static_cast<uint64_t>(uuid));
	}
};
