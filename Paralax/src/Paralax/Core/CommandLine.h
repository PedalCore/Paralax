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
	// On Windows that encoding is UTF-8, and argv holds the arguments in the ANSI code page, which loses characters it
	// cannot represent, so they are read again from the process's UTF-16 command line and converted (an unpaired
	// surrogate becomes U+FFFD, as ToUtf8 does). If that fails, or does not match argv (see SelectProcessArguments),
	// argv is used as given and a warning is logged. On POSIX argv is returned unchanged: its bytes are the file
	// names, whatever their encoding, and FromUtf8 passes them through.
	//
	// Never throws (other than std::bad_alloc). Main thread only, called once at startup by the entry point, after
	// Log::Init so the warning can be reported.
	std::vector<std::string> GetProcessArguments(int argc, const char* const* argv);

	// argv[0..argc) as strings, in the native narrow encoding: element i is argv[i], or empty where that is null. An
	// empty list if argv is null or argc is not positive. Pure; safe to call from any thread.
	std::vector<std::string> CopyArgv(int argc, const char* const* argv);

	// The rule GetProcessArguments follows where it reads the arguments again from the operating system (Windows),
	// public so every platform tests it. Returns reread if it holds as many arguments as CopyArgv(argc, argv) and
	// each ASCII one equals argv's (ASCII is the same in every code page, so a difference means the two were split
	// differently, as the C runtime and CommandLineToArgvW can split unusual quoting). Otherwise (nullopt when they
	// could not be read, a different count or a different split) logs one warning and returns CopyArgv(argc, argv).
	// Safe to call from any thread once logging is initialised.
	std::vector<std::string> SelectProcessArguments(int argc, const char* const* argv, std::optional<std::vector<std::string>> reread);

}
