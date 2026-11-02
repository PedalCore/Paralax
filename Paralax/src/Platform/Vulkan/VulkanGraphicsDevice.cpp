#include "pxpch.h"
#include "Paralax/Renderer/GraphicsDevice.h"

#include "Paralax/Core/DynamicLibrary.h"
#include "Platform/Vulkan/VulkanHppDispatcher.h"
#include "Platform/Vulkan/VulkanLoader.h"

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

// Vulkan is only called through function pointers loaded at runtime (see VulkanFunctions); never link the loader.
#include <vulkan/vulkan.h>

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Paralax {

	namespace {

		// Zero-initialises a Vulkan structure and sets its sType.
		template<typename T>
		T MakeVkStruct(VkStructureType type)
		{
			T value{};
			value.sType = type;
			return value;
		}

		std::string ToLower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		bool ContainsExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
		{
			return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
		}

		const char* ResultToString(VkResult result)
		{
			switch (result)
			{
				case VK_SUCCESS: return "VK_SUCCESS";
				case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
				case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
				case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
				case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
				case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
				case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
				case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
				case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
				default: return "VkResult error";
			}
		}

		// Vulkan entry points used by device creation. Loaded at runtime so building needs no Vulkan SDK.
		struct VulkanFunctions
		{
			PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
			PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
			PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
			PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
			PFN_vkCreateInstance CreateInstance = nullptr;

			PFN_vkDestroyInstance DestroyInstance = nullptr;
			PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
			PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
			PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
			PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
			PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
			PFN_vkCreateDevice CreateDevice = nullptr;
			PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
			PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT = nullptr;
			PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT = nullptr;

			PFN_vkDestroyDevice DestroyDevice = nullptr;
			PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
			PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
		};

		template<typename T>
		void LoadInstanceFunction(const VulkanFunctions& functions, VkInstance instance, const char* name, T& out)
		{
			out = reinterpret_cast<T>(functions.GetInstanceProcAddr(instance, name));
		}

		// Features Paralax requires (R) or uses when available (optional). Kept in one place so device selection
		// and device creation agree.
		struct FeatureChain
		{
			VkPhysicalDeviceFeatures2 Features2 = MakeVkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
			VkPhysicalDeviceVulkan11Features Vulkan11 = MakeVkStruct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
			VkPhysicalDeviceVulkan12Features Vulkan12 = MakeVkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
			VkPhysicalDeviceVulkan13Features Vulkan13 = MakeVkStruct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);

			FeatureChain()
			{
				Features2.pNext = &Vulkan11;
				Vulkan11.pNext = &Vulkan12;
				Vulkan12.pNext = &Vulkan13;
				Vulkan13.pNext = nullptr;
			}

			FeatureChain(const FeatureChain&) = delete;
			FeatureChain& operator=(const FeatureChain&) = delete;
		};

		bool HasRequiredFeatures(const FeatureChain& supported, std::string& outMissing)
		{
			std::vector<const char*> missing;
			if (!supported.Vulkan13.dynamicRendering)
				missing.push_back("dynamicRendering");
			if (!supported.Vulkan13.synchronization2)
				missing.push_back("synchronization2");
			if (!supported.Vulkan12.timelineSemaphore)
				missing.push_back("timelineSemaphore");
			outMissing = fmt::format("{}", fmt::join(missing, ", "));
			return missing.empty();
		}

		// Copies every required and supported optional feature into `enabled`.
		void SelectFeatures(const FeatureChain& supported, FeatureChain& enabled)
		{
			const VkPhysicalDeviceFeatures& s10 = supported.Features2.features;
			VkPhysicalDeviceFeatures& e10 = enabled.Features2.features;
			e10.samplerAnisotropy = s10.samplerAnisotropy;
			e10.fillModeNonSolid = s10.fillModeNonSolid;
			e10.independentBlend = s10.independentBlend;
			e10.imageCubeArray = s10.imageCubeArray;
			e10.depthClamp = s10.depthClamp;
			e10.multiDrawIndirect = s10.multiDrawIndirect;
			e10.drawIndirectFirstInstance = s10.drawIndirectFirstInstance;
			e10.textureCompressionBC = s10.textureCompressionBC;
			e10.shaderInt16 = s10.shaderInt16;
			e10.shaderInt64 = s10.shaderInt64;
			e10.fragmentStoresAndAtomics = s10.fragmentStoresAndAtomics;
			e10.shaderStorageImageWriteWithoutFormat = s10.shaderStorageImageWriteWithoutFormat;
			e10.shaderStorageImageReadWithoutFormat = s10.shaderStorageImageReadWithoutFormat;

			enabled.Vulkan11.shaderDrawParameters = supported.Vulkan11.shaderDrawParameters;
			enabled.Vulkan11.storageBuffer16BitAccess = supported.Vulkan11.storageBuffer16BitAccess;

			const VkPhysicalDeviceVulkan12Features& s12 = supported.Vulkan12;
			VkPhysicalDeviceVulkan12Features& e12 = enabled.Vulkan12;
			e12.timelineSemaphore = VK_TRUE;
			e12.bufferDeviceAddress = s12.bufferDeviceAddress;
			e12.descriptorIndexing = s12.descriptorIndexing;
			e12.runtimeDescriptorArray = s12.runtimeDescriptorArray;
			e12.descriptorBindingPartiallyBound = s12.descriptorBindingPartiallyBound;
			e12.descriptorBindingVariableDescriptorCount = s12.descriptorBindingVariableDescriptorCount;
			e12.shaderSampledImageArrayNonUniformIndexing = s12.shaderSampledImageArrayNonUniformIndexing;
			e12.shaderStorageBufferArrayNonUniformIndexing = s12.shaderStorageBufferArrayNonUniformIndexing;
			e12.descriptorBindingSampledImageUpdateAfterBind = s12.descriptorBindingSampledImageUpdateAfterBind;
			e12.descriptorBindingStorageImageUpdateAfterBind = s12.descriptorBindingStorageImageUpdateAfterBind;
			e12.descriptorBindingStorageBufferUpdateAfterBind = s12.descriptorBindingStorageBufferUpdateAfterBind;
			e12.descriptorBindingUpdateUnusedWhilePending = s12.descriptorBindingUpdateUnusedWhilePending;
			e12.scalarBlockLayout = s12.scalarBlockLayout;
			e12.shaderFloat16 = s12.shaderFloat16;
			e12.drawIndirectCount = s12.drawIndirectCount;
			e12.samplerFilterMinmax = s12.samplerFilterMinmax;
			e12.hostQueryReset = s12.hostQueryReset;
			e12.storageBuffer8BitAccess = s12.storageBuffer8BitAccess;

			enabled.Vulkan13.dynamicRendering = VK_TRUE;
			enabled.Vulkan13.synchronization2 = VK_TRUE;
			enabled.Vulkan13.maintenance4 = supported.Vulkan13.maintenance4;
			enabled.Vulkan13.shaderDemoteToHelperInvocation = supported.Vulkan13.shaderDemoteToHelperInvocation;
		}

		// Forwards NVRHI's diagnostics to the engine log and counts errors.
		class NvrhiMessageCallback : public nvrhi::IMessageCallback
		{
		public:
			explicit NvrhiMessageCallback(std::atomic<uint32_t>& errorCount)
				: m_ErrorCount(errorCount)
			{
			}

			void message(nvrhi::MessageSeverity severity, const char* messageText) override
			{
				switch (severity)
				{
					case nvrhi::MessageSeverity::Info: PX_CORE_TRACE("[NVRHI] {}", messageText); break;
					case nvrhi::MessageSeverity::Warning: PX_CORE_WARN("[NVRHI] {}", messageText); break;
					case nvrhi::MessageSeverity::Error:
					case nvrhi::MessageSeverity::Fatal:
						m_ErrorCount++;
						PX_CORE_ERROR("[NVRHI] {}", messageText);
						break;
				}
			}

		private:
			std::atomic<uint32_t>& m_ErrorCount;
		};

		VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
														  [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
		{
			auto* errorCount = static_cast<std::atomic<uint32_t>*>(userData);
			const char* message = data && data->pMessage ? data->pMessage : "(no message)";

			if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
			{
				(*errorCount)++;
				PX_CORE_ERROR("[Vulkan] {}", message);
			}
			else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
				PX_CORE_WARN("[Vulkan] {}", message);
			else
				PX_CORE_TRACE("[Vulkan] {}", message);

			return VK_FALSE;
		}

	}

	class VulkanGraphicsDevice : public GraphicsDevice
	{
	public:
		VulkanGraphicsDevice() = default;
		~VulkanGraphicsDevice() override;

		VulkanGraphicsDevice(const VulkanGraphicsDevice&) = delete;
		VulkanGraphicsDevice& operator=(const VulkanGraphicsDevice&) = delete;

		bool Init(const GraphicsDeviceSpecification& specification);

		nvrhi::IDevice* GetDevice() const override { return m_Device.Get(); }
		nvrhi::IDevice* GetBackendDevice() const override { return m_BackendDevice.Get(); }
		const GraphicsDeviceInfo& GetInfo() const override { return m_Info; }
		uint32_t GetValidationErrorCount() const override { return m_ValidationErrorCount.load(); }

		void WaitForIdle() override
		{
			if (m_Device)
				m_Device->waitForIdle();
		}

		void CollectGarbage() override
		{
			if (m_Device)
				m_Device->runGarbageCollection();
		}

		void* GetNativeInstance() const override { return m_Instance; }
		void* GetNativePhysicalDevice() const override { return m_PhysicalDevice; }
		void* GetNativeDevice() const override { return m_VkDevice; }
		void* GetNativeGraphicsQueue() const override { return m_GraphicsQueue; }
		uint32_t GetGraphicsQueueFamily() const override { return m_GraphicsQueueFamily; }

		void* GetVulkanProcAddress(const char* name) const override
		{
			if (m_VkDevice && m_Functions.GetDeviceProcAddr)
			{
				if (PFN_vkVoidFunction function = m_Functions.GetDeviceProcAddr(m_VkDevice, name))
					return reinterpret_cast<void*>(function);
			}
			return m_Functions.GetInstanceProcAddr ? reinterpret_cast<void*>(m_Functions.GetInstanceProcAddr(m_Instance, name)) : nullptr;
		}

	private:
		bool LoadLoader();
		bool CreateInstance(const GraphicsDeviceSpecification& specification);
		bool SelectPhysicalDevice(const GraphicsDeviceSpecification& specification);
		bool CreateLogicalDevice(const GraphicsDeviceSpecification& specification);
		bool CreateNvrhiDevice(const GraphicsDeviceSpecification& specification);

	private:
		VulkanFunctions m_Functions;

		VkInstance m_Instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_VkDevice = VK_NULL_HANDLE;
		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		uint32_t m_GraphicsQueueFamily = 0;

		std::vector<std::string> m_EnabledInstanceExtensions;
		std::vector<std::string> m_EnabledDeviceExtensions;
		bool m_BufferDeviceAddress = false;
		bool m_UniformBufferUpdateAfterBind = false;

		bool m_OwnsDeviceSlot = false;
		GraphicsDeviceInfo m_Info;
		std::atomic<uint32_t> m_ValidationErrorCount = 0;
		Scope<NvrhiMessageCallback> m_MessageCallback;
		nvrhi::DeviceHandle m_BackendDevice;
		nvrhi::DeviceHandle m_Device;
	};

	namespace {

		// NVRHI's static Vulkan backend dispatches through vulkan.hpp's single global dispatcher, so only one device
		// can exist at a time: creating a second would repoint the first device's function table.
		bool s_DeviceExists = false;

	}

	Scope<GraphicsDevice> GraphicsDevice::Create(const GraphicsDeviceSpecification& specification)
	{
		if (s_DeviceExists)
		{
			PX_CORE_ERROR("A GraphicsDevice already exists; only one may exist at a time");
			return nullptr;
		}

		auto device = CreateScope<VulkanGraphicsDevice>();
		if (!device->Init(specification))
			return nullptr;
		return device;
	}

	VulkanGraphicsDevice::~VulkanGraphicsDevice()
	{
		if (m_OwnsDeviceSlot)
			s_DeviceExists = false;

		if (m_Device)
		{
			m_Device->waitForIdle();
			m_Device = nullptr;
		}
		if (m_BackendDevice)
		{
			// NVRHI resources reference the device; any handle that outlives it would be a use-after-free.
			const unsigned long remaining = m_BackendDevice.Reset();
			PX_CORE_VERIFY(remaining == 0, "GraphicsDevice destroyed while {} NVRHI reference(s) are still alive", remaining);
		}

		if (m_VkDevice && m_Functions.DestroyDevice)
		{
			if (m_Functions.DeviceWaitIdle)
				m_Functions.DeviceWaitIdle(m_VkDevice);
			m_Functions.DestroyDevice(m_VkDevice, nullptr);
		}

		if (m_DebugMessenger && m_Functions.DestroyDebugUtilsMessengerEXT)
			m_Functions.DestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);

		if (m_Instance && m_Functions.DestroyInstance)
			m_Functions.DestroyInstance(m_Instance, nullptr);
	}

	bool VulkanGraphicsDevice::Init(const GraphicsDeviceSpecification& specification)
	{
		s_DeviceExists = true;
		m_OwnsDeviceSlot = true;
		m_MessageCallback = CreateScope<NvrhiMessageCallback>(m_ValidationErrorCount);

		return LoadLoader() && CreateInstance(specification) && SelectPhysicalDevice(specification) && CreateLogicalDevice(specification) && CreateNvrhiDevice(specification);
	}

	bool VulkanGraphicsDevice::LoadLoader()
	{
		m_Functions.GetInstanceProcAddr = VulkanLoader::GetInstanceProcAddr();
		if (!m_Functions.GetInstanceProcAddr)
			return false;

		LoadInstanceFunction(m_Functions, VK_NULL_HANDLE, "vkEnumerateInstanceVersion", m_Functions.EnumerateInstanceVersion);
		LoadInstanceFunction(m_Functions, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties", m_Functions.EnumerateInstanceExtensionProperties);
		LoadInstanceFunction(m_Functions, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties", m_Functions.EnumerateInstanceLayerProperties);
		LoadInstanceFunction(m_Functions, VK_NULL_HANDLE, "vkCreateInstance", m_Functions.CreateInstance);

		if (!m_Functions.EnumerateInstanceVersion || !m_Functions.EnumerateInstanceExtensionProperties || !m_Functions.CreateInstance)
		{
			PX_CORE_ERROR("The Vulkan loader '{}' is too old (Vulkan 1.1+ loader required)", VulkanLoader::GetLibraryName());
			return false;
		}

		return true;
	}

	bool VulkanGraphicsDevice::CreateInstance(const GraphicsDeviceSpecification& specification)
	{
		uint32_t loaderVersion = 0;
		m_Functions.EnumerateInstanceVersion(&loaderVersion);
		if (loaderVersion < VK_API_VERSION_1_3)
		{
			PX_CORE_ERROR("Vulkan 1.3 is required; the installed loader supports {}.{}", VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
			return false;
		}

		uint32_t extensionCount = 0;
		m_Functions.EnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> available(extensionCount);
		m_Functions.EnumerateInstanceExtensionProperties(nullptr, &extensionCount, available.data());

		for (const std::string& extension : specification.InstanceExtensions)
		{
			if (!ContainsExtension(available, extension.c_str()))
			{
				PX_CORE_ERROR("Required Vulkan instance extension '{}' is not available", extension);
				return false;
			}
			m_EnabledInstanceExtensions.push_back(extension);
		}

		VkInstanceCreateFlags flags = 0;
		if (ContainsExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
		{
			// Required to see portability (non-conformant) implementations such as MoltenVK.
			m_EnabledInstanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}

		std::vector<const char*> layers;
		const bool debugUtilsAvailable = ContainsExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		bool synchronizationValidation = false;
		if (specification.EnableValidation)
		{
			uint32_t layerCount = 0;
			if (m_Functions.EnumerateInstanceLayerProperties)
				m_Functions.EnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> availableLayers(layerCount);
			if (layerCount > 0)
				m_Functions.EnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

			const bool hasValidationLayer = std::any_of(availableLayers.begin(), availableLayers.end(),
														[](const VkLayerProperties& l) { return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
			if (hasValidationLayer)
			{
				layers.push_back("VK_LAYER_KHRONOS_validation");
				if (specification.EnableSynchronizationValidation)
				{
					// Extensions a layer provides are listed only when enumerating with that layer's name. The newer
					// VK_EXT_layer_settings is not offered by the layer Ubuntu 24.04 ships (1.3.275), so the deprecated but
					// universally supported VK_EXT_validation_features is the one to ask for.
					uint32_t layerExtensionCount = 0;
					m_Functions.EnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerExtensionCount, nullptr);
					std::vector<VkExtensionProperties> layerExtensions(layerExtensionCount);
					if (layerExtensionCount > 0)
						m_Functions.EnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerExtensionCount, layerExtensions.data());
					if (ContainsExtension(layerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
					{
						m_EnabledInstanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
						synchronizationValidation = true;
					}
					else
						PX_CORE_WARN("Vulkan synchronization validation requested but the validation layer lacks VK_EXT_validation_features");
				}
			}
			else
				PX_CORE_WARN("Vulkan validation requested but VK_LAYER_KHRONOS_validation is not installed; using NVRHI validation only");

			if (debugUtilsAvailable)
				m_EnabledInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}

		std::vector<const char*> extensionNames;
		for (const std::string& extension : m_EnabledInstanceExtensions)
			extensionNames.push_back(extension.c_str());

		VkApplicationInfo applicationInfo = MakeVkStruct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
		applicationInfo.pApplicationName = "Paralax";
		applicationInfo.pEngineName = "Paralax";
		applicationInfo.apiVersion = VK_API_VERSION_1_3;

		VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeVkStruct<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
		messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messengerInfo.pfnUserCallback = DebugUtilsCallback;
		messengerInfo.pUserData = &m_ValidationErrorCount;

		const bool useMessenger = specification.EnableValidation && debugUtilsAvailable;

		const VkValidationFeatureEnableEXT enabledValidationFeatures[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
		VkValidationFeaturesEXT validationFeatures = MakeVkStruct<VkValidationFeaturesEXT>(VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT);
		validationFeatures.enabledValidationFeatureCount = 1;
		validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures;
		// Both structures extend VkInstanceCreateInfo. The messenger structure is reused for
		// vkCreateDebugUtilsMessengerEXT below, which requires its own pNext to be null, so it ends the chain.
		validationFeatures.pNext = useMessenger ? &messengerInfo : nullptr;
		m_Info.SynchronizationValidation = synchronizationValidation;

		VkInstanceCreateInfo createInfo = MakeVkStruct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
		// The messenger in the chain also captures messages from instance creation itself.
		createInfo.pNext = synchronizationValidation ? static_cast<const void*>(&validationFeatures) : (useMessenger ? static_cast<const void*>(&messengerInfo) : nullptr);
		createInfo.flags = flags;
		createInfo.pApplicationInfo = &applicationInfo;
		createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
		createInfo.ppEnabledLayerNames = layers.data();
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
		createInfo.ppEnabledExtensionNames = extensionNames.data();

		const VkResult result = m_Functions.CreateInstance(&createInfo, nullptr, &m_Instance);
		if (result != VK_SUCCESS)
		{
			PX_CORE_ERROR("vkCreateInstance failed: {}", ResultToString(result));
			m_Instance = VK_NULL_HANDLE;
			return false;
		}

		LoadInstanceFunction(m_Functions, m_Instance, "vkDestroyInstance", m_Functions.DestroyInstance);
		if (!m_Functions.DestroyInstance)
		{
			// Without vkDestroyInstance the instance cannot be released; this loader is unusable.
			PX_CORE_ERROR("Failed to load vkDestroyInstance");
			return false;
		}
		LoadInstanceFunction(m_Functions, m_Instance, "vkEnumeratePhysicalDevices", m_Functions.EnumeratePhysicalDevices);
		LoadInstanceFunction(m_Functions, m_Instance, "vkGetPhysicalDeviceProperties2", m_Functions.GetPhysicalDeviceProperties2);
		LoadInstanceFunction(m_Functions, m_Instance, "vkGetPhysicalDeviceFeatures2", m_Functions.GetPhysicalDeviceFeatures2);
		LoadInstanceFunction(m_Functions, m_Instance, "vkGetPhysicalDeviceQueueFamilyProperties", m_Functions.GetPhysicalDeviceQueueFamilyProperties);
		LoadInstanceFunction(m_Functions, m_Instance, "vkEnumerateDeviceExtensionProperties", m_Functions.EnumerateDeviceExtensionProperties);
		LoadInstanceFunction(m_Functions, m_Instance, "vkCreateDevice", m_Functions.CreateDevice);
		LoadInstanceFunction(m_Functions, m_Instance, "vkGetDeviceProcAddr", m_Functions.GetDeviceProcAddr);
		LoadInstanceFunction(m_Functions, m_Instance, "vkCreateDebugUtilsMessengerEXT", m_Functions.CreateDebugUtilsMessengerEXT);
		LoadInstanceFunction(m_Functions, m_Instance, "vkDestroyDebugUtilsMessengerEXT", m_Functions.DestroyDebugUtilsMessengerEXT);

		if (!m_Functions.DestroyInstance || !m_Functions.EnumeratePhysicalDevices || !m_Functions.GetPhysicalDeviceProperties2 || !m_Functions.GetPhysicalDeviceFeatures2 || !m_Functions.GetPhysicalDeviceQueueFamilyProperties || !m_Functions.EnumerateDeviceExtensionProperties || !m_Functions.CreateDevice || !m_Functions.GetDeviceProcAddr)
		{
			PX_CORE_ERROR("Failed to load Vulkan instance functions");
			return false;
		}

		if (useMessenger && m_Functions.CreateDebugUtilsMessengerEXT)
			m_Functions.CreateDebugUtilsMessengerEXT(m_Instance, &messengerInfo, nullptr, &m_DebugMessenger);

		return true;
	}

	bool VulkanGraphicsDevice::SelectPhysicalDevice(const GraphicsDeviceSpecification& specification)
	{
		uint32_t count = 0;
		m_Functions.EnumeratePhysicalDevices(m_Instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		if (count > 0)
			m_Functions.EnumeratePhysicalDevices(m_Instance, &count, devices.data());

		if (devices.empty())
		{
			PX_CORE_ERROR("No Vulkan devices found");
			return false;
		}

		std::string preferred = specification.PreferredAdapter;
		if (const char* environment = std::getenv("PX_GPU"); environment && *environment)
			preferred = environment;
		preferred = ToLower(preferred);

		int bestScore = -1;
		for (VkPhysicalDevice device : devices)
		{
			VkPhysicalDeviceProperties2 properties = MakeVkStruct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
			m_Functions.GetPhysicalDeviceProperties2(device, &properties);
			const VkPhysicalDeviceProperties& props = properties.properties;

			std::string reason;
			if (props.apiVersion < VK_API_VERSION_1_3)
				reason = fmt::format("supports Vulkan {}.{} (1.3 required)", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));

			FeatureChain supported;
			if (reason.empty())
			{
				m_Functions.GetPhysicalDeviceFeatures2(device, &supported.Features2);
				std::string missing;
				if (!HasRequiredFeatures(supported, missing))
					reason = "missing features: " + missing;
			}

			uint32_t familyCount = 0;
			m_Functions.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			m_Functions.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

			int graphicsFamily = -1;
			for (uint32_t i = 0; i < familyCount; i++)
			{
				if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
				{
					graphicsFamily = static_cast<int>(i);
					break;
				}
			}
			if (reason.empty() && graphicsFamily < 0)
				reason = "no graphics+compute queue";

			uint32_t extensionCount = 0;
			m_Functions.EnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> extensions(extensionCount);
			m_Functions.EnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
			if (reason.empty() && !specification.Headless && !ContainsExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
				reason = "no swapchain support";

			if (!reason.empty())
			{
				PX_CORE_INFO("Skipping Vulkan device '{}': {}", props.deviceName, reason);
				continue;
			}

			int score = 0;
			switch (props.deviceType)
			{
				case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 400; break;
				case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 300; break;
				case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 200; break;
				case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 100; break;
				default: score = 50; break;
			}
			if (!preferred.empty() && ToLower(props.deviceName).find(preferred) != std::string::npos)
				score += 10000;

			if (score > bestScore)
			{
				bestScore = score;
				m_PhysicalDevice = device;
				m_GraphicsQueueFamily = static_cast<uint32_t>(graphicsFamily);
				m_Info.AdapterName = props.deviceName;
				m_Info.VendorID = props.vendorID;
				m_Info.ApiVersionMajor = VK_API_VERSION_MAJOR(props.apiVersion);
				m_Info.ApiVersionMinor = VK_API_VERSION_MINOR(props.apiVersion);
				m_Info.IsSoftwareRasterizer = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
			}
		}

		if (!m_PhysicalDevice)
		{
			PX_CORE_ERROR("No Vulkan device meets Paralax's requirements (Vulkan 1.3 with dynamic rendering, synchronization2 and timeline semaphores)");
			return false;
		}

		if (!preferred.empty() && ToLower(m_Info.AdapterName).find(preferred) == std::string::npos)
			PX_CORE_WARN("Preferred adapter '{}' not found; using '{}'", preferred, m_Info.AdapterName);

		return true;
	}

	bool VulkanGraphicsDevice::CreateLogicalDevice(const GraphicsDeviceSpecification& specification)
	{
		uint32_t extensionCount = 0;
		m_Functions.EnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> available(extensionCount);
		m_Functions.EnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extensionCount, available.data());

		if (!specification.Headless)
			m_EnabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		// Portability implementations (MoltenVK) require this extension to be enabled when it is exposed.
		if (ContainsExtension(available, "VK_KHR_portability_subset"))
			m_EnabledDeviceExtensions.push_back("VK_KHR_portability_subset");

		m_Info.SupportsRayQuery = ContainsExtension(available, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && ContainsExtension(available, VK_KHR_RAY_QUERY_EXTENSION_NAME);
		m_Info.SupportsRayTracingPipeline = ContainsExtension(available, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
		m_Info.SupportsMeshShaders = ContainsExtension(available, VK_EXT_MESH_SHADER_EXTENSION_NAME);

		FeatureChain supported;
		m_Functions.GetPhysicalDeviceFeatures2(m_PhysicalDevice, &supported.Features2);
		FeatureChain enabled;
		SelectFeatures(supported, enabled);
		m_BufferDeviceAddress = enabled.Vulkan12.bufferDeviceAddress == VK_TRUE;
		m_UniformBufferUpdateAfterBind = false;

		VkPhysicalDeviceProperties2 properties = MakeVkStruct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
		VkPhysicalDeviceDriverProperties driverProperties = MakeVkStruct<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
		properties.pNext = &driverProperties;
		m_Functions.GetPhysicalDeviceProperties2(m_PhysicalDevice, &properties);
		m_Info.DriverName = fmt::format("{} {}", driverProperties.driverName, driverProperties.driverInfo);

		const float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo = MakeVkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
		queueInfo.queueFamilyIndex = m_GraphicsQueueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;

		std::vector<const char*> extensionNames;
		for (const std::string& extension : m_EnabledDeviceExtensions)
			extensionNames.push_back(extension.c_str());

		VkDeviceCreateInfo createInfo = MakeVkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
		createInfo.pNext = &enabled.Features2;
		createInfo.queueCreateInfoCount = 1;
		createInfo.pQueueCreateInfos = &queueInfo;
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
		createInfo.ppEnabledExtensionNames = extensionNames.data();

		const VkResult result = m_Functions.CreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_VkDevice);
		if (result != VK_SUCCESS)
		{
			PX_CORE_ERROR("vkCreateDevice failed on '{}': {}", m_Info.AdapterName, ResultToString(result));
			m_VkDevice = VK_NULL_HANDLE;
			return false;
		}

		m_Functions.DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(m_Functions.GetDeviceProcAddr(m_VkDevice, "vkDestroyDevice"));
		m_Functions.GetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(m_Functions.GetDeviceProcAddr(m_VkDevice, "vkGetDeviceQueue"));
		m_Functions.DeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(m_Functions.GetDeviceProcAddr(m_VkDevice, "vkDeviceWaitIdle"));
		if (!m_Functions.DestroyDevice || !m_Functions.GetDeviceQueue || !m_Functions.DeviceWaitIdle)
		{
			PX_CORE_ERROR("Failed to load Vulkan device functions");
			return false;
		}

		m_Functions.GetDeviceQueue(m_VkDevice, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);
		return true;
	}

	bool VulkanGraphicsDevice::CreateNvrhiDevice(const GraphicsDeviceSpecification& specification)
	{
		std::vector<const char*> instanceExtensions;
		for (const std::string& extension : m_EnabledInstanceExtensions)
			instanceExtensions.push_back(extension.c_str());
		std::vector<const char*> deviceExtensions;
		for (const std::string& extension : m_EnabledDeviceExtensions)
			deviceExtensions.push_back(extension.c_str());

		VulkanHpp::InitDispatcher(m_Functions.GetInstanceProcAddr, m_Instance, m_VkDevice);

		nvrhi::vulkan::DeviceDesc desc;
		desc.errorCB = m_MessageCallback.get();
		desc.instance = m_Instance;
		desc.physicalDevice = m_PhysicalDevice;
		desc.device = m_VkDevice;
		desc.graphicsQueue = m_GraphicsQueue;
		desc.graphicsQueueIndex = static_cast<int>(m_GraphicsQueueFamily);
		desc.instanceExtensions = instanceExtensions.data();
		desc.numInstanceExtensions = instanceExtensions.size();
		desc.deviceExtensions = deviceExtensions.data();
		desc.numDeviceExtensions = deviceExtensions.size();
		desc.bufferDeviceAddressSupported = m_BufferDeviceAddress;
		desc.descriptorBindingUniformBufferUpdateAfterBind = m_UniformBufferUpdateAfterBind;
		desc.vulkanLibraryName = VulkanLoader::GetLibraryName();

		m_BackendDevice = nvrhi::vulkan::createDevice(desc);
		m_Device = m_BackendDevice;
		if (!m_Device)
		{
			PX_CORE_ERROR("Failed to create the NVRHI Vulkan device");
			return false;
		}

		if (specification.EnableValidation)
			m_Device = nvrhi::validation::createValidationLayer(m_Device);

		PX_CORE_INFO("Graphics device: {} (Vulkan {}.{}, {}){}", m_Info.AdapterName, m_Info.ApiVersionMajor, m_Info.ApiVersionMinor, m_Info.DriverName,
					 specification.EnableValidation ? (m_Info.SynchronizationValidation ? " with validation and synchronization validation" : " with validation") : "");
		return true;
	}

}
