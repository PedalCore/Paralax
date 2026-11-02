#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Paralax {

	// The process environment, read and written in the engine's string encoding (Core/PathEncoding.h), so a value can
	// name a path through FromUtf8 whatever characters it holds. Windows keeps the environment in UTF-16, and these
	// convert from and to UTF-8; on POSIX the bytes pass through unchanged. Names are case-sensitive on POSIX and not on
	// Windows.
	//
	// Not named Get/SetEnvironmentVariable: <Windows.h> defines those as macros, which would rename the functions only
	// in the translation units that include it.
	//
	// Threading: the environment is process-wide state, which the C library reads (std::getenv anywhere in the process,
	// third-party libraries included) without synchronising with changes to it. GetProcessEnvironmentVariable may run on
	// any thread alongside other reads; SetProcessEnvironmentVariable only while no other thread reads or writes the
	// environment. The engine itself only reads it: setting variables is for tests and tools, before other threads
	// start.

	// True if the name can be set on every platform: not empty, and holding no '=' (which ends a name in the
	// environment block) and no NUL. Pure; safe to call from any thread.
	bool IsValidEnvironmentVariableName(std::string_view name);

	// The variable's value, or std::nullopt if it is not set, the name is not valid, or on Windows the name is 32,767
	// UTF-16 units or longer (the C runtime's limit, so no such variable can be set). On Windows each unpaired surrogate
	// in the value becomes U+FFFD.
	std::optional<std::string> GetProcessEnvironmentVariable(std::string_view name);

	// Sets the variable to value, or with std::nullopt removes it (removing an unset variable succeeds). Returns false,
	// changing nothing, if the name is not valid (IsValidEnvironmentVariableName), the value holds a NUL, or on Windows
	// the name or the value is 32,767 UTF-16 units or longer (the C runtime's limit); also false if the platform refuses
	// the change, as when it runs out of memory.
	//
	// The change reaches std::getenv and child processes started afterwards. On Windows that takes the C runtime's own
	// copies of the environment as well as the process's block, which SetEnvironmentVariableW alone would leave stale
	// for getenv, and std::getenv sees the value in the ANSI code page, without the characters it lacks:
	// GetProcessEnvironmentVariable reads it exactly. Also on Windows, malformed UTF-8 is set with U+FFFD in place of
	// each invalid sequence, and an empty value removes the variable, because the C runtime cannot hold one: treat empty
	// and unset alike, as the engine does.
	bool SetProcessEnvironmentVariable(std::string_view name, std::optional<std::string_view> value);

}
