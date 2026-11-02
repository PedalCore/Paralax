#include "pxpch.h"
#include "Paralax/Core/CommandLine.h"

#if !defined(PX_PLATFORM_WINDOWS)

namespace Paralax {

	std::vector<std::string> GetProcessArguments(int argc, const char* const* argv)
	{
		// The bytes are the file names, in whatever encoding they were created with: converting them would name
		// other files. FromUtf8 passes them through unchanged, so UTF-8 or not they reach the file system as given.
		return CopyArgv(argc, argv);
	}

}

#endif
