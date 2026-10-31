#include "pxpch.h"
#include "Paralax/Core/CommandLine.h"

#include <algorithm>
#include <charconv>

namespace Paralax {

	CommandLine::CommandLine(int argc, const char* const* argv, std::vector<std::string> flags)
		: m_Flags(std::move(flags))
	{
		if (argc > 0 && argv && argv[0])
			m_Program = argv[0];

		for (int i = 1; i < argc; i++)
		{
			if (argv[i])
				m_Arguments.emplace_back(argv[i]);
		}

		Parse();
	}

	CommandLine::CommandLine(std::vector<std::string> args, std::vector<std::string> flags)
		: m_Arguments(std::move(args))
		, m_Flags(std::move(flags))
	{
		Parse();
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
