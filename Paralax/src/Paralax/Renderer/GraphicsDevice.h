#pragma once

#include "Paralax/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Paralax {

	struct GraphicsDeviceSpecification
	{
		// Enables the Khronos validation layer (when installed) and NVRHI's validation layer. Validation errors are
		// logged and counted (see GetValidationErrorCount).
		bool EnableValidation = false;
		// With EnableValidation: also enables the layer's synchronization validation, which reports GPU work that
		// reads or writes a resource without a barrier ordering it after the previous access. NVRHI re-places
		// barriers for a draw or dispatch only when one of its binding sets changed or a copy, clear or write preceded
		// it, so this is what catches a missed one. Costs recording time; tests and --validation keep it on. Ignored
		// when the layer lacks VK_EXT_validation_features.
		bool EnableSynchronizationValidation = true;
		// When true, no window-system (surface/swapchain) extensions are requested; rendering is offscreen only.
		bool Headless = false;
		// When non-empty, the first adapter whose name contains this string (case-insensitive) is preferred.
		// The PX_GPU environment variable overrides this.
		std::string PreferredAdapter;
		// Additional instance extensions, e.g. those the windowing system requires for surfaces.
		std::vector<std::string> InstanceExtensions;
	};

	// Numeric resource limits reported by the selected adapter. Defaults are Vulkan 1.3 core guarantees.
	struct GraphicsDeviceLimits
	{
		uint64_t MaxStorageBufferBytes = 128u * 1024 * 1024;
		uint32_t MaxTextureArrayLayers = 256;
		uint32_t MaxStorageBuffersPerStage = 4;
		uint32_t MaxStorageBuffersPerSet = 24;
	};

	struct GraphicsDeviceInfo
	{
		GraphicsDeviceLimits Limits;
		std::string AdapterName;
		std::string DriverName;
		uint32_t VendorID = 0;
		uint32_t ApiVersionMajor = 0;
		uint32_t ApiVersionMinor = 0;
		bool IsSoftwareRasterizer = false;
		// The Khronos layer's synchronization validation is active (see GraphicsDeviceSpecification).
		bool SynchronizationValidation = false;
		bool SupportsRayQuery = false;
		bool SupportsRayTracingPipeline = false;
		bool SupportsMeshShaders = false;
	};

	// Owns the native graphics API objects (Vulkan instance, physical and logical device, queues) and the NVRHI
	// device built on top of them. Created and destroyed on the main thread; GetDevice() follows NVRHI's
	// threading rules. Only one GraphicsDevice may exist at a time (Create fails while another exists).
	//
	// Every NVRHI handle created from the device must be released before the device is destroyed; destroying it
	// while handles are alive is verified as a fatal error.
	class GraphicsDevice
	{
	public:
		virtual ~GraphicsDevice() = default;

		// Returns nullptr (with the reason logged) if no suitable device is available, e.g. no Vulkan loader or no
		// adapter supporting Vulkan 1.3 with the features Paralax requires.
		static Scope<GraphicsDevice> Create(const GraphicsDeviceSpecification& specification);

		virtual nvrhi::IDevice* GetDevice() const = 0;
		// The API-specific NVRHI device without the validation wrapper (nvrhi::vulkan::IDevice). For platform code
		// that needs backend-only operations such as queue semaphores; everything else uses GetDevice().
		virtual nvrhi::IDevice* GetBackendDevice() const = 0;
		virtual const GraphicsDeviceInfo& GetInfo() const = 0;

		// Number of validation errors reported since creation (always 0 when validation is disabled).
		virtual uint32_t GetValidationErrorCount() const = 0;

		// Blocks until the GPU has finished all submitted work and releases resources whose use has completed.
		virtual void WaitForIdle() = 0;
		// Releases resources whose GPU use has completed. Call once per frame.
		virtual void CollectGarbage() = 0;

		// Native handles for platform code (swapchain creation). Types are VkInstance/VkPhysicalDevice/VkDevice.
		virtual void* GetNativeInstance() const = 0;
		virtual void* GetNativePhysicalDevice() const = 0;
		virtual void* GetNativeDevice() const = 0;
		virtual void* GetNativeGraphicsQueue() const = 0;
		virtual uint32_t GetGraphicsQueueFamily() const = 0;
		// Resolves a Vulkan function for platform code (instance-level when the device lookup fails).
		virtual void* GetVulkanProcAddress(const char* name) const = 0;
	};

}
