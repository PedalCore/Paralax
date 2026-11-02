#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Paralax {

	// Paths are UTF-8 in every text the engine writes or reads: JSON documents (scenes, prefabs, materials, the
	// project file, the asset registry, the export manifest), log messages, command parameters and results, and
	// script strings. That holds on every platform, so a project moves between them unchanged.
	//
	// std::filesystem::path's own narrow conversions do not give that: on Windows, path::string() and
	// generic_string() produce the ANSI code page and throw std::system_error for a character it cannot represent,
	// and path(std::string) throws on bytes that are not a valid code-page sequence. Engine code therefore never
	// calls them on a path that came from the user, a file or a document; it uses the two functions below, which
	// never throw. Command-line arguments are the exception: they arrive in the native narrow encoding, so they
	// become paths through path(std::string) and are converted with ToUtf8 from there.
	//
	// Every function here is pure and safe to call from any thread.

	// The path in generic form (forward slashes) as UTF-8. Where the native encoding is UTF-16 (Windows) an
	// unpaired surrogate becomes U+FFFD; elsewhere the file system's bytes pass through unchanged.
	std::string ToUtf8(const std::filesystem::path& path);

	// A path from UTF-8 text. Where the native encoding is UTF-16 (Windows) a malformed sequence becomes U+FFFD, so
	// hostile text never throws; elsewhere the bytes pass through unchanged, as the file system stores them.
	std::filesystem::path FromUtf8(std::string_view text);

	// The transcoding ToUtf8/FromUtf8 use where the native path encoding is UTF-16, public so every platform tests
	// it. Malformed input (unpaired surrogates, truncated or overlong sequences, encoded surrogates, code points
	// beyond U+10FFFF) becomes U+FFFD, one per maximal invalid subsequence; the output is always well formed.
	std::string Utf16ToUtf8(std::u16string_view text);
	std::u16string Utf8ToUtf16(std::string_view text);

}
