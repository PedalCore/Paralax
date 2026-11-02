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

	// A null entry keeps its position as an empty argument, in CommandLine(argc, argv) too.
	CommandLine args(4, argv);
	CHECK(args.GetProgram() == "app");
	CHECK(args.GetArguments() == std::vector<std::string>{"", "--frames", "3"});
	CHECK(args.GetPositional() == std::vector<std::string>{""});
	CHECK(args.GetOption("frames") == "3");

	// In a value's position it is the (empty) value: skipping it would make the next argument the value instead.
	const char* nullValue[] = {"app", "--frames", nullptr, "3"};
	CommandLine valueArgs(4, nullValue);
	CHECK(valueArgs.GetArguments() == std::vector<std::string>{"--frames", "", "3"});
	CHECK(valueArgs.GetOption("frames") == "");
	CHECK(valueArgs.GetPositional() == std::vector<std::string>{"3"});
}

TEST_CASE("SplitWindowsCommandLine: splits as the C runtime splits argv")
{
	struct Case
	{
		const char* Rule;
		std::u16string_view Line;
		std::vector<std::string> Expected;
	};
	const Case cases[] = {
		{"a program path alone", u"prog", {"prog"}},
		{"an empty line is an empty program path", u"", {""}},
		{"a leading blank ends an empty program path", u"  prog a", {"", "prog", "a"}},
		{"spaces and tabs separate arguments", u"prog a  b\tc", {"prog", "a", "b", "c"}},
		{"trailing blanks add no argument", u"prog a \t ", {"prog", "a"}},
		{"only spaces and tabs separate", u"prog a\nb\rc", {"prog", "a\nb\rc"}},
		{"a quoted program path keeps its spaces", uR"("C:\Program Files\Game\Game.exe" --headless)", {R"(C:\Program Files\Game\Game.exe)", "--headless"}},
		{"backslashes before a quote are literal in the program path", uR"("C:\Games\" --headless)", {R"(C:\Games\)", "--headless"}},
		{"quotes anywhere in the program path group", uR"(C:\My" "Games\Game.exe a)", {R"(C:\My Games\Game.exe)", "a"}},
		{"a program path ending inside quotes runs to the end", uR"("C:\My Games\Game.exe --headless)", {R"(C:\My Games\Game.exe --headless)"}},
		{"\"\" in the program path closes and reopens quoting", uR"("C:\a""b c" d)", {R"(C:\ab c)", "d"}},
		{"an empty quoted program path", uR"("" a)", {"", "a"}},
		{"a leading tab ends an empty program path", u"\tprog", {"", "prog"}},
		{"a quoted argument keeps its blanks", u"prog \"a b\" \"c\td\"", {"prog", "a b", "c\td"}},
		{"quotes inside an argument group", uR"(prog a"b c"d)", {"prog", "ab cd"}},
		{"\"\" is an empty argument", uR"(prog "" "")", {"prog", "", ""}},
		{"a lone quote at the end of the line is an empty argument", uR"(prog a ")", {"prog", "a", ""}},
		{"a lone quote elsewhere opens quoting", uR"(prog " x)", {"prog", " x"}},
		{"an unterminated quote runs to the end", uR"(prog "a b)", {"prog", "a b"}},
		{"backslashes not before a quote are literal", uR"(prog C:\a\\b\\\c d\)", {"prog", R"(C:\a\\b\\\c)", R"(d\)"}},
		{"2n backslashes and a quote give n backslashes and toggle quoting", uR"(prog "a\\" b \\\\"c d")", {"prog", R"(a\)", "b", R"(\\c d)"}},
		{"2n+1 backslashes and a quote give n backslashes and a literal quote", uR"(prog \"a b\" \\\"c)", {"prog", R"("a)", R"(b")", R"(\"c)"}},
		{"inside quotes \"\" is a literal quote and quoting continues", uR"(prog "a""b c" d)", {"prog", R"(a"b c)", "d"}},
		// CommandLineToArgvW ends the quoting at "" instead, which gives as many arguments, split elsewhere.
		{"the C runtime's split where CommandLineToArgvW differs", uR"(prog a "b""c d" e)", {"prog", "a", R"(b"c d)", "e"}},
		{"backslashes before \"\" inside quotes", uR"(prog "a\\""b")", {"prog", R"(a\"b)"}},
		// The parity comes first: after an odd count the quote is literal, so a following quote closes the quoting.
		{"2n+1 backslashes before \"\" inside quotes", uR"(prog "a\\\"" b)", {"prog", R"(a\")", "b"}},
		{"2n backslashes before \"\" inside quotes keep quoting", uR"(prog "a\\"" b)", {"prog", R"(a\" b)"}},
		{"outside quotes \"\" is an empty group", uR"(prog a""b)", {"prog", "ab"}},
		{"three quotes open quoting with a literal quote", uR"(prog """ x)", {"prog", R"(" x)"}},
		{"four quotes are one literal quote", uR"(prog """" x)", {"prog", R"(")", "x"}},
		{"a test filter as CTest quotes it", uR"(prog "--test-case=Thing: a\, b")", {"prog", R"(--test-case=Thing: a\, b)"}},
		{"a NUL ends the line", std::u16string_view(u"prog a\0b c", 10), {"prog", "a"}},
	};
	for (const Case& testCase : cases)
	{
		CAPTURE(testCase.Rule);
		CHECK(SplitWindowsCommandLine(testCase.Line) == testCase.Expected);
	}
}

TEST_CASE("SplitWindowsCommandLine: best-fit look-alikes of quotes and backslashes stay what they are")
{
	// Windows maps these to '"' and '\' when it makes the ANSI command line argv is split from (best fit), which
	// would split a file name holding them into other arguments. In the UTF-16 line they are ordinary characters.
	// U+FF38 and U+FF39 are fullwidth X and Y, U+FF02 a fullwidth quote, U+FF3C a fullwidth backslash and U+00A5 the
	// yen sign (a backslash in the Japanese code page).
	CHECK(SplitWindowsCommandLine(u"prog --project \"D:/My Games/\uFF38\uFF02 \uFF39\" --headless") ==
		  std::vector<std::string>{"prog", "--project", "D:/My Games/\xEF\xBC\xB8\xEF\xBC\x82 \xEF\xBC\xB9", "--headless"});
	CHECK(SplitWindowsCommandLine(u"prog \uFF02--export x\uFF02") == std::vector<std::string>{"prog", "\xEF\xBC\x82--export", "x\xEF\xBC\x82"});
	CHECK(SplitWindowsCommandLine(u"prog \uFF3C\"a b\"") == std::vector<std::string>{"prog", "\xEF\xBC\xBC"
																							 "a b"});
	CHECK(SplitWindowsCommandLine(u"prog \u00A5\"a b\"") == std::vector<std::string>{"prog", "\xC2\xA5"
																							 "a b"});
}

TEST_CASE("SplitWindowsCommandLine: converts each argument to UTF-8")
{
	const std::string name(ProjectName);
	CHECK(SplitWindowsCommandLine(u"\"C:/Spiele/Projekt-\u00E9-\u65E5\u672C-\u03A9/Game.exe\" --project \"Projekt-\u00E9-\u65E5\u672C-\u03A9\"") ==
		  std::vector<std::string>{"C:/Spiele/" + name + "/Game.exe", "--project", name});

	// A surrogate pair is one character; an unpaired surrogate (allowed in Windows file names) becomes U+FFFD, as
	// ToUtf8 does, and never takes the quote after it.
	CHECK(SplitWindowsCommandLine(u"prog \U0001F600") == std::vector<std::string>{"prog", "\xF0\x9F\x98\x80"});
	std::u16string unpaired = u"prog a";
	unpaired += static_cast<char16_t>(0xD800);
	unpaired += u"b \"";
	unpaired += static_cast<char16_t>(0xDC00);
	unpaired += u"\" c";
	CHECK(SplitWindowsCommandLine(unpaired) == std::vector<std::string>{"prog", "a\xEF\xBF\xBD"
																				"b",
																		"\xEF\xBF\xBD", "c"});
}

TEST_CASE("ProcessArgumentsFromCommandLine: an empty line gives the executable's path whole")
{
	// The C runtime would split this path at its space, as if it were the command line.
	const std::optional<std::vector<std::string>> arguments = ProcessArgumentsFromCommandLine(u"", uR"(C:\Program Files\Game\Game.exe)");
	REQUIRE(arguments.has_value());
	CHECK(*arguments == std::vector<std::string>{R"(C:\Program Files\Game\Game.exe)"});

	// Converted to UTF-8 like every argument.
	CHECK(ProcessArgumentsFromCommandLine(u"", u"C:/Spiele/Projekt-\u00E9-\u65E5\u672C-\u03A9/Game.exe") == std::vector<std::string>{"C:/Spiele/" + std::string(ProjectName) + "/Game.exe"});
	// Without a known path the program path is empty, as SplitWindowsCommandLine gives for an empty line.
	CHECK(ProcessArgumentsFromCommandLine(u"", u"") == std::vector<std::string>{""});
}

TEST_CASE("ProcessArgumentsFromCommandLine: a line of blanks is split, not replaced by the executable's path")
{
	// Not empty, so the C runtime splits it too: an empty program path and nothing else.
	CHECK(ProcessArgumentsFromCommandLine(u" ", u"C:/Game.exe") == std::vector<std::string>{""});
	CHECK(ProcessArgumentsFromCommandLine(u"\t ", u"C:/Game.exe") == std::vector<std::string>{""});
}

TEST_CASE("ProcessArgumentsFromCommandLine: a normal line is split and the executable's path is ignored")
{
	CHECK(ProcessArgumentsFromCommandLine(uR"("C:\Games\Game.exe" --project "My Game")", u"D:/Other.exe") == std::vector<std::string>{R"(C:\Games\Game.exe)", "--project", "My Game"});
	CHECK(ProcessArgumentsFromCommandLine(u"prog", u"D:/Other.exe") == std::vector<std::string>{"prog"});
}

TEST_CASE("ProcessArgumentsFromCommandLine: no line gives nothing, so the caller falls back to argv")
{
	CHECK_FALSE(ProcessArgumentsFromCommandLine(nullptr, u"C:/Game.exe").has_value());
	CHECK_FALSE(ProcessArgumentsFromCommandLine(nullptr, u"").has_value());
}

TEST_CASE("GetProcessArguments: gives this process's arguments as the C runtime split them, the program first")
{
	// main's own arguments. On Windows this splits the test runner's real UTF-16 command line, which checks
	// SplitWindowsCommandLine against the C runtime itself: argv is its split of the same line (CTest quotes the test
	// filter), so the two must agree wherever the code page cannot have changed a character, in ASCII arguments.
	const Testing::MainArguments& mainArguments = Testing::GetMainArguments();
	REQUIRE(mainArguments.Count > 0);

	Testing::ScopedLogCapture capture;
	const std::vector<std::string> arguments = GetProcessArguments(mainArguments.Count, mainArguments.Values);
	CHECK_FALSE(LoggedArgumentWarning(capture));
	REQUIRE(arguments.size() == static_cast<size_t>(mainArguments.Count));
	CHECK(FromUtf8(arguments.front()).stem() == "Paralax-Tests");
	for (size_t i = 0; i < arguments.size(); i++)
	{
		CAPTURE(i);
		const std::string& argument = arguments[i];
		if (std::all_of(argument.begin(), argument.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }))
			CHECK(argument == mainArguments.Values[i]);
	}
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
