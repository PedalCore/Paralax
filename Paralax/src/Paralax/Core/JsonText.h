#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace Paralax {

	// A JSON document as the engine writes it to a file: tab-indented, with a trailing newline. nlohmann's dump()
	// throws on a string that is not valid UTF-8 (an entity name set from a script, say); here such bytes become
	// U+FFFD instead, so saving never throws and the file always loads. Paths should never need that: they are
	// written with ToUtf8, and only names that are well-formed UTF-8 are accepted into documents (see
	// PathEncoding.h), so a path is never altered on the way to the file. Pure; safe to call from any thread.
	std::string FormatJsonDocument(const nlohmann::json& document);

	// The deepest array/object nesting accepted in any JSON document the engine reads from a file or the network.
	// Engine documents (scenes, prefabs, materials, projects, registries, agent requests) nest a handful of levels;
	// anything near this limit is hostile.
	inline constexpr int MaxJsonDepth = 64;

	// The most bytes a text document the engine reads whole (scene, prefab, material, project, asset registry) may
	// have, and so the most its writers write. Real documents are a few megabytes at most; anything near this is
	// hostile or broken, and the limit stops a loader from allocating an arbitrary amount of memory before it can
	// refuse the file.
	inline constexpr size_t MaxTextFileBytes = 64ull * 1024 * 1024;

	// Returns true if the JSON text nests arrays or objects deeper than maxDepth. nlohmann parses iteratively, but
	// everything done with the value afterwards (copying, dumping, comparing, walking it) recurses once per level,
	// so hostile nesting overflows the stack unless it is refused on the raw text, before it is parsed. Brackets
	// inside strings do not count, and escaped characters (including escaped quotes) are skipped. Malformed or
	// truncated text is not an error here (the parser reports that); unmatched closing brackets never reduce the
	// depth below zero, so they cannot hide nesting that follows. One O(n) pass over the text; never throws.
	// Pure, so it is safe to call from any thread (the agent server calls it on its connection threads).
	bool JsonExceedsDepth(std::string_view text, int maxDepth);

	// Messages that echo a refused value (an error from SetField, a warning from a loader) show at most this many
	// bytes of it. The value can be megabytes (a 16 MiB string sent to a number field), and the message is logged,
	// returned to agents and kept in lists such as the script errors.
	inline constexpr size_t MaxPreviewBytes = 64;

	// Untrusted text (a name or key from a file, an agent or a script) as a message may show it. Characters are
	// escaped as inside a JSON string: double quotes, backslashes, and as \uXXXX every control character (C0, DEL and
	// C1), U+2028/U+2029 and the invisible format characters (U+200B-U+200F, U+202A-U+202E, U+2060-U+2069, U+FEFF),
	// so the text can neither break a log line, start a terminal escape sequence nor reorder how a line displays.
	// Every malformed UTF-8 sequence becomes U+FFFD (see DecodeUtf8), so the result is always well-formed UTF-8. The
	// preview is cut between characters before it would exceed MaxPreviewBytes, and then ends in "..."; outCut, if
	// given, says whether it was cut. Never throws, and reads only as much of the text as it shows. Pure; safe to
	// call from any thread.
	std::string PreviewText(std::string_view text, bool* outCut = nullptr);

	// PreviewText in single quotes, then the text's full length if the preview was cut: 'Bogus', or
	// 'aaaa...' (16777216 bytes). For echoing a refused name or key. Pure; safe to call from any thread.
	std::string QuoteText(std::string_view text);

	// Describes a JSON value for a message (a refused field value, say) without echoing it whole: a compact preview
	// of the value as JSON, built as PreviewText builds its text, and the value's JSON type, as in "wide" (string),
	// 1.5 (number) or [1,2] (array); null is just null. A preview cut short ends in "..." and the type then gives
	// the value's full size: "aaaa... (string of 16777216 bytes), or [0,0,... (array of 10000 elements). Numbers
	// JSON cannot hold, which a value built in code can, show as NaN, Infinity and -Infinity, and binary data only
	// by its size: binary data (3 bytes) on its own, [<binary 3 bytes>] (array) inside a container, where the token
	// is left out whole if it does not fit. The walk is iterative and stops as soon as the preview is full, so
	// neither the value's size nor its depth matters (depth is capped where documents are read, but not for values
	// built in code). Never throws. Pure; safe to call from any thread, provided no other thread modifies the value.
	std::string DescribeJsonValue(const nlohmann::json& value);

}
