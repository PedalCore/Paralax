#include "pxpch.h"
#include "Paralax/Core/EngineResources.h"

#if !defined(PX_PLATFORM_WINDOWS)

	#if defined(PX_PLATFORM_MACOS)
		#include <mach-o/dyld.h>
	#endif

	#include <cstdint>
	#include <vector>

namespace Paralax {

	std::filesystem::path GetExecutablePath()
	{
		std::error_code error;
	#if defined(PX_PLATFORM_MACOS)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size); // Reports the required size.
		std::vector<char> buffer(size + 1, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			return {};
		// The result may contain symlinks or "..": canonicalize it.
		std::filesystem::path path = std::filesystem::weakly_canonical(buffer.data(), error);
	#else
		std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
	#endif
		return error ? std::filesystem::path() : path;
	}

}

#endif
