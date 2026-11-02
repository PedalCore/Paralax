#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Paralax {

	// Text the engine writes or reads names paths in UTF-8: JSON documents (scenes, prefabs, materials, the project
	// file, the asset registry, the export manifest), command parameters and results, script strings and log
	// messages. ToUtf8 and FromUtf8 convert between that text and std::filesystem::path, and never throw.
	//
	// Where paths are UTF-16 (Windows) they transcode; where paths are bytes (POSIX) the bytes pass through
	// unchanged, so a file name there need not be UTF-8 at all. What keeps documents UTF-8 on every platform is that
	// only names whose UTF-8 form is well formed and converts back to the same path are accepted into them:
	// AssetManager::ToRegistryPath, the gate every project file path goes through, refuses any other name. Log
	// messages print names as ToUtf8 gives them, so on POSIX a name that is not UTF-8 is logged as its bytes.
	//
	// std::filesystem::path's own conversions are not used on such paths. On Windows path::string() and
	// generic_string() produce the ANSI code page and throw std::system_error for a character it cannot represent,
	// path(std::string) throws on bytes that are not a valid code-page sequence, and even the UTF-8 ones
	// (generic_u8string(), path(std::u8string)) throw on an unpaired surrogate or malformed UTF-8.
	//
	// Command-line arguments follow the same rule: the entry point reads them with GetProcessArguments
	// (Core/CommandLine.h), which on Windows gives UTF-8 rather than argv's ANSI code page (on POSIX, argv's bytes),
	// so a path option becomes a path through FromUtf8.
	//
	// Every function here is pure and safe to call from any thread.

	// The path in generic form (forward slashes) as UTF-8. Where the native encoding is UTF-16 (Windows) an
	// unpaired surrogate becomes U+FFFD; elsewhere the file system's bytes pass through unchanged, so the result is
	// only UTF-8 if the name is (check with IsWellFormedUtf8 before writing it into a document).
	std::string ToUtf8(const std::filesystem::path& path);

	// A path from UTF-8 text. Where the native encoding is UTF-16 (Windows) a malformed sequence becomes U+FFFD, so
	// hostile text never throws; elsewhere the bytes pass through unchanged, as the file system stores them.
	std::filesystem::path FromUtf8(std::string_view text);

	// True if text is well-formed UTF-8: only the byte sequences Unicode allows, so no overlong forms, encoded
	// surrogates, code points beyond U+10FFFF or truncated sequences.
	bool IsWellFormedUtf8(std::string_view text);

	// The transcoding ToUtf8/FromUtf8 use where the native path encoding is UTF-16, public so every platform tests
	// it. Malformed input (unpaired surrogates, truncated or overlong sequences, encoded surrogates, code points
	// beyond U+10FFFF) becomes U+FFFD, one per maximal invalid subsequence; the output is always well formed.
	std::string Utf16ToUtf8(std::u16string_view text);
	std::u16string Utf8ToUtf16(std::string_view text);

}
