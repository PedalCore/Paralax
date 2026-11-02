#include "pxpch.h"
#include "Paralax/Core/Environment.h"

#include "Paralax/Core/PathEncoding.h"

#if defined(PX_PLATFORM_WINDOWS)

	#include <algorithm>
	#include <cstdlib>
	#include <cwchar>

namespace Paralax {

	namespace {

		// The C runtime treats a name or value of _MAX_ENV UTF-16 units or more as an invalid parameter, whose default
		// handler ends the process, when reading as well as writing: such names and values never reach it.
		constexpr size_t EnvironmentLimit = _MAX_ENV;

		// wchar_t is a UTF-16 unit on Windows: converted with the engine's tested transcoding.
		std::wstring ToWide(std::string_view text)
		{
			const std::u16string units = Utf8ToUtf16(text);
			std::wstring wide(units.size(), L'\0');
			std::transform(units.begin(), units.end(), wide.begin(), [](char16_t unit) { return static_cast<wchar_t>(unit); });
			return wide;
		}

		std::string FromWide(const wchar_t* text)
		{
			std::u16string units(std::wcslen(text), u'\0');
			std::transform(text, text + units.size(), units.begin(), [](wchar_t unit) { return static_cast<char16_t>(unit); });
			return Utf16ToUtf8(units);
		}

	}

	std::optional<std::string> GetProcessEnvironmentVariable(std::string_view name)
	{
		if (!IsValidEnvironmentVariableName(name))
			return std::nullopt;
		// The C runtime's wide copy, which SetProcessEnvironmentVariable keeps current. Its narrow copy (what getenv
		// reads) is in the ANSI code page, without the characters the code page lacks.
		const std::wstring wideName = ToWide(name);
		if (wideName.size() >= EnvironmentLimit)
			return std::nullopt;
		if (const wchar_t* value = _wgetenv(wideName.c_str()))
			return FromWide(value);
		return std::nullopt;
	}

	bool SetProcessEnvironmentVariable(std::string_view name, std::optional<std::string_view> value)
	{
		if (!IsValidEnvironmentVariableName(name) || (value && value->find('\0') != std::string_view::npos))
			return false;
		// The C runtime caches the environment: getenv and _wgetenv read its own narrow and wide copies, not the
		// process's block. SetEnvironmentVariableW changes only that block, so getenv would keep returning the old
		// value. _wputenv_s updates both copies (the narrow one converted to the ANSI code page) and the block, which
		// child processes inherit. An empty value is how it removes a variable.
		const std::wstring wideName = ToWide(name);
		const std::wstring wideValue = value ? ToWide(*value) : std::wstring();
		if (wideName.size() >= EnvironmentLimit || wideValue.size() >= EnvironmentLimit)
			return false;
		return _wputenv_s(wideName.c_str(), wideValue.c_str()) == 0;
	}

}

#endif
