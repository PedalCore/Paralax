#include "pxpch.h"
#include "Paralax/Core/CommandLine.h"

#include "Paralax/Core/PathEncoding.h"

#include <algorithm>
#include <charconv>

namespace Paralax {

	std::vector<std::string> CopyArgv(int argc, const char* const* argv)
	{
		std::vector<std::string> arguments;
		if (argc <= 0 || !argv)
			return arguments;
		arguments.reserve(static_cast<size_t>(argc));
		for (int i = 0; i < argc; i++)
			arguments.emplace_back(argv[i] ? argv[i] : "");
		return arguments;
	}

	std::vector<std::string> SplitWindowsCommandLine(std::u16string_view commandLine)
	{
		// The structure follows the C runtime's parse_command_line, so the two can be compared rule by rule. The line
		// ends at a NUL as well as at its end, as the C runtime reads a NUL-terminated string.
		auto at = [commandLine](size_t index) { return index < commandLine.size() ? commandLine[index] : u'\0'; };
		auto isBlank = [](char16_t c) { return c == u' ' || c == u'\t'; };

		std::vector<std::string> arguments;
		std::u16string argument;
		size_t position = 0;

		// The program path is a file name, which cannot hold a quote, so quotes only group and backslashes are literal.
		bool inQuotes = false;
		for (char16_t c = at(position); c != u'\0' && (inQuotes || !isBlank(c)); c = at(++position))
		{
			if (c == u'"')
				inQuotes = !inQuotes;
			else
				argument += c;
		}
		arguments.push_back(Utf16ToUtf8(argument));

		inQuotes = false;
		for (;;)
		{
			while (isBlank(at(position)))
				position++;
			if (at(position) == u'\0')
				break;

			// An argument ends only at a blank outside quotes or at the end of the line, so quoting is off again at the
			// start of the next one.
			argument.clear();
			for (;;)
			{
				bool copy = true;
				size_t backslashes = 0;
				while (at(position) == u'\\')
				{
					position++;
					backslashes++;
				}
				if (at(position) == u'"')
				{
					if (backslashes % 2 == 0)
					{
						if (inQuotes && at(position + 1) == u'"')
						{
							position++; // "" inside quotes: the second is a literal quote, and quoting continues.
						}
						else
						{
							copy = false;
							inQuotes = !inQuotes;
						}
					}
					backslashes /= 2;
				}
				argument.append(backslashes, u'\\');

				const char16_t c = at(position);
				if (c == u'\0' || (!inQuotes && isBlank(c)))
					break;
				if (copy)
					argument += c;
				position++;
			}
			arguments.push_back(Utf16ToUtf8(argument));
		}
		return arguments;
	}

	CommandLine::CommandLine(int argc, const char* const* argv, std::vector<std::string> flags)
		: CommandLine(FromProcessArguments(CopyArgv(argc, argv), std::move(flags)))
	{
	}

	CommandLine::CommandLine(std::vector<std::string> args, std::vector<std::string> flags)
		: m_Arguments(std::move(args))
		, m_Flags(std::move(flags))
	{
		Parse();
	}

	CommandLine CommandLine::FromProcessArguments(std::vector<std::string> arguments, std::vector<std::string> flags)
	{
		std::string program;
		if (!arguments.empty())
		{
			program = std::move(arguments.front());
			arguments.erase(arguments.begin());
		}
		CommandLine commandLine(std::move(arguments), std::move(flags));
		commandLine.m_Program = std::move(program);
		return commandLine;
	}

	void CommandLine::Parse()
	{
		bool optionsEnded = false;

		for (size_t i = 0; i < m_Arguments.size(); i++)
		{
			const std::string& arg = m_Arguments[i];

			if (optionsEnded || arg.size() < 3 || !arg.starts_with("--"))
			{
				if (!optionsEnded && arg == "--")
				{
					optionsEnded = true;
					continue;
				}

				m_Positional.push_back(arg);
				continue;
			}

			Option option;
			const size_t equals = arg.find('=');
			if (equals != std::string::npos)
			{
				option.Name = arg.substr(2, equals - 2);
				option.Value = arg.substr(equals + 1);
			}
			else
			{
				option.Name = arg.substr(2);
				// A following argument that is not itself an option is this option's value, unless it is a flag.
				const bool isFlag = std::find(m_Flags.begin(), m_Flags.end(), option.Name) != m_Flags.end();
				if (!isFlag && i + 1 < m_Arguments.size() && !m_Arguments[i + 1].starts_with("--"))
					option.Value = m_Arguments[++i];
			}

			m_Options.push_back(std::move(option));
		}
	}

	bool CommandLine::HasOption(std::string_view name) const
	{
		return std::any_of(m_Options.begin(), m_Options.end(), [name](const Option& o) { return o.Name == name; });
	}

	std::optional<std::string> CommandLine::GetOption(std::string_view name) const
	{
		// The last occurrence wins, matching common CLI conventions.
		for (auto it = m_Options.rbegin(); it != m_Options.rend(); ++it)
		{
			if (it->Name == name)
				return it->Value;
		}

		return std::nullopt;
	}

	std::optional<uint64_t> CommandLine::GetUnsignedOption(std::string_view name, bool* outMalformed) const
	{
		if (outMalformed)
			*outMalformed = false;

		std::optional<std::string> value = GetOption(name);
		if (!value)
		{
			if (outMalformed && HasOption(name))
				*outMalformed = true;
			return std::nullopt;
		}

		uint64_t result = 0;
		const char* first = value->data();
		const char* last = first + value->size();
		auto [ptr, ec] = std::from_chars(first, last, result);
		if (ec != std::errc() || ptr != last || value->empty())
		{
			if (outMalformed)
				*outMalformed = true;
			return std::nullopt;
		}

		return result;
	}

}
