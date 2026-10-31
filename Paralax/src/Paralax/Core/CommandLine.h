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
	class CommandLine
	{
	public:
		CommandLine() = default;
		CommandLine(int argc, const char* const* argv, std::vector<std::string> flags = {});
		explicit CommandLine(std::vector<std::string> args, std::vector<std::string> flags = {});

		// The program path (argv[0]), or empty if not provided.
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

}
