#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/UUID.h>
#include <Paralax/Core/UUIDEngine.h>

#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace Paralax;

TEST_CASE("UUID: without an OS entropy source the generator seeds itself, warning once")
{
	Paralax::Testing::ScopedLogCapture log;
	// What std::random_device does where getrandom and /dev/urandom are denied.
	const auto failing = []() -> uint32_t { throw std::runtime_error("entropy source denied"); };
	std::mt19937_64 first = CreateUUIDEngine(failing);
	std::mt19937_64 second = CreateUUIDEngine(failing);
	CHECK(first() != second()); // Each from its own fallback seed.
	size_t warnings = 0;
	for (const auto& entry : log.GetEntries())
		warnings += entry.Level == spdlog::level::warn && entry.Message.find("entropy source denied") != std::string::npos ? 1 : 0;
	CHECK(warnings == 1);

	// A working source is what seeds the generator.
	const auto constant = []() -> uint32_t { return 42; };
	CHECK(CreateUUIDEngine(constant)() == CreateUUIDEngine(constant)());
}

TEST_CASE("UUID: generated values are valid and unique")
{
	std::unordered_set<UUID> seen;
	for (int i = 0; i < 100000; i++)
	{
		UUID id;
		REQUIRE(id.IsValid());
		REQUIRE(seen.insert(id).second);
	}
}

TEST_CASE("UUID: explicit values round-trip and zero is invalid")
{
	constexpr UUID id(0x1234'5678'9ABC'DEF0ull);
	static_assert(static_cast<uint64_t>(id) == 0x1234'5678'9ABC'DEF0ull);
	CHECK(id.IsValid());
	CHECK_FALSE(UUID(0).IsValid());
}

TEST_CASE("UUID: hashing matches the hash of the underlying value")
{
	const UUID id(42);
	CHECK(std::hash<UUID>()(id) == std::hash<uint64_t>()(42));
}

TEST_CASE("UUID: the fallback seed differs between calls and threads and seeds a working generator")
{
	// The seed stands in for std::random_device where the OS has no entropy to give: it must at least never repeat
	// between calls, so two processes or threads started together do not generate the same identifiers.
	std::unordered_set<uint64_t> seeds;
	for (int i = 0; i < 1000; i++)
		CHECK(seeds.insert(UUID::FallbackSeed()).second);

	std::vector<uint64_t> fromThreads(4);
	std::vector<std::thread> threads;
	for (size_t t = 0; t < fromThreads.size(); t++)
		threads.emplace_back([&fromThreads, t] { fromThreads[t] = UUID::FallbackSeed(); });
	for (auto& thread : threads)
		thread.join();
	for (uint64_t seed : fromThreads)
		CHECK(seeds.insert(seed).second);

	// Generators seeded one after the other (two threads starting together) produce different identifiers.
	std::mt19937_64 first(UUID::FallbackSeed());
	std::mt19937_64 second(UUID::FallbackSeed());
	CHECK(first() != second());
}

TEST_CASE("UUID: generation is thread-safe and unique across threads")
{
	constexpr int threadCount = 4;
	constexpr int perThread = 10000;
	std::vector<std::vector<uint64_t>> results(threadCount);

	std::vector<std::thread> threads;
	for (int t = 0; t < threadCount; t++)
	{
		threads.emplace_back([&results, t] {
			results[static_cast<size_t>(t)].reserve(perThread);
			for (int i = 0; i < perThread; i++)
				results[static_cast<size_t>(t)].push_back(UUID()); });
	}
	for (auto& thread : threads)
		thread.join();

	std::unordered_set<uint64_t> all;
	for (const auto& values : results)
		for (uint64_t value : values)
			all.insert(value);

	CHECK(all.size() == static_cast<size_t>(threadCount * perThread));
}

TEST_CASE("UUIDSequence: a seed gives the same splitmix64 values on every platform, and copies continue independently")
{
	// The published splitmix64 outputs for seed 0: ids that differ between platforms would break replays.
	UUIDSequence zero(0);
	CHECK(static_cast<uint64_t>(zero.Next()) == 0xE220A8397B1DCDAFull);
	CHECK(static_cast<uint64_t>(zero.Next()) == 0x6E789E6AA1B965F4ull);
	CHECK(static_cast<uint64_t>(zero.Next()) == 0x06C45D188009454Full);

	UUIDSequence original(0x1D5EED);
	UUIDSequence copy = original;
	std::vector<UUID> fromOriginal;
	std::vector<UUID> fromCopy;
	for (int i = 0; i < 1000; i++)
		fromOriginal.push_back(original.Next());
	for (int i = 0; i < 1000; i++)
		fromCopy.push_back(copy.Next());
	CHECK(fromOriginal == fromCopy);
	CHECK(std::unordered_set<UUID>(fromOriginal.begin(), fromOriginal.end()).size() == fromOriginal.size());
	CHECK(UUIDSequence(1).Next() != UUIDSequence(2).Next());
}

TEST_CASE("UUIDSequence: the one state that would give 0 is stepped over")
{
	// The counter is stepped before mixing, so this seed reaches state 0 first, which the finaliser maps to 0.
	UUIDSequence sequence(0ull - 0x9E3779B97F4A7C15ull);
	const UUID first = sequence.Next();
	CHECK(first.IsValid());
	CHECK(first == UUIDSequence(0).Next()); // The value after the skipped one.
}
