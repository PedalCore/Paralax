#pragma once

// Platform detection. Exactly one of PX_PLATFORM_WINDOWS, PX_PLATFORM_MACOS or PX_PLATFORM_LINUX is defined.

#if defined(_WIN32)
	#if defined(_WIN64)
		#define PX_PLATFORM_WINDOWS
	#else
		#error "Paralax requires a 64-bit Windows target."
	#endif
#elif defined(__APPLE__) && defined(__MACH__)
	#include <TargetConditionals.h>
	#if TARGET_OS_OSX == 1
		#define PX_PLATFORM_MACOS
	#else
		#error "Only macOS is supported among Apple platforms."
	#endif
#elif defined(__linux__)
	#define PX_PLATFORM_LINUX
#else
	#error "Unsupported platform. Paralax supports Windows, macOS and Linux."
#endif
