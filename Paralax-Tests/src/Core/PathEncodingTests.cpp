#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/PathEncoding.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Paralax;

namespace {

	// "Brick-é-日本.png", spelled as UTF-8 bytes so the test does not depend on how the compiler reads the source.
	constexpr std::string_view BrickName = "Brick-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.png";
	// U+1F600, outside the Basic Multilingual Plane: a surrogate pair where paths are UTF-16.
	constexpr std::string_view Emoji = "\xF0\x9F\x98\x80";
	constexpr std::string_view ReplacementUtf8 = "\xEF\xBF\xBD";
	constexpr char16_t Replacement = 0xFFFD;

	// POSIX paths are bytes; Windows paths are UTF-16.
	constexpr bool NativePathsAreBytes = sizeof(std::filesystem::path::value_type) == 1;

}

TEST_CASE("PathEncoding: ToUtf8 and FromUtf8 round-trip non-ASCII names")
{
	const std::string name(BrickName);
	const std::filesystem::path path = FromUtf8(name);
	CHECK(ToUtf8(path) == name);
	CHECK(path == std::filesystem::path(u8"Brick-é-日本.png"));
	CHECK(path.extension() == ".png");
	CHECK(ToUtf8(path.stem()) == "Brick-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC");

	const std::string nested = "Textures/" + name;
	CHECK(ToUtf8(FromUtf8(nested)) == nested);
	CHECK(ToUtf8(FromUtf8(nested).filename()) == name);
	CHECK(ToUtf8(FromUtf8(nested).parent_path()) == "Textures");

	const std::string sprite = "Sprites/" + std::string(Emoji) + ".png";
	CHECK(ToUtf8(FromUtf8(sprite)) == sprite);
	CHECK(FromUtf8(sprite) == std::filesystem::path(u8"Sprites/\U0001F600.png"));

	// For well-formed names the engine's conversion agrees with the standard library's (on Windows this compares
	// the engine's UTF-16 transcoding with the C++ runtime's).
	const std::u8string standard = FromUtf8(nested).generic_u8string();
	CHECK(std::string(standard.begin(), standard.end()) == nested);
}

TEST_CASE("PathEncoding: a non-ASCII file is created, listed and found by its UTF-8 name")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("PathEncodingFiles");
	std::ofstream(directory / FromUtf8(BrickName), std::ios::binary) << "brick";

	std::vector<std::string> names;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
		names.push_back(ToUtf8(entry.path().filename()));
	REQUIRE(names.size() == 1);
	CHECK(names.front() == BrickName);
	CHECK(Paralax::Testing::ReadFileText(FromUtf8(ToUtf8(directory) + "/" + std::string(BrickName))) == "brick");
}

TEST_CASE("PathEncoding: ToUtf8 writes the generic form, and ASCII is unchanged")
{
	CHECK(ToUtf8(std::filesystem::path("Scenes") / "Main.pxscene") == "Scenes/Main.pxscene");
	CHECK(ToUtf8(FromUtf8("a/b/c.png")) == "a/b/c.png");
	CHECK(ToUtf8(std::filesystem::path()).empty());
	CHECK(FromUtf8("").empty());
	// Files written before UTF-8 was guaranteed (all ASCII in practice) read and write back byte for byte.
	const std::string ascii = "Textures/Brick_01 (copy).png";
	CHECK(ToUtf8(FromUtf8(ascii)) == ascii);
	CHECK(FromUtf8(ascii) == std::filesystem::path(ascii));
}

TEST_CASE("PathEncoding: malformed UTF-8 never throws")
{
	// Lone and truncated bytes, an overlong form, an encoded surrogate, a code point past U+10FFFF, a stray
	// continuation byte.
	for (std::string_view text : {"\xFF", "a\xFE"
										  "b",
								  "\xC3", "\xE6\x97", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\x80"})
	{
		const std::string bytes(text);
		CAPTURE(bytes);
		std::filesystem::path path;
		CHECK_NOTHROW(path = FromUtf8(bytes));
		std::string back;
		CHECK_NOTHROW(back = ToUtf8(path));
		if constexpr (NativePathsAreBytes)
			CHECK(back == bytes); // The file system's bytes pass through.
		else
			CHECK(back.find(ReplacementUtf8) != std::string::npos);
	}
}

TEST_CASE("PathEncoding: malformed UTF-8 is replaced one maximal subpart at a time")
{
	CHECK(Utf8ToUtf16("").empty());
	CHECK(Utf8ToUtf16("a\xFF"
					  "b") == std::u16string{u'a', Replacement, u'b'});
	// A truncated sequence is one replacement, and the character after it is kept.
	CHECK(Utf8ToUtf16("\xE6\x97"
					  "A") == std::u16string{Replacement, u'A'});
	CHECK(Utf8ToUtf16("\xF0\x9F\x98"
					  "A") == std::u16string{Replacement, u'A'});
	CHECK(Utf8ToUtf16("\xF0\x9F\x98") == std::u16string{Replacement});
	// Overlong forms, encoded surrogates and code points past U+10FFFF are not well formed: every byte is replaced.
	CHECK(Utf8ToUtf16("\xC0\xAF") == std::u16string{Replacement, Replacement});
	CHECK(Utf8ToUtf16("\xE0\x80\xAF") == std::u16string{Replacement, Replacement, Replacement});
	CHECK(Utf8ToUtf16("\xED\xA0\x80") == std::u16string{Replacement, Replacement, Replacement});
	CHECK(Utf8ToUtf16("\xF4\x90\x80\x80") == std::u16string{Replacement, Replacement, Replacement, Replacement});
	CHECK(Utf8ToUtf16("\xF5\x80") == std::u16string{Replacement, Replacement});
	CHECK(Utf8ToUtf16("\x80\xBF") == std::u16string{Replacement, Replacement});
	// The mixed example of Unicode's Table 3-8: truncated sequences of each length, a lead byte followed by ASCII,
	// and stray continuation bytes.
	CHECK(Utf8ToUtf16("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64") ==
		  std::u16string{u'a', Replacement, Replacement, Replacement, u'b', Replacement, u'c', Replacement, Replacement, u'd'});
	// Just outside each class of lead byte's allowed second byte, so no prefix is valid.
	CHECK(Utf8ToUtf16("\xF0\x80\x80\x80") == std::u16string{Replacement, Replacement, Replacement, Replacement});
	CHECK(Utf8ToUtf16("\xC1\xBF") == std::u16string{Replacement, Replacement});
	CHECK(Utf8ToUtf16("\xE0\x9F\xBF") == std::u16string{Replacement, Replacement, Replacement});
	// An encoded surrogate cut short at the end of the input.
	CHECK(Utf8ToUtf16("\xED\xA0") == std::u16string{Replacement, Replacement});
	// The boundaries of the ranges that are well formed.
	CHECK(Utf8ToUtf16("\xED\x9F\xBF") == std::u16string{0xD7FF});
	CHECK(Utf8ToUtf16("\xEE\x80\x80") == std::u16string{0xE000});
	CHECK(Utf8ToUtf16("\xF4\x8F\xBF\xBF") == std::u16string{0xDBFF, 0xDFFF});
	CHECK(Utf8ToUtf16(Emoji) == std::u16string{0xD83D, 0xDE00});
}

TEST_CASE("PathEncoding: IsWellFormedUtf8 accepts exactly the sequences Unicode allows")
{
	CHECK(IsWellFormedUtf8(""));
	CHECK(IsWellFormedUtf8("Textures/Brick_01.png"));
	CHECK(IsWellFormedUtf8(BrickName));
	CHECK(IsWellFormedUtf8(Emoji));
	CHECK(IsWellFormedUtf8(ReplacementUtf8)); // A real U+FFFD is a character like any other.
	CHECK(IsWellFormedUtf8("\xED\x9F\xBF"
						   "\xEE\x80\x80"
						   "\xF4\x8F\xBF\xBF")); // The edges of the allowed ranges.
	for (std::string_view text : {"\xFF", "Brick\xFF.png", "Niveau-\xE9.pxscene", "\xC3", "a\xE6\x97", "\xC0\xAF", "\xC1\xBF", "\xE0\x9F\xBF",
								  "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\x80"})
	{
		const std::string bytes(text);
		CAPTURE(bytes);
		CHECK_FALSE(IsWellFormedUtf8(text));
	}
}

TEST_CASE("PathEncoding: unpaired UTF-16 surrogates become U+FFFD")
{
	const std::string replacement(ReplacementUtf8);
	CHECK(Utf16ToUtf8(std::u16string{0xD800}) == replacement);
	CHECK(Utf16ToUtf8(std::u16string{0xDC00, u'x'}) == replacement + "x");
	CHECK(Utf16ToUtf8(std::u16string{u'a', 0xD83D}) == "a" + replacement);
	// A high surrogate followed by a valid pair: only the first is unpaired.
	CHECK(Utf16ToUtf8(std::u16string{0xD800, 0xD800, 0xDC00}) == replacement + "\xF0\x90\x80\x80");
	CHECK(Utf16ToUtf8(std::u16string{0xD83D, 0xDE00}) == Emoji);
}

TEST_CASE("PathEncoding: UTF-8 and UTF-16 transcoding round-trips every code point")
{
	std::u16string all;
	for (char32_t code = 1; code <= 0x10FFFF; code++)
	{
		if (code >= 0xD800 && code <= 0xDFFF)
			continue;
		if (code < 0x10000)
		{
			all += static_cast<char16_t>(code);
		}
		else
		{
			all += static_cast<char16_t>(0xD800 + ((code - 0x10000) >> 10));
			all += static_cast<char16_t>(0xDC00 + ((code - 0x10000) & 0x3FF));
		}
	}
	const std::string utf8 = Utf16ToUtf8(all);
	CHECK(IsWellFormedUtf8(utf8));
	CHECK(Utf8ToUtf16(utf8) == all);
	// An independent reference: the standard library's own UTF-16 to UTF-8 conversion.
	const std::u8string reference = std::filesystem::path(all).u8string();
	CHECK(std::string(reference.begin(), reference.end()) == utf8);
}
