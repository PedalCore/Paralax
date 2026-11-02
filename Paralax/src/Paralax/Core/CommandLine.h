#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Paralax {

	// Parsed command-line arguments. Options take the forms "--name", "--name value" and "--name=value";
	// everything else is a positional argument. "--" ends option parsing. Options listed as flags never take a
	// separate value ("--headless scene.pxscene" keeps scene.pxscene positional), but accept "--flag=value".
	//
	// Arguments are kept as given. Built from GetProcessArguments (as the entry point does) they are in the engine's
	// path encoding, so an option that names a file becomes a path through FromUtf8 (Core/PathEncoding.h), never
	// path(std::string).
	class CommandLine
	{
	public:
		CommandLine() = default;
		// argv[0] is the program; the arguments follow. A null entry is an empty argument (see CopyArgv).
		CommandLine(int argc, const char* const* argv, std::vector<std::string> flags = {});
		// The arguments alone, without a program path.
		explicit CommandLine(std::vector<std::string> args, std::vector<std::string> flags = {});

		// The process arguments as GetProcessArguments returns them: the program path first, then the arguments.
		// An empty list gives an empty command line.
		static CommandLine FromProcessArguments(std::vector<std::string> arguments, std::vector<std::string> flags = {});

		// The program path (argv[0]), or empty if not provided. UTF-8 when built from GetProcessArguments.
		const std::string& GetProgram() const { return m_Program; }
		const std::vector<std::string>& GetArguments() const { return m_Arguments; }
		const std::vector<std::string>& GetPositional() const { return m_Positional; }

		bool HasOption(std::string_view name) const;
		std::optional<std::string> GetOption(std::string_view name) const;

		// Returns the option parsed as an unsigned integer, std::nullopt if absent, or the error flag set if malformed.
		std::optional<uint64_t> GetUnsignedOption(std::string_view name, bool* outMalformed = nullptr) const;

	private:
		void Parse();

		struct Option
		{
			std::string Name;
			std::optional<std::string> Value;
		};

		std::string m_Program;
		std::vector<std::string> m_Arguments;
		std::vector<std::string> m_Positional;
		std::vector<Option> m_Options;
		std::vector<std::string> m_Flags;
	};

	// The arguments the process was started with, the program path (argv[0]) first, in the engine's encoding for
	// string paths (what FromUtf8 takes), so a path given on the command line reaches the engine whatever characters
	// it holds. Pass main's argc and argv; the result is a copy the caller owns.
	//
	// On Windows that encoding is UTF-8, and argv is unusable: the C runtime splits it from the ANSI command line,
	// which Windows derives from the UTF-16 one with best-fit mapping. Characters the code page lacks become '?' or
	// look-alikes, and some look-alikes are quotes or backslashes (U+FF02 becomes '"'), so a crafted file name could
	// split into other arguments, options included. The UTF-16 command line is split instead, by
	// SplitWindowsCommandLine. Only if Windows cannot provide it is argv used, converted from the code page (so the
	// characters it can represent survive), with a warning logged. On POSIX argv is returned unchanged: its bytes are
	// the file names, whatever their encoding, and FromUtf8 passes them through.
	//
	// Never throws (other than std::bad_alloc). Main thread only, called once at startup by the entry point, after
	// Log::Init so the warning can be reported.
	std::vector<std::string> GetProcessArguments(int argc, const char* const* argv);

	// argv[0..argc) as strings, in the native narrow encoding: element i is argv[i], or empty where that is null. An
	// empty list if argv is null or argc is not positive. Pure; safe to call from any thread.
	std::vector<std::string> CopyArgv(int argc, const char* const* argv);

	// Splits a Windows command line (as GetCommandLineW gives it) into its arguments, the program path first, each as
	// UTF-8 (an unpaired surrogate becomes U+FFFD). It follows the Universal C Runtime's parse_command_line, which
	// builds argv for every MSVC program since Visual Studio 2015, so the arguments are the ones the C runtime would
	// give a program whose code page could represent them, and the ones the tools that start programs quote for
	// (CMake, Python, cmd). CommandLineToArgvW is not used: it differs from the C runtime on "" inside quotes.
	//   - Program path: '"' toggles quoting and is dropped; everything else, '\' included, is literal. It ends at a
	//     space or tab outside quotes. (A leading space therefore gives an empty program path.)
	//   - Other arguments are separated by runs of spaces and tabs (only those) outside quotes.
	//   - 2n backslashes then '"': n backslashes, and the quote toggles quoting.
	//   - 2n+1 backslashes then '"': n backslashes and a literal '"'.
	//   - Backslashes not followed by '"' are literal.
	//   - Inside quotes, "" is a literal '"' and quoting continues (the rule since Visual Studio 2008).
	//   - The line ends at its end or at a NUL. Trailing spaces add no argument; "" (or a lone '"') is an empty one.
	// An empty command line gives one empty program path. Pure; safe to call from any thread. Public so every platform
	// tests it; only Windows uses it.
	std::vector<std::string> SplitWindowsCommandLine(std::u16string_view commandLine);

}
