#include "pxpch.h"
#include "Platform/Vulkan/VulkanHppDispatcher.h"

// Must match the configuration NVRHI's Vulkan backend compiles vulkan.hpp with (src/vulkan/vulkan-backend.h).
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

// The one definition of the default dispatcher in the program. NVRHI only defines it in shared-library builds.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Paralax::VulkanHpp {

	void InitDispatcher(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkDevice device)
	{
		VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance));
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device));
	}

}
