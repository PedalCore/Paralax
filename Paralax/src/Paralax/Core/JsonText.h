#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

namespace Paralax {

	// A JSON document as the engine writes it to a file: tab-indented, with a trailing newline. nlohmann's dump()
	// throws on a string that is not valid UTF-8 (an entity name set from a script, say); here such bytes become
	// U+FFFD instead, so saving never throws and the file always loads. Paths never need that: they are written
	// with ToUtf8 (PathEncoding.h), which is UTF-8 on every platform.
	std::string FormatJsonDocument(const nlohmann::json& document);

	// The deepest array/object nesting accepted in any JSON document the engine reads from a file or the network.
	// Engine documents (scenes, prefabs, materials, projects, registries, agent requests) nest a handful of levels;
	// anything near this limit is hostile.
	inline constexpr int MaxJsonDepth = 64;

	// Returns true if the JSON text nests arrays or objects deeper than maxDepth. nlohmann parses iteratively, but
	// everything done with the value afterwards (copying, dumping, comparing, walking it) recurses once per level,
	// so hostile nesting overflows the stack unless it is refused on the raw text, before it is parsed. Brackets
	// inside strings do not count, and escaped characters (including escaped quotes) are skipped. Malformed or
	// truncated text is not an error here (the parser reports that); unmatched closing brackets never reduce the
	// depth below zero, so they cannot hide nesting that follows. One O(n) pass over the text; never throws.
	// Pure, so it is safe to call from any thread (the agent server calls it on its connection threads).
	bool JsonExceedsDepth(std::string_view text, int maxDepth);

}
