#include <doctest/doctest.h>

#include <Paralax/Core/CommandLine.h>

using namespace Paralax;

TEST_CASE("CommandLine: parses argc/argv, skipping the program name")
{
	const char* argv[] = {"/bin/Paralax-Editor", "project.pxproj", "--headless", "--frames", "10"};
	CommandLine args(5, argv);

	CHECK(args.GetProgram() == "/bin/Paralax-Editor");
	REQUIRE(args.GetArguments().size() == 4);
	REQUIRE(args.GetPositional().size() == 1);
	CHECK(args.GetPositional()[0] == "project.pxproj");
	CHECK(args.HasOption("headless"));
	CHECK_FALSE(args.GetOption("headless").has_value());
	CHECK(args.GetOption("frames") == "10");
	CHECK(args.GetUnsignedOption("frames") == 10u);
}

TEST_CASE("CommandLine: supports --name=value and last occurrence wins")
{
	CommandLine args({"--scene=a.pxscene", "--scene", "b.pxscene"});
	CHECK(args.GetOption("scene") == "b.pxscene");
	CHECK(args.GetPositional().empty());
}

TEST_CASE("CommandLine: empty values via --name= are present but empty")
{
	CommandLine args({"--name="});
	CHECK(args.HasOption("name"));
	CHECK(args.GetOption("name") == "");
}

TEST_CASE("CommandLine: -- ends option parsing")
{
	CommandLine args({"--verbose", "--", "--not-an-option", "file"});
	CHECK(args.HasOption("verbose"));
	CHECK_FALSE(args.GetOption("verbose").has_value());
	CHECK_FALSE(args.HasOption("not-an-option"));
	REQUIRE(args.GetPositional().size() == 2);
	CHECK(args.GetPositional()[0] == "--not-an-option");
	CHECK(args.GetPositional()[1] == "file");
}

TEST_CASE("CommandLine: single dashes and bare -- prefixes are positional")
{
	CommandLine args({"-x", "-", "--"});
	CHECK(args.GetPositional() == std::vector<std::string>{"-x", "-"});
}

TEST_CASE("CommandLine: unsigned options reject malformed values")
{
	bool malformed = false;

	CHECK_FALSE(CommandLine({"--frames", "-5"}).GetUnsignedOption("frames", &malformed).has_value());
	CHECK(malformed);

	CHECK_FALSE(CommandLine({"--frames=12abc"}).GetUnsignedOption("frames", &malformed).has_value());
	CHECK(malformed);

	CHECK_FALSE(CommandLine({"--frames"}).GetUnsignedOption("frames", &malformed).has_value());
	CHECK(malformed);

	CHECK_FALSE(CommandLine({"--frames="}).GetUnsignedOption("frames", &malformed).has_value());
	CHECK(malformed);

	CHECK_FALSE(CommandLine({"--frames=99999999999999999999999"}).GetUnsignedOption("frames", &malformed).has_value());
	CHECK(malformed);

	CHECK_FALSE(CommandLine(std::vector<std::string>{}).GetUnsignedOption("frames", &malformed).has_value());
	CHECK_FALSE(malformed);

	CHECK(CommandLine({"--frames=0"}).GetUnsignedOption("frames", &malformed) == 0u);
	CHECK_FALSE(malformed);
}

TEST_CASE("CommandLine: default-constructed and null argv are empty")
{
	CHECK(CommandLine().GetArguments().empty());
	CHECK(CommandLine(0, nullptr).GetProgram().empty());
}

TEST_CASE("CommandLine: flags never consume the following argument")
{
	CommandLine args({"--headless", "project.pxproj", "--frames", "5", "--headless=false"}, {"headless"});
	REQUIRE(args.GetPositional().size() == 1);
	CHECK(args.GetPositional()[0] == "project.pxproj");
	CHECK(args.GetOption("frames") == "5");
	// The explicit "=value" form still works for flags; the last occurrence wins.
	CHECK(args.GetOption("headless") == "false");

	const char* argv[] = {"app", "--headless", "scene"};
	CommandLine fromArgv(3, argv, {"headless"});
	CHECK(fromArgv.GetPositional() == std::vector<std::string>{"scene"});
	CHECK_FALSE(fromArgv.GetOption("headless").has_value());
}
