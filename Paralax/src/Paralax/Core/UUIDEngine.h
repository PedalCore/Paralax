#pragma once

#include <cstdint>
#include <functional>
#include <random>

namespace Paralax {

	// The generator behind UUID(), seeded from eight calls to entropy. If entropy throws a std::exception (there is no
	// OS entropy source), the generator is seeded from UUID::FallbackSeed instead and that is logged once per process.
	// Never throws; thread-safe. Internal to UUID: declared here so tests can supply a failing source.
	std::mt19937_64 CreateUUIDEngine(const std::function<uint32_t()>& entropy);

}
