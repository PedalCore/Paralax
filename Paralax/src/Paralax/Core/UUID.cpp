#include "pxpch.h"
#include "Paralax/Core/UUID.h"

#include <random>

namespace Paralax {

	namespace {

		uint64_t GenerateUUID()
		{
			// One engine per thread, seeded from the OS entropy source.
			thread_local std::mt19937_64 engine{[] {
				std::random_device device;
				std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
				return std::mt19937_64(seed);
			}()};
			thread_local std::uniform_int_distribution<uint64_t> distribution(1, UINT64_MAX);

			return distribution(engine);
		}

	}

	UUID::UUID()
		: m_UUID(GenerateUUID())
	{
	}

}
