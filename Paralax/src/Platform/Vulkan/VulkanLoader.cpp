#include "pxpch.h"
#include "Platform/Vulkan/VulkanLoader.h"

#include "Paralax/Core/DynamicLibrary.h"

#include <mutex>

namespace Paralax::VulkanLoader {

	namespace {

		std::vector<std::string> GetCandidateNames()
		{
#if defined(PX_PLATFORM_WINDOWS)
			return {"vulkan-1.dll"};
#elif defined(PX_PLATFORM_MACOS)
			// The Khronos loader first (Homebrew's prefix is not on dyld's default search path), then MoltenVK.
			return {"libvulkan.1.dylib", "libvulkan.dylib", "/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib", "libMoltenVK.dylib"};
#else
			return {"libvulkan.so.1", "libvulkan.so"};
#endif
		}

		struct LoaderState
		{
			DynamicLibrary Library;
			PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
			std::string Name;
		};

		LoaderState& GetState()
		{
			static LoaderState s_State;
			static std::once_flag s_Once;
			std::call_once(s_Once, [] {
				if (!s_State.Library.Load(GetCandidateNames()))
				{
					PX_CORE_ERROR("Vulkan is not available: no Vulkan loader found (install a GPU driver with Vulkan support)");
					return;
				}

				s_State.GetInstanceProcAddr = s_State.Library.GetFunction<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
				if (!s_State.GetInstanceProcAddr)
				{
					PX_CORE_ERROR("'{}' does not export vkGetInstanceProcAddr", s_State.Library.GetLoadedName());
					return;
				}
				s_State.Name = s_State.Library.GetLoadedName();
			});
			return s_State;
		}

	}

	PFN_vkGetInstanceProcAddr GetInstanceProcAddr()
	{
		return GetState().GetInstanceProcAddr;
	}

	const std::string& GetLibraryName()
	{
		return GetState().Name;
	}

}
