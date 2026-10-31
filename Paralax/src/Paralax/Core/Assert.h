#pragma once

#include "Paralax/Core/Config.h"

#include <spdlog/fmt/fmt.h>

#include <functional>
#include <string>

namespace Paralax {

	struct AssertionInfo
	{
		const char* Expression = "";
		const char* File = "";
		int Line = 0;
		std::string Message;
	};

	// Handles failed assertions. The default handler logs the failure, breaks into the debugger in
	// Debug builds and then aborts the process.
	//
	// A custom handler must not return normally: it either throws (tests use this, see Paralax-Tests'
	// ScopedAssertionCapture) or terminates. If it returns, the process is aborted, so code after a failed
	// assert/verify never runs. SetHandler, ResetHandler and Fail are thread-safe.
	class Assertion
	{
	public:
		using Handler = std::function<void(const AssertionInfo&)>;

		static void SetHandler(Handler handler);
		static void ResetHandler();
		static bool HasCustomHandler();

		// Invoked by the PX_*_ASSERT / PX_*_VERIFY macros; not intended to be called directly.
		[[noreturn]] static void Fail(const char* expression, const char* file, int line, std::string message = {});
	};

}

// PX_CORE_ASSERT (engine) / PX_ASSERT (client): checks a condition in Debug and Release builds;
// compiled out (and not evaluated) in Dist.
#if defined(PX_ENABLE_ASSERTS)
	#define PX_INTERNAL_ASSERT_IMPL(condition, ...)                                                                  \
		do                                                                                                           \
		{                                                                                                            \
			if (!(condition)) [[unlikely]]                                                                           \
				::Paralax::Assertion::Fail(#condition, __FILE__, __LINE__ __VA_OPT__(, ::fmt::format(__VA_ARGS__))); \
		} while (false)

	#define PX_CORE_ASSERT(condition, ...) PX_INTERNAL_ASSERT_IMPL(condition __VA_OPT__(, __VA_ARGS__))
	#define PX_ASSERT(condition, ...) PX_INTERNAL_ASSERT_IMPL(condition __VA_OPT__(, __VA_ARGS__))
#else
	#define PX_CORE_ASSERT(condition, ...) \
		do                                 \
		{                                  \
			(void)sizeof(!(condition));    \
		} while (false)
	#define PX_ASSERT(condition, ...) PX_CORE_ASSERT(condition)
#endif

// Like PX_*_ASSERT, but the condition is always evaluated and always checked, including in Dist builds.
// Use for conditions whose failure would otherwise lead to undefined behaviour or data loss.
#define PX_CORE_VERIFY(condition, ...)                                                                           \
	do                                                                                                           \
	{                                                                                                            \
		if (!(condition)) [[unlikely]]                                                                           \
			::Paralax::Assertion::Fail(#condition, __FILE__, __LINE__ __VA_OPT__(, ::fmt::format(__VA_ARGS__))); \
	} while (false)
#define PX_VERIFY(condition, ...) PX_CORE_VERIFY(condition __VA_OPT__(, __VA_ARGS__))
