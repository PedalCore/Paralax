#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/EngineResources.h>

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
