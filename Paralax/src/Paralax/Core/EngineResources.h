#pragma once

#include <filesystem>

namespace Paralax {

	// Engine resources that ship next to the executables (e.g. built-in HDR environments), as opposed to project
	// assets. The build copies them to "<runtime output directory>/Resources"; project export copies them next to
	// the exported game.
	//
	// Returns the PARALAX_RESOURCE_DIR environment variable if set (tools, tests), otherwise "Resources" beside the
	// running executable, or an empty path if the executable's location cannot be determined. The directory is not
	// checked for existence: callers report missing files themselves. Thread-safe.
	std::filesystem::path GetEngineResourceDirectory();

	// Absolute path of the running executable, or an empty path if the platform cannot tell. Thread-safe.
	std::filesystem::path GetExecutablePath();

}
