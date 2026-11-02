#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/JsonText.h>

#include <algorithm>
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
