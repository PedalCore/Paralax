#include "pxpch.h"
#include "Paralax/Core/Environment.h"

namespace Paralax {

	bool IsValidEnvironmentVariableName(std::string_view name)
	{
		// POSIX setenv refuses an empty name or one with '=', and so does the Windows C runtime; a NUL would end the
		// name early wherever it reaches the C library.
		constexpr std::string_view Forbidden("=\0", 2);
		return !name.empty() && name.find_first_of(Forbidden) == std::string_view::npos;
	}

}
