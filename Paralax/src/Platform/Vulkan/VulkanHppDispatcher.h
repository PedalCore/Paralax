#pragma once

#include <vulkan/vulkan_core.h>

namespace Paralax::VulkanHpp {

	// Initialises vulkan.hpp's default dynamic dispatcher, which the statically linked NVRHI Vulkan backend uses
	// for every Vulkan call. Must be called after the VkDevice exists and before nvrhi::vulkan::createDevice.
	void InitDispatcher(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkDevice device);

}
