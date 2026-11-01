#include "pxpch.h"
#include "Paralax/Core/EngineResources.h"

#include <cstdlib>

namespace Paralax {

	std::filesystem::path GetEngineResourceDirectory()
	{
		// std::getenv is safe for concurrent readers as long as nothing modifies the environment, which the engine
		// never does.
		if (const char* overridden = std::getenv("PARALAX_RESOURCE_DIR"); overridden && *overridden)
			return std::filesystem::path(overridden);
		const std::filesystem::path executable = GetExecutablePath();
		if (executable.empty())
			return {};
		return executable.parent_path() / "Resources";
	}

}
