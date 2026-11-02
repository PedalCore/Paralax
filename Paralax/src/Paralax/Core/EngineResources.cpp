#include "pxpch.h"
#include "Paralax/Core/EngineResources.h"

#include "Paralax/Core/Environment.h"
#include "Paralax/Core/PathEncoding.h"

namespace Paralax {

	std::filesystem::path GetEngineResourceDirectory()
	{
		// In the engine's encoding, so on Windows the directory may hold characters outside the ANSI code page (which
		// std::getenv would have lost). Safe alongside other readers, as nothing in the engine sets the environment.
		if (std::optional<std::string> overridden = GetProcessEnvironmentVariable("PARALAX_RESOURCE_DIR"); overridden && !overridden->empty())
			return FromUtf8(*overridden);
		const std::filesystem::path executable = GetExecutablePath();
		if (executable.empty())
			return {};
		return executable.parent_path() / "Resources";
	}

}
