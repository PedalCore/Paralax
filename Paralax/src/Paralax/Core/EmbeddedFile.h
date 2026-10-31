#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Paralax {

	// A file compiled into the binary by cmake/EmbedFiles.cmake (shaders, fonts, editor resources).
	struct EmbeddedFile
	{
		const char* Name;
		const uint8_t* Data;
		size_t Size;
	};

	// Looks up an embedded file by name in a table; returns an empty span if absent.
	inline std::span<const uint8_t> FindEmbeddedFile(const EmbeddedFile* entries, size_t count, std::string_view name)
	{
		for (size_t i = 0; i < count; i++)
		{
			if (name == entries[i].Name)
				return {entries[i].Data, entries[i].Size};
		}
		return {};
	}

}
