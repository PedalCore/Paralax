#pragma once

#include <vulkan/vulkan_core.h>

#include <string>

namespace Paralax::VulkanLoader {

	// Loads the system Vulkan loader once per process and keeps it loaded until exit. Both the graphics device and
	// GLFW (for window surfaces) use this one library, so they always agree on the loader. Thread-safe.
	// Returns nullptr (with an error logged on first failure) if no Vulkan loader is installed.
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr();

	// File name of the loaded library, or empty if none was loaded.
	const std::string& GetLibraryName();

}
