#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/CommandLine.h>
#include <Paralax/Core/PathEncoding.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Paralax;

namespace {

	// "Projekt-é-日本-Ω", spelled as UTF-8 bytes so the test does not depend on how the compiler reads the source. Its
	// characters share no ANSI code page, so on Windows argv could never hold all of them.
	constexpr std::string_view ProjectName = "Projekt-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC-\xCE\xA9";

	// POSIX paths are bytes; Windows paths are UTF-16.
	constexpr bool NativePathsAreBytes = sizeof(std::filesystem::path::value_type) == 1;

	bool LoggedArgumentWarning(const Testing::ScopedLogCapture& capture)
	{
		return capture.Contains(spdlog::level::warn, "command line");
	}

}

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

TEST_CASE("CommandLine: built from the process arguments, the first is the program")
{
	const std::string project = "/home/user/" + std::string(ProjectName);
	CommandLine args = CommandLine::FromProcessArguments({"/bin/Paralax-Editor", "--headless", "--project", project, "scene"}, {"headless"});

	CHECK(args.GetProgram() == "/bin/Paralax-Editor");
	CHECK(args.GetArguments() == std::vector<std::string>{"--headless", "--project", project, "scene"});
	CHECK(args.HasOption("headless"));
	CHECK(args.GetOption("project") == project);
	CHECK(args.GetPositional() == std::vector<std::string>{"scene"});
}

TEST_CASE("CommandLine: an empty process argument list is an empty command line")
{
	CommandLine args = CommandLine::FromProcessArguments({});
	CHECK(args.GetProgram().empty());
	CHECK(args.GetArguments().empty());

	CommandLine programOnly = CommandLine::FromProcessArguments({"Paralax-Runtime"});
	CHECK(programOnly.GetProgram() == "Paralax-Runtime");
	CHECK(programOnly.GetArguments().empty());
}

TEST_CASE("CommandLine: non-ASCII option values and program paths keep every byte")
{
	const std::string name(ProjectName);
	const std::string program = "C:/Spiele/" + name + "/Game.exe";
	const std::string scene = "Scenes/" + name + ".pxscene";
	CommandLine args = CommandLine::FromProcessArguments({program, "--project", name, "--scene=" + scene, "--export", "Ausgabe-\xCE\xA9", name});

	CHECK(args.GetProgram() == program);
	CHECK(args.GetOption("project") == name);
	CHECK(args.GetOption("scene") == scene);
	CHECK(args.GetOption("export") == "Ausgabe-\xCE\xA9");
	CHECK(args.GetPositional() == std::vector<std::string>{name});

	// Decoded as the editor and the runtime decode them, they are the paths named, on every platform.
	CHECK(FromUtf8(*args.GetOption("scene")) == std::filesystem::path(u8"Scenes/Projekt-\u00E9-\u65E5\u672C-\u03A9.pxscene"));
	CHECK(ToUtf8(FromUtf8(*args.GetOption("project"))) == name);
	CHECK(ToUtf8(FromUtf8(args.GetProgram()).filename()) == "Game.exe");
}

TEST_CASE("CommandLine: option values that are not UTF-8 pass through unchanged")
{
	// POSIX file names are bytes and need not be UTF-8: such a path must still name its file.
	const std::string bytes = "Latin1-\xE9-\xFF";
	CommandLine args = CommandLine::FromProcessArguments({"app", "--project", bytes});
	REQUIRE(args.GetOption("project") == bytes);

	// Where paths are UTF-16 (Windows) each malformed byte becomes U+FFFD instead, never a throw. This is not a
	// template, so both branches below are compiled everywhere: both compare strings.
	const std::string replaced = "Latin1-\xEF\xBF\xBD-\xEF\xBF\xBD";
	CHECK(Utf16ToUtf8(Utf8ToUtf16(bytes)) == replaced);
	std::filesystem::path path;
	CHECK_NOTHROW(path = FromUtf8(*args.GetOption("project")));
	if constexpr (NativePathsAreBytes)
		CHECK(ToUtf8(path) == bytes);
	else
		CHECK(ToUtf8(path) == replaced);
}

TEST_CASE("CommandLine: argc/argv parses like the process argument list")
{
	const std::string name(ProjectName);
	const char* argv[] = {"/bin/Paralax-Runtime", "--project", name.c_str(), "--headless"};
	CommandLine fromArgv(4, argv, {"headless"});
	CommandLine fromList = CommandLine::FromProcessArguments(CopyArgv(4, argv), {"headless"});

	CHECK(fromArgv.GetProgram() == fromList.GetProgram());
	CHECK(fromArgv.GetArguments() == fromList.GetArguments());
	CHECK(fromArgv.GetOption("project") == name);
	CHECK(fromList.GetOption("project") == name);
}

TEST_CASE("CopyArgv: copies argc entries, null ones as empty strings")
{
	const char* argv[] = {"app", nullptr, "--frames", "3", nullptr};
	CHECK(CopyArgv(4, argv) == std::vector<std::string>{"app", "", "--frames", "3"});
	CHECK(CopyArgv(1, argv) == std::vector<std::string>{"app"});
	CHECK(CopyArgv(0, argv).empty());
	CHECK(CopyArgv(-1, argv).empty());
	CHECK(CopyArgv(3, nullptr).empty());

	// A null entry keeps its position, so the following option still finds its value.
	CommandLine args(4, argv);
	CHECK(args.GetProgram() == "app");
	CHECK(args.GetOption("frames") == "3");
}

TEST_CASE("SelectProcessArguments: uses the arguments read again when there are argc of them")
{
	const std::string name(ProjectName);
	// What argv holds on Windows for a name the code page cannot represent.
	const char* argv[] = {"Paralax-Editor.exe", "--project", "Projekt-\xE9-\?\?-\?"};
	std::vector<std::string> reread = {"Paralax-Editor.exe", "--project", name};

	Testing::ScopedLogCapture capture;
	CHECK(SelectProcessArguments(3, argv, reread) == reread);
	CHECK_FALSE(LoggedArgumentWarning(capture));

	// No arguments at all is consistent too.
	CHECK(SelectProcessArguments(0, nullptr, std::vector<std::string>{}).empty());
	CHECK_FALSE(LoggedArgumentWarning(capture));
}

TEST_CASE("SelectProcessArguments: falls back to argv with one warning when the arguments could not be read")
{
	const char* argv[] = {"Paralax-Editor.exe", "--headless"};

	Testing::ScopedLogCapture capture;
	CHECK(SelectProcessArguments(2, argv, std::nullopt) == std::vector<std::string>{"Paralax-Editor.exe", "--headless"});
	const std::vector<Testing::ScopedLogCapture::Entry> entries = capture.GetEntries();
	CHECK(std::count_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.Level == spdlog::level::warn; }) == 1);
	CHECK(LoggedArgumentWarning(capture));
}

TEST_CASE("SelectProcessArguments: falls back to argv with one warning when the count differs from argc")
{
	const char* argv[] = {"Paralax-Editor.exe", "--project", "a b"};

	for (const std::vector<std::string>& reread : {std::vector<std::string>{"Paralax-Editor.exe", "--project", "a", "b"}, std::vector<std::string>{"Paralax-Editor.exe", "--project"}, std::vector<std::string>{}})
	{
		CAPTURE(reread.size());
		Testing::ScopedLogCapture capture;
		CHECK(SelectProcessArguments(3, argv, reread) == std::vector<std::string>{"Paralax-Editor.exe", "--project", "a b"});
		const std::vector<Testing::ScopedLogCapture::Entry> entries = capture.GetEntries();
		CHECK(std::count_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.Level == spdlog::level::warn; }) == 1);
		CHECK(LoggedArgumentWarning(capture));
	}
}

TEST_CASE("SelectProcessArguments: falls back to argv with one warning when an ASCII argument differs from argv")
{
	// The same count, split differently (as the C runtime and CommandLineToArgvW can split a quoted "" differently).
	// ASCII is the same in every code page, so a difference there can only be a different split.
	const char* argv[] = {"Paralax-Editor.exe", "a", "b\"c d", "e"};
	const std::vector<std::string> reread = {"Paralax-Editor.exe", "a", "b\"c", "d e"};

	Testing::ScopedLogCapture capture;
	CHECK(SelectProcessArguments(4, argv, reread) == std::vector<std::string>{"Paralax-Editor.exe", "a", "b\"c d", "e"});
	const std::vector<Testing::ScopedLogCapture::Entry> entries = capture.GetEntries();
	CHECK(std::count_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.Level == spdlog::level::warn; }) == 1);
	CHECK(LoggedArgumentWarning(capture));

	// A null argv entry (an empty argument) differs from a non-empty ASCII one too.
	const char* withNull[] = {"Paralax-Editor.exe", nullptr};
	CHECK(SelectProcessArguments(2, withNull, std::vector<std::string>{"Paralax-Editor.exe", "x"}) == std::vector<std::string>{"Paralax-Editor.exe", ""});
}

TEST_CASE("GetProcessArguments: gives this process's argc arguments, the program first")
{
	// main's own arguments: on Windows this reads and converts the test runner's real UTF-16 command line.
	const Testing::MainArguments& mainArguments = Testing::GetMainArguments();
	REQUIRE(mainArguments.Count > 0);

	Testing::ScopedLogCapture capture;
	const std::vector<std::string> arguments = GetProcessArguments(mainArguments.Count, mainArguments.Values);
	CHECK_FALSE(LoggedArgumentWarning(capture));
	REQUIRE(arguments.size() == static_cast<size_t>(mainArguments.Count));
	CHECK(FromUtf8(arguments.front()).stem() == "Paralax-Tests");
	// Converted from UTF-16 they are always well formed; POSIX bytes are passed on as they are.
	if constexpr (!NativePathsAreBytes)
	{
		for (const std::string& argument : arguments)
			CHECK(IsWellFormedUtf8(argument));
	}
	else
	{
		CHECK(arguments == CopyArgv(mainArguments.Count, mainArguments.Values));
	}
}
