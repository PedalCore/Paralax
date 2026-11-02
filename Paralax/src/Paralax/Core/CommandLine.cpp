#include "pxpch.h"
#include "Paralax/Core/CommandLine.h"

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

	std::vector<std::string> SelectProcessArguments(int argc, const char* const* argv, std::optional<std::vector<std::string>> reread)
	{
		std::vector<std::string> original = CopyArgv(argc, argv);

		// argv may already have lost characters (on Windows, those outside the code page), so a path given there can
		// name another file: say so once, where the cause is known.
		if (!reread)
		{
			PX_CORE_WARN("Could not read the Unicode command line: using argv, whose arguments may have lost characters");
			return original;
		}
		if (reread->size() != original.size())
		{
			PX_CORE_WARN("The Unicode command line has {} arguments but argv has {}: using argv, whose arguments may have lost characters", reread->size(), original.size());
			return original;
		}
		// ASCII is the same in every code page, so an ASCII argument that differs from argv's means the two were split
		// differently (the parsers disagree on unusual quoting), and the arguments no longer correspond.
		for (size_t i = 0; i < original.size(); i++)
		{
			const std::string& text = (*reread)[i];
			const bool ascii = std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
			if (ascii && text != original[i])
			{
				PX_CORE_WARN("The Unicode command line splits differently from argv (argument {}): using argv, whose arguments may have lost characters", i);
				return original;
			}
		}
		return std::move(*reread);
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
