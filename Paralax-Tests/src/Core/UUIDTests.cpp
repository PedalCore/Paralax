#include <doctest/doctest.h>

#include <Paralax/Core/UUID.h>

#include <thread>
#include <unordered_set>
#include <vector>

using namespace Paralax;

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
