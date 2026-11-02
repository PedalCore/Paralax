#include "pxpch.h"
#include "Paralax/Core/CommandLine.h"

#include "Paralax/Core/EngineResources.h"
#include "Paralax/Core/PathEncoding.h"

#if defined(PX_PLATFORM_WINDOWS)

	#include <Windows.h>

	#include <algorithm>
	#include <climits>
	#include <cstring>
	#include <string_view>

namespace Paralax {

	namespace {

		// wchar_t is a UTF-16 unit on Windows: copied into the type the engine's tested conversions take.
		std::u16string ToUtf16(std::wstring_view text)
		{
			std::u16string units(text.size(), u'\0');
			std::transform(text.begin(), text.end(), units.begin(), [](wchar_t unit) { return static_cast<char16_t>(unit); });
			return units;
		}

		// One argv argument from the ANSI code page to UTF-8, keeping the characters the code page can represent (the
		// others are already lost). If Windows cannot convert it, its bytes are kept: FromUtf8 turns what is not UTF-8
		// into U+FFFD and never throws.
		std::string AnsiToUtf8(const char* argument)
		{
			const size_t length = std::strlen(argument);
			if (length == 0 || length > static_cast<size_t>(INT_MAX))
				return std::string(argument, length);

			const int narrowLength = static_cast<int>(length);
			const int size = MultiByteToWideChar(CP_ACP, 0, argument, narrowLength, nullptr, 0);
			if (size <= 0)
				return std::string(argument, length);
			std::wstring wide(static_cast<size_t>(size), L'\0');
			if (MultiByteToWideChar(CP_ACP, 0, argument, narrowLength, wide.data(), size) != size)
				return std::string(argument, length);
			return Utf16ToUtf8(ToUtf16(wide));
		}

	}

	std::vector<std::string> GetProcessArguments(int argc, const char* const* argv)
	{
		// argv is never preferred: it is the C runtime's split of the ANSI command line, which best-fit mapping can
		// give different quotes and backslashes (see CommandLine.h), so it may not even split the same way. What to make
		// of the UTF-16 line is decided in Core, where every platform tests it.
		const wchar_t* commandLine = GetCommandLineW();
		const std::u16string units = commandLine ? ToUtf16(commandLine) : std::u16string();
		const std::u16string executable = ToUtf16(GetExecutablePath().wstring());
		if (std::optional<std::vector<std::string>> fromCommandLine = ProcessArgumentsFromCommandLine(commandLine ? units.c_str() : nullptr, executable))
			return std::move(*fromCommandLine);

		// No command line, which Windows does not document as possible: a guard, so the program still starts with argv.
		PX_CORE_WARN("Could not read the Unicode command line: using argv in the system code page, which may have lost characters");
		std::vector<std::string> arguments;
		if (argc <= 0 || !argv)
			return arguments;
		arguments.reserve(static_cast<size_t>(argc));
		for (int i = 0; i < argc; i++)
			arguments.push_back(argv[i] ? AnsiToUtf8(argv[i]) : std::string());
		return arguments;
	}

}

#endif
