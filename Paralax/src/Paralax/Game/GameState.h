#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace Paralax {

	// Values a game keeps across scene switches (a score, the level reached), owned by its GameSession and reached
	// from scripts through the Game table. Holds booleans, numbers and strings; sizes are capped because scripts are
	// untrusted. Main-thread-only.
	class GameState
	{
	public:
		// Integers stay integers (a level number reads back as 3, not 3.0).
		using Value = std::variant<bool, int64_t, double, std::string>;

		static constexpr size_t MaxKeys = 1024;
		static constexpr size_t MaxKeyBytes = 256;
		static constexpr size_t MaxStringBytes = 64 * 1024;
		// All keys and strings together: the store lives outside the scripts' memory limit.
		static constexpr size_t MaxTotalBytes = 1024 * 1024;

		// Returns false (changing nothing) for an empty or over-long key, an over-long string, a non-finite number, a
		// new key beyond MaxKeys, or a value that would take the store beyond MaxTotalBytes.
		bool Set(std::string_view key, Value value);
		std::optional<Value> Get(std::string_view key) const;
		bool Remove(std::string_view key);
		void Clear()
		{
			m_Values.clear();
			m_Bytes = 0;
		}
		size_t GetBytes() const { return m_Bytes; }
		size_t GetSize() const { return m_Values.size(); }

	private:
		static size_t SizeOf(const std::string& key, const Value& value);

		std::map<std::string, Value, std::less<>> m_Values;
		size_t m_Bytes = 0;
	};

}
