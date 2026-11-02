#include "pxpch.h"
#include "Paralax/Core/Environment.h"

#if !defined(PX_PLATFORM_WINDOWS)

	// setenv and unsetenv are POSIX, declared by the C header rather than <cstdlib>.
	#include <stdlib.h>

namespace Paralax {

	std::optional<std::string> GetProcessEnvironmentVariable(std::string_view name)
	{
		if (!IsValidEnvironmentVariableName(name))
			return std::nullopt;
		const std::string terminatedName(name);
		if (const char* value = getenv(terminatedName.c_str()))
			return std::string(value);
		return std::nullopt;
	}

	bool SetProcessEnvironmentVariable(std::string_view name, std::optional<std::string_view> value)
	{
		if (!IsValidEnvironmentVariableName(name) || (value && value->find('\0') != std::string_view::npos))
			return false;
		const std::string terminatedName(name);
		if (!value)
			return unsetenv(terminatedName.c_str()) == 0;
		const std::string terminatedValue(*value);
		return setenv(terminatedName.c_str(), terminatedValue.c_str(), 1) == 0;
	}

}

#endif
