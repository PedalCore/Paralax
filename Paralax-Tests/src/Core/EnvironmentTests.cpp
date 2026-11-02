#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/Environment.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace Paralax;
using Paralax::Testing::ScopedEnvironmentVariable;

namespace {

	// "Wert-é-日本-Ω", spelled as UTF-8 bytes so the test does not depend on how the compiler reads the source. The
	// characters share no ANSI code page, so on Windows only the wide environment can hold them all.
	constexpr std::string_view NonAsciiValue = "Wert-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC-\xCE\xA9";

	// Windows, where paths and the environment are UTF-16, behaves differently for empty values.
	constexpr bool NativePathsAreBytes = sizeof(std::filesystem::path::value_type) == 1;

	std::optional<std::string> GetCRuntimeValue(const char* name)
	{
		const char* value = std::getenv(name);
		return value ? std::optional<std::string>(value) : std::nullopt;
	}

}

TEST_CASE("Environment: a variable that is set reads back, through std::getenv too, until it is removed")
{
	constexpr const char* Name = "PARALAX_TEST_ENVIRONMENT_SET";
	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
	CHECK_FALSE(GetProcessEnvironmentVariable(Name));

	// std::getenv reads the C runtime's copy of the environment, which on Windows is not the process's block: it must
	// see each change as well, or the engine (which reads its variables with std::getenv) would not.
	REQUIRE(SetProcessEnvironmentVariable(Name, "first value"));
	CHECK(GetProcessEnvironmentVariable(Name) == "first value");
	CHECK(GetCRuntimeValue(Name) == "first value");

	// Replacing it, with a value that holds the name-ending '=' (only names may not).
	REQUIRE(SetProcessEnvironmentVariable(Name, "a=b"));
	CHECK(GetProcessEnvironmentVariable(Name) == "a=b");
	CHECK(GetCRuntimeValue(Name) == "a=b");

	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
	CHECK_FALSE(GetProcessEnvironmentVariable(Name));
	CHECK_FALSE(GetCRuntimeValue(Name));
	// Removing a variable that is not set succeeds.
	CHECK(SetProcessEnvironmentVariable(Name, std::nullopt));
}

TEST_CASE("Environment: values with non-ASCII characters read back exactly")
{
	constexpr const char* Name = "PARALAX_TEST_ENVIRONMENT_NON_ASCII";
	REQUIRE(SetProcessEnvironmentVariable(Name, NonAsciiValue));
	CHECK(GetProcessEnvironmentVariable(Name) == NonAsciiValue);
	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
	CHECK_FALSE(GetProcessEnvironmentVariable(Name));
}

TEST_CASE("Environment: malformed UTF-8 is kept as bytes on POSIX and set with U+FFFD on Windows")
{
	constexpr const char* Name = "PARALAX_TEST_ENVIRONMENT_MALFORMED";
	// A lone continuation byte between ASCII characters: one maximal invalid subsequence.
	REQUIRE(SetProcessEnvironmentVariable(Name, "a\x80z"));
	CHECK(GetProcessEnvironmentVariable(Name) == (NativePathsAreBytes ? "a\x80z" : "a\xEF\xBF\xBDz"));
	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
}

TEST_CASE("Environment: values beyond the Windows C runtime's limit are refused there, never fatal")
{
	// The Windows C runtime takes names and values shorter than 32,767 UTF-16 units and ends the process for any other
	// (an invalid parameter), so those must be refused before they reach it. POSIX has no such limit.
	constexpr const char* Name = "PARALAX_TEST_ENVIRONMENT_LONG";
	const std::string longValue(4096, 'v');
	REQUIRE(SetProcessEnvironmentVariable(Name, longValue));
	CHECK(GetProcessEnvironmentVariable(Name) == longValue);

	// Refused on Windows, leaving the previous value.
	const std::string tooLong(32767, 'v');
	CHECK(SetProcessEnvironmentVariable(Name, tooLong) == NativePathsAreBytes);
	CHECK(GetProcessEnvironmentVariable(Name) == (NativePathsAreBytes ? tooLong : longValue));
	const std::string tooLongName(32767, 'N');
	CHECK(SetProcessEnvironmentVariable(tooLongName, "value") == NativePathsAreBytes);
	CHECK(SetProcessEnvironmentVariable(tooLongName, std::nullopt) == NativePathsAreBytes);
	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
}

TEST_CASE("Environment: an empty value is kept on POSIX and removes the variable on Windows")
{
	constexpr const char* Name = "PARALAX_TEST_ENVIRONMENT_EMPTY";
	REQUIRE(SetProcessEnvironmentVariable(Name, "not empty"));
	REQUIRE(SetProcessEnvironmentVariable(Name, ""));
	if (NativePathsAreBytes)
		CHECK(GetProcessEnvironmentVariable(Name) == "");
	else
		CHECK_FALSE(GetProcessEnvironmentVariable(Name));
	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
}

TEST_CASE("Environment: invalid names and values are refused and change nothing")
{
	CHECK(IsValidEnvironmentVariableName("PARALAX_TEST"));
	CHECK(IsValidEnvironmentVariableName("lower.case-name with spaces"));
	CHECK_FALSE(IsValidEnvironmentVariableName(""));
	CHECK_FALSE(IsValidEnvironmentVariableName("A=B"));
	CHECK_FALSE(IsValidEnvironmentVariableName("=C:"));
	CHECK_FALSE(IsValidEnvironmentVariableName(std::string_view("A\0B", 3)));

	// Two of the invalid names begin with a valid one, which must not be set or removed in their place.
	const std::string valid = "PARALAX_TEST_ENVIRONMENT_INVALID";
	REQUIRE(SetProcessEnvironmentVariable(valid, "kept"));
	for (const std::string& invalid : {std::string(), valid + "=other", valid + std::string(1, '\0') + "X"})
	{
		CAPTURE(invalid);
		CHECK_FALSE(SetProcessEnvironmentVariable(invalid, "replaced"));
		CHECK_FALSE(SetProcessEnvironmentVariable(invalid, std::nullopt));
		CHECK_FALSE(GetProcessEnvironmentVariable(invalid));
	}
	// A NUL would cut the value short.
	CHECK_FALSE(SetProcessEnvironmentVariable(valid, std::string_view("cut\0short", 9)));
	CHECK(GetProcessEnvironmentVariable(valid) == "kept");
	REQUIRE(SetProcessEnvironmentVariable(valid, std::nullopt));
}

TEST_CASE("Environment: ScopedEnvironmentVariable restores the previous value exactly, or removes what it set")
{
	constexpr const char* Name = "PARALAX_TEST_ENVIRONMENT_SCOPED";
	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
	{
		ScopedEnvironmentVariable created(Name, "created");
		CHECK(GetProcessEnvironmentVariable(Name) == "created");
	}
	CHECK_FALSE(GetProcessEnvironmentVariable(Name));

	// A previous value std::getenv could not represent on Windows (outside the ANSI code page) comes back unchanged.
	REQUIRE(SetProcessEnvironmentVariable(Name, NonAsciiValue));
	{
		ScopedEnvironmentVariable replaced(Name, "replaced");
		CHECK(GetProcessEnvironmentVariable(Name) == "replaced");
		{
			ScopedEnvironmentVariable removed(Name, std::nullopt);
			CHECK_FALSE(GetProcessEnvironmentVariable(Name));
		}
		CHECK(GetProcessEnvironmentVariable(Name) == "replaced");
	}
	CHECK(GetProcessEnvironmentVariable(Name) == NonAsciiValue);

	// Refused whole: not split at its '=' into a variable PARALAX_TEST holding "INVALID=value".
	CHECK_THROWS_AS(ScopedEnvironmentVariable("PARALAX_TEST=INVALID", "value"), std::runtime_error);
	CHECK_FALSE(GetProcessEnvironmentVariable("PARALAX_TEST"));
	CHECK(GetProcessEnvironmentVariable(Name) == NonAsciiValue);
	REQUIRE(SetProcessEnvironmentVariable(Name, std::nullopt));
}
