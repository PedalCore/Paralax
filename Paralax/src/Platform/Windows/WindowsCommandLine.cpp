#include "pxpch.h"
#include "Paralax/Core/CommandLine.h"

#if defined(PX_PLATFORM_WINDOWS)

	#include <Windows.h>
	// CommandLineToArgvW. WIN32_LEAN_AND_MEAN (set for every target) keeps Windows.h from including it, and it needs
	// Windows.h first.
	#include <shellapi.h>

	#include <climits>
	#include <cwchar>
	#include <memory>

namespace Paralax {

	namespace {

		// CommandLineToArgvW allocates the array, and its strings, as one LocalAlloc block.
		struct LocalFreeDeleter
		{
			void operator()(LPWSTR* arguments) const { LocalFree(arguments); }
		};

		// One argument as UTF-8, or nullopt if Windows cannot convert it. Without WC_ERR_INVALID_CHARS an unpaired
		// surrogate (which Windows allows in file names) becomes U+FFFD, as ToUtf8 does for paths, rather than losing
		// every other argument to the code page.
		std::optional<std::string> WideArgumentToUtf8(const wchar_t* argument)
		{
			const size_t length = std::wcslen(argument);
			if (length == 0)
				return std::string();
			if (length > static_cast<size_t>(INT_MAX))
				return std::nullopt;

			const int wideLength = static_cast<int>(length);
			const int size = WideCharToMultiByte(CP_UTF8, 0, argument, wideLength, nullptr, 0, nullptr, nullptr);
			if (size <= 0)
				return std::nullopt;
			std::string text(static_cast<size_t>(size), '\0');
			if (WideCharToMultiByte(CP_UTF8, 0, argument, wideLength, text.data(), size, nullptr, nullptr) != size)
				return std::nullopt;
			return text;
		}

		// The arguments split from the process's UTF-16 command line, or nullopt if Windows cannot split or convert
		// them. CommandLineToArgvW splits as the C runtime splits argv except for unusual quoting (consecutive quotes
		// inside a quoted argument); SelectProcessArguments uses argv where the two splits visibly differ.
		std::optional<std::vector<std::string>> ReadUnicodeArguments()
		{
			int count = 0;
			const std::unique_ptr<LPWSTR, LocalFreeDeleter> wide(CommandLineToArgvW(GetCommandLineW(), &count));
			if (!wide || count < 0)
				return std::nullopt;

			std::vector<std::string> arguments;
			arguments.reserve(static_cast<size_t>(count));
			for (int i = 0; i < count; i++)
			{
				const wchar_t* argument = wide.get()[i];
				if (!argument)
					return std::nullopt;
				std::optional<std::string> converted = WideArgumentToUtf8(argument);
				if (!converted)
					return std::nullopt;
				arguments.push_back(std::move(*converted));
			}
			return arguments;
		}

	}

	std::vector<std::string> GetProcessArguments(int argc, const char* const* argv)
	{
		// argv is in the ANSI code page, which replaces the characters it cannot represent (often with '?'), so it is
		// only the fallback.
		return SelectProcessArguments(argc, argv, ReadUnicodeArguments());
	}

}

#endif
