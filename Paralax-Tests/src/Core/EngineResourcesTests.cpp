#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/EngineResources.h>
#include <Paralax/Core/PathEncoding.h>

using namespace Paralax;

TEST_CASE("EngineResources: the executable path names this test binary")
{
	const std::filesystem::path executable = GetExecutablePath();
	REQUIRE_FALSE(executable.empty());
	CHECK(executable.is_absolute());
	CHECK(std::filesystem::exists(executable));
	CHECK(executable.stem().string().find("Paralax-Tests") != std::string::npos);
}

TEST_CASE("EngineResources: resources live beside the executable unless overridden")
{
	{
		Paralax::Testing::ScopedEnvironmentVariable unset("PARALAX_RESOURCE_DIR", std::nullopt);
		CHECK(GetEngineResourceDirectory() == GetExecutablePath().parent_path() / "Resources");
	}
	{
		Paralax::Testing::ScopedEnvironmentVariable overridden("PARALAX_RESOURCE_DIR", "/custom/resources");
		CHECK(GetEngineResourceDirectory() == std::filesystem::path("/custom/resources"));
	}
	{
		// An empty value is treated as unset.
		Paralax::Testing::ScopedEnvironmentVariable empty("PARALAX_RESOURCE_DIR", "");
		CHECK(GetEngineResourceDirectory() == GetExecutablePath().parent_path() / "Resources");
	}
}

TEST_CASE("EngineResources: an override outside the ANSI code page names its directory exactly")
{
	// "/Ressourcen-é-日本-Ω" as UTF-8 bytes, in the engine's encoding. No ANSI code page holds all of these characters, so
	// on Windows std::getenv could not have returned them.
	const std::string directory = "/Ressourcen-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC-\xCE\xA9";
	Paralax::Testing::ScopedEnvironmentVariable overridden("PARALAX_RESOURCE_DIR", directory);
	CHECK(GetEngineResourceDirectory() == FromUtf8(directory));
	CHECK(ToUtf8(GetEngineResourceDirectory()) == directory);
}
