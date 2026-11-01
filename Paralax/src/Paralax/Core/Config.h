#pragma once

// Build configuration macros. Exactly one of PX_DEBUG, PX_RELEASE or PX_DIST is defined by the build system.

#include "Paralax/Core/PlatformDetection.h"

#if !defined(PX_DEBUG) && !defined(PX_RELEASE) && !defined(PX_DIST)
	#error "No Paralax build configuration defined (expected PX_DEBUG, PX_RELEASE or PX_DIST)."
#endif

#if defined(PX_DEBUG)
	#if defined(PX_PLATFORM_WINDOWS)
		#define PX_DEBUGBREAK() __debugbreak()
	#elif defined(__clang__)
		#define PX_DEBUGBREAK() __builtin_debugtrap()
	#elif defined(__GNUC__)
		#include <csignal>
		#define PX_DEBUGBREAK() std::raise(SIGTRAP)
	#endif
	#define PX_ENABLE_ASSERTS
#elif defined(PX_RELEASE)
	#define PX_DEBUGBREAK() ((void)0)
	#define PX_ENABLE_ASSERTS
#else
	#define PX_DEBUGBREAK() ((void)0)
#endif

// Marks a function as printf-like (format string at formatIndex, variadic arguments from firstArgument; for a
// va_list-forwarding function pass 0), so the compiler checks callers instead of warning inside the function.
#if defined(__GNUC__) || defined(__clang__)
	#define PX_PRINTF_FORMAT(formatIndex, firstArgument) __attribute__((format(printf, formatIndex, firstArgument)))
#else
	#define PX_PRINTF_FORMAT(formatIndex, firstArgument)
#endif
