#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/JsonText.h>
#include <Paralax/Core/PathEncoding.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

using namespace Paralax;
using Paralax::Testing::DeeplyNestedJson;

TEST_CASE("JsonText: nesting at the limit passes and one level deeper fails")
{
	CHECK_FALSE(JsonExceedsDepth("", MaxJsonDepth));
	CHECK_FALSE(JsonExceedsDepth("42", 0));
	CHECK_FALSE(JsonExceedsDepth("[]", 1));
	CHECK_FALSE(JsonExceedsDepth(R"({"a": [1, 2], "b": {"c": null}})", 2));
	CHECK(JsonExceedsDepth("[[]]", 1));
	CHECK(JsonExceedsDepth(R"({"a": [[1]]})", 2));

	CHECK_FALSE(JsonExceedsDepth(DeeplyNestedJson(MaxJsonDepth), MaxJsonDepth));
	CHECK(JsonExceedsDepth(DeeplyNestedJson(MaxJsonDepth + 1), MaxJsonDepth));
	CHECK(JsonExceedsDepth(DeeplyNestedJson(20000), MaxJsonDepth));

	// Depth is nesting, not count: each sibling closes before the next opens.
	std::string wide = "[";
	for (int i = 0; i < 10000; i++)
		wide += "[[0]],";
	wide += "0]";
	CHECK_FALSE(JsonExceedsDepth(wide, 3));

	// Nesting counts wherever it sits in the document.
	CHECK(JsonExceedsDepth(R"({"Paralax": {"Type": "Scene"}, "Entities": )" + DeeplyNestedJson(MaxJsonDepth) + "}", MaxJsonDepth));
}

TEST_CASE("JsonText: brackets inside strings and escaped quotes do not count")
{
	CHECK_FALSE(JsonExceedsDepth(R"(["[[[[[[[[[[[[[[[[[[[[", "{{{{{{{{{{{{{{{{{{{{{"])", 1));
	// Keys are strings too.
	CHECK_FALSE(JsonExceedsDepth(R"({"[[[[[[[[[[[[[[[[": 1})", 1));
	// An escaped quote does not end the string, so the brackets after it are still text.
	CHECK_FALSE(JsonExceedsDepth(R"(["a\"[[[[[[[[[[[[[[[[[[[["])", 1));
	// An escaped backslash leaves the following quote unescaped: the string ends and real nesting follows.
	CHECK(JsonExceedsDepth(R"(["a\\", [[0]]])", 2));
	// Closing brackets inside strings do not lower the depth either.
	CHECK(JsonExceedsDepth(R"([["]]]]]]]]]]", [0]]])", 2));
	CHECK_FALSE(JsonExceedsDepth(R"([["]]]]]]]]]]", [0]]])", 3));
}

TEST_CASE("JsonText: truncated and malformed text is scanned without crashing")
{
	// A backslash as the last character has nothing to escape.
	CHECK_FALSE(JsonExceedsDepth(R"(["abc\)", 1));
	CHECK_FALSE(JsonExceedsDepth("\\", 1));
	// An unterminated string swallows the rest of the text.
	CHECK_FALSE(JsonExceedsDepth(R"(["abc[[[[[[[[)", 1));
	// Unbalanced opening brackets count; stray closing brackets neither crash nor bank depth for later nesting.
	CHECK_FALSE(JsonExceedsDepth("[[[", 3));
	CHECK(JsonExceedsDepth("[[[[", 3));
	CHECK_FALSE(JsonExceedsDepth("]]]]]]]]]]", 1));
	CHECK(JsonExceedsDepth("]]]]]]]]]][[", 1));
	CHECK_FALSE(JsonExceedsDepth("}}}}{", 1));

	// Every prefix of a hostile array flips to "too deep" exactly where the limit is crossed.
	const std::string arrays = std::string(20000, '[') + std::string(20000, ']');
	for (size_t length = 0; length <= 2 * static_cast<size_t>(MaxJsonDepth); length++)
	{
		CAPTURE(length);
		CHECK(JsonExceedsDepth(std::string_view(arrays).substr(0, length), MaxJsonDepth) == (length > static_cast<size_t>(MaxJsonDepth)));
	}

	// The same for a mixed document cut at arbitrary points, including inside a key and after the leaf.
	const std::string nested = DeeplyNestedJson(20000);
	const size_t lengths[] = {0, 1, 3, 6, 7, 192, 193, 1000, nested.size() - 1, nested.size()};
	for (size_t length : lengths)
	{
		const std::string_view prefix(nested.data(), length);
		const auto openers = std::count_if(prefix.begin(), prefix.end(), [](char c) { return c == '[' || c == '{'; });
		CAPTURE(length);
		CHECK(JsonExceedsDepth(prefix, MaxJsonDepth) == (openers > MaxJsonDepth));
	}
}

TEST_CASE("JsonText: a negative depth limit fails the precondition")
{
#if defined(PX_ENABLE_ASSERTS)
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(JsonExceedsDepth("[]", -1), Paralax::Testing::AssertionFailedError);
#else
	// Dist: the assert is compiled out, so a negative limit refuses the first bracket and still accepts scalars.
	CHECK(JsonExceedsDepth("[]", -1));
	CHECK_FALSE(JsonExceedsDepth("42", -1));
#endif
}

TEST_CASE("JsonText: FormatJsonDocument replaces invalid UTF-8 instead of throwing")
{
	const nlohmann::json document = {{"Name", std::string("Crate\xFF")}, {"Tag", std::string("\xC3")}};
	// What it guards against: nlohmann's strict default throws on these bytes.
	CHECK_THROWS_AS(static_cast<void>(document.dump(1, '\t')), nlohmann::json::type_error);

	std::string text;
	CHECK_NOTHROW(text = FormatJsonDocument(document));
	const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
	REQUIRE_FALSE(parsed.is_discarded());
	CHECK(parsed["Name"] == "Crate\xEF\xBF\xBD");
	CHECK(parsed["Tag"] == "\xEF\xBF\xBD");
}

TEST_CASE("JsonText: FormatJsonDocument writes valid documents as before, with UTF-8 unescaped")
{
	const nlohmann::json document = {{"Paralax", {{"Type", "AssetRegistry"}, {"Version", 1}}}, {"Path", "Textures/Brick-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.png"}};
	const std::string text = FormatJsonDocument(document);
	CHECK(text == document.dump(1, '\t') + "\n");
	CHECK(text.find("\"Path\": \"Textures/Brick-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.png\"") != std::string::npos);
	CHECK(nlohmann::json::parse(text) == document);
}

TEST_CASE("JsonText: DescribeJsonValue shows a short value whole, with its type")
{
	using json = nlohmann::json;
	CHECK(DescribeJsonValue(json()) == "null");
	CHECK(DescribeJsonValue(true) == "true (boolean)");
	CHECK(DescribeJsonValue(-3) == "-3 (number)");
	CHECK(DescribeJsonValue(UINT64_MAX) == "18446744073709551615 (number)");
	CHECK(DescribeJsonValue(10.0) == "10.0 (number)");
	CHECK(DescribeJsonValue(-DBL_MAX) == "-1.7976931348623157e+308 (number)");
	CHECK(DescribeJsonValue("wide") == "\"wide\" (string)");
	CHECK(DescribeJsonValue("") == "\"\" (string)");
	CHECK(DescribeJsonValue(json::array({1, "a", 3})) == "[1,\"a\",3] (array)");
	CHECK(DescribeJsonValue(json::array()) == "[] (array)");
	CHECK(DescribeJsonValue(json::object()) == "{} (object)");
	CHECK(DescribeJsonValue(json{{"a", 1}, {"b", json::array({true, nullptr, json::object()})}}) == "{\"a\":1,\"b\":[true,null,{}]} (object)");

	// Numbers JSON cannot hold, which a value built in code can: dump() would write them as null.
	CHECK(DescribeJsonValue(std::numeric_limits<double>::quiet_NaN()) == "NaN (number)");
	CHECK(DescribeJsonValue(std::numeric_limits<double>::infinity()) == "Infinity (number)");
	CHECK(DescribeJsonValue(-std::numeric_limits<double>::infinity()) == "-Infinity (number)");
	CHECK(DescribeJsonValue(json::array({std::numeric_limits<double>::quiet_NaN()})) == "[NaN] (array)");
	// Binary data (from CBOR or code) is described by its size alone.
	CHECK(DescribeJsonValue(json::binary({1, 2, 3})) == "binary data (3 bytes)");
	CHECK(DescribeJsonValue(json::binary({1})) == "binary data (1 byte)");
	CHECK(DescribeJsonValue(json::array({json::binary({1})})) == "[<binary>] (array)");

	// A preview of exactly MaxPreviewBytes is whole, so it is not cut.
	const std::string fits(MaxPreviewBytes - 2, 'a');
	CHECK(DescribeJsonValue(fits) == "\"" + fits + "\" (string)");
	const std::string overflows(MaxPreviewBytes - 1, 'a');
	CHECK(DescribeJsonValue(overflows) == "\"" + overflows + "... (string of 63 bytes)");
}

TEST_CASE("JsonText: a long string is described in a few bytes that give its full size")
{
	const std::string huge(16u << 20, 'x');
	const std::string description = DescribeJsonValue(huge);
	CHECK(description == "\"" + std::string(MaxPreviewBytes - 1, 'x') + "... (string of 16777216 bytes)");
	CHECK(description.size() < 128);

	bool cut = false;
	CHECK(PreviewText(huge, &cut) == std::string(MaxPreviewBytes, 'x') + "...");
	CHECK(cut);
	CHECK(PreviewText("Bogus", &cut) == "Bogus");
	CHECK_FALSE(cut);
	CHECK(PreviewText(std::string(MaxPreviewBytes, 'x')) == std::string(MaxPreviewBytes, 'x'));

	CHECK(QuoteText("Bogus") == "'Bogus'");
	CHECK(QuoteText("") == "''");
	CHECK(QuoteText(huge) == "'" + std::string(MaxPreviewBytes, 'x') + "...' (16777216 bytes)");
	CHECK(QuoteText(std::string(MaxPreviewBytes + 1, 'x')) == "'" + std::string(MaxPreviewBytes, 'x') + "...' (65 bytes)");
}

TEST_CASE("JsonText: a preview is cut between characters, never inside one")
{
	// Two-, three- and four-byte characters after every amount of ASCII padding, so the limit falls at every offset
	// inside a character.
	for (std::string_view character : {std::string_view("\xC3\xA9"), std::string_view("\xE6\x97\xA5"), std::string_view("\xF0\x9F\x98\x80")})
	{
		for (size_t padding = 0; padding < character.size(); padding++)
		{
			std::string text(padding, 'a');
			while (text.size() < 4 * MaxPreviewBytes)
				text += character;
			CAPTURE(character.size());
			CAPTURE(padding);

			bool cut = false;
			const std::string preview = PreviewText(text, &cut);
			CHECK(cut);
			CHECK(IsWellFormedUtf8(preview));
			REQUIRE(preview.ends_with("..."));
			const std::string shown = preview.substr(0, preview.size() - 3);
			// Whole characters of the text, as many as fit.
			CHECK(text.starts_with(shown));
			CHECK((shown.size() - padding) % character.size() == 0);
			CHECK(shown.size() <= MaxPreviewBytes);
			CHECK(shown.size() + character.size() > MaxPreviewBytes);

			// The same inside a JSON string, after its opening quote.
			const std::string description = DescribeJsonValue(text);
			CHECK(IsWellFormedUtf8(description));
			const std::string quoted = description.substr(1, description.find("...") - 1);
			CHECK(text.starts_with(quoted));
			CHECK((quoted.size() - padding) % character.size() == 0);
			CHECK(quoted.size() + 1 <= MaxPreviewBytes);
			CHECK(quoted.size() + 1 + character.size() > MaxPreviewBytes);
		}
	}
}

TEST_CASE("JsonText: bytes that are not UTF-8 are shown as U+FFFD, and describing them never throws")
{
	const std::string replacement = "\xEF\xBF\xBD";
	// A stray byte and a sequence cut short, as a script or other code can put into a json string.
	const nlohmann::json value = std::string("ab\xFF\xC3");
	// What it guards against: nlohmann's strict default throws on these bytes.
	CHECK_THROWS_AS(static_cast<void>(value.dump()), nlohmann::json::type_error);

	std::string description;
	CHECK_NOTHROW(description = DescribeJsonValue(value));
	CHECK(description == "\"ab" + replacement + replacement + "\" (string)");
	CHECK_NOTHROW(description = DescribeJsonValue(nlohmann::json{{std::string("k\xFF"), std::string("\xE6\x97")}}));
	CHECK(description == "{\"k" + replacement + "\":\"" + replacement + "\"} (object)");
	CHECK(PreviewText("\xFF") == replacement);
	CHECK(QuoteText("Fish\xFF") == "'Fish" + replacement + "'");

	// A replacement is three bytes, so a long run of bad bytes is cut after as many as fit, and stays well formed.
	const std::string bad(1000, '\xFF');
	const std::string preview = PreviewText(bad);
	CHECK(IsWellFormedUtf8(preview));
	std::string expected;
	for (size_t i = 0; i < MaxPreviewBytes / 3; i++)
		expected += replacement;
	CHECK(preview == expected + "...");
	CHECK(IsWellFormedUtf8(DescribeJsonValue(bad)));
}

TEST_CASE("JsonText: quotes, backslashes and control characters are escaped, and an escape is never split")
{
	CHECK(PreviewText("a\"b\\c\nd\te\r\b\f\x01\x1F\x7F") == "a\\\"b\\\\c\\nd\\te\\r\\b\\f\\u0001\\u001f\\u007f");
	CHECK(DescribeJsonValue("line\nbreak") == "\"line\\nbreak\" (string)");
	// C1 controls (U+0085 NEL, U+009B CSI) and the Unicode line and paragraph separators, as UTF-8; the characters
	// around them (U+00A0, U+2027, U+202A) are printable and stay as they are.
	CHECK(PreviewText("\xC2\x80\xC2\x85\xC2\x9B\xC2\x9F\xC2\xA0") == "\\u0080\\u0085\\u009b\\u009f\xC2\xA0");
	CHECK(PreviewText("\xE2\x80\xA7\xE2\x80\xA8\xE2\x80\xA9\xE2\x80\xAA") == "\xE2\x80\xA7\\u2028\\u2029\xE2\x80\xAA");
	CHECK(QuoteText("it's \"quoted\"") == "'it's \\\"quoted\\\"'");
	// Ten six-byte escapes fit in the preview; the eleventh is left out whole.
	std::string expected;
	for (int i = 0; i < 10; i++)
		expected += "\\u0001";
	CHECK(PreviewText(std::string(100, '\x01')) == expected + "...");
}

TEST_CASE("JsonText: DescribeJsonValue walks containers iteratively and stops once the preview is full")
{
	using json = nlohmann::json;
	// Values built in code are not depth-checked like documents read from files: dumping this one would recurse
	// 100,000 levels deep.
	json deep = json::array();
	json* innermost = &deep;
	for (int level = 0; level < 100000; level++)
		innermost = &innermost->emplace_back(json::array());
	std::string description;
	CHECK_NOTHROW(description = DescribeJsonValue(deep));
	CHECK(description == std::string(MaxPreviewBytes, '[') + "... (array of 1 element)");

	json wide = json::array();
	for (int i = 0; i < 10000; i++)
		wide.push_back(0);
	description = DescribeJsonValue(wide);
	CHECK(description.starts_with("[0,0,0,"));
	CHECK(description.ends_with("... (array of 10000 elements)"));
	CHECK(description.size() < 128);

	json members = json::object();
	for (int i = 0; i < 1000; i++)
		members["Key" + std::to_string(i)] = i;
	description = DescribeJsonValue(members);
	CHECK(description.starts_with("{\"Key0\":0,\"Key1\":1,"));
	CHECK(description.ends_with("... (object of 1000 members)"));
	CHECK(description.size() < 128);

	// A long string or key inside a container is cut like a string on its own.
	CHECK(DescribeJsonValue(json::array({std::string(1000, 'x')})) == "[\"" + std::string(MaxPreviewBytes - 2, 'x') + "... (array of 1 element)");
	CHECK(DescribeJsonValue(json{{std::string(1000, 'k'), 1}}) == "{\"" + std::string(MaxPreviewBytes - 2, 'k') + "... (object of 1 member)");
	// A token that does not fit is left out whole: no number is ever cut in half.
	json numbers = json::array();
	for (int i = 0; i < 100; i++)
		numbers.push_back(123456789);
	description = DescribeJsonValue(numbers);
	CHECK(description == "[123456789,123456789,123456789,123456789,123456789,123456789,... (array of 100 elements)");
}
