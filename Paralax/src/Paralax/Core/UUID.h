#pragma once

#include <cstdint>
#include <functional>

namespace Paralax {

	// 64-bit randomly generated identifier, used to identify entities and assets persistently.
	// The value 0 is reserved as "invalid" and is never generated. Generation is thread-safe.
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

	private:
		uint64_t m_UUID;
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
