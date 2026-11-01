#include "pxpch.h"
#include "Platform/Vulkan/VulkanDiagnostics.h"

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace Paralax::VulkanDiagnostics {

	namespace {

		struct ImageInfo
		{
			bool IsBRDF = false;
			uint32_t ReadbackCopies = 0;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			VkDeviceSize Offset = 0;
		};

		struct ViewInfo
		{
			VkImage Image = VK_NULL_HANDLE;
			bool IsBRDF = false;
		};

		struct TraceState
		{
			std::mutex Mutex;
			uint64_t Sequence = 0;
			uint32_t PendingBRDFs = 0;
			std::unordered_map<VkImage, ImageInfo> Images;
			std::unordered_map<VkImageView, ViewInfo> Views;
			std::unordered_map<VkDeviceMemory, VkDeviceSize> MemorySizes;
			std::unordered_set<VkDeviceMemory> BRDFMemories;
		};

		TraceState s_Trace;
		std::remove_reference_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)> s_Original;

		template<typename Handle>
		uint64_t HandleID(Handle handle)
		{
			if constexpr (std::is_pointer_v<Handle>)
				return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
			else
				return static_cast<uint64_t>(handle);
		}

		// Callers hold the trace mutex. The second LUT readback is the first-frame checkpoint in SceneRenderer.
		bool WindowOpen()
		{
			return s_Trace.PendingBRDFs != 0;
		}

		bool IsBRDF(VkImage image)
		{
			auto it = s_Trace.Images.find(image);
			return it != s_Trace.Images.end() && it->second.IsBRDF;
		}

		VkDeviceSize AllocationSize(VkDeviceMemory memory)
		{
			auto it = s_Trace.MemorySizes.find(memory);
			return it == s_Trace.MemorySizes.end() ? 0 : it->second;
		}

		VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image)
		{
			const VkResult result = s_Original.vkCreateImage(device, info, allocator, image);
			if (result == VK_SUCCESS && info && image)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				const bool isBRDF = info->format == VK_FORMAT_R16G16_SFLOAT && info->extent.width == 128 && info->extent.height == 128 &&
									info->extent.depth == 1 && info->arrayLayers == 1 && info->mipLevels == 1 && (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
				s_Trace.Images[*image] = {isBRDF};
				if (isBRDF)
					s_Trace.PendingBRDFs++;
				if (WindowOpen())
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} create_image image={:#x} lut={} width={} height={} format={} usage={:#x} initialLayout={}",
								 ++s_Trace.Sequence, HandleID(*image), isBRDF, info->extent.width, info->extent.height, static_cast<uint32_t>(info->format), info->usage, static_cast<uint32_t>(info->initialLayout));
			}
			return result;
		}

		VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
		{
			{
				std::scoped_lock lock(s_Trace.Mutex);
				auto it = s_Trace.Images.find(image);
				if (WindowOpen() || IsBRDF(image))
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} destroy_image image={:#x} memory={:#x}", ++s_Trace.Sequence, HandleID(image), it == s_Trace.Images.end() ? 0 : HandleID(it->second.Memory));
				if (it != s_Trace.Images.end())
				{
					if (it->second.IsBRDF && it->second.ReadbackCopies < 2)
						s_Trace.PendingBRDFs--;
					s_Trace.Images.erase(it);
				}
			}
			s_Original.vkDestroyImage(device, image, allocator);
		}

		VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view)
		{
			const VkResult result = s_Original.vkCreateImageView(device, info, allocator, view);
			if (result == VK_SUCCESS && info && view)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				const bool isBRDF = IsBRDF(info->image);
				s_Trace.Views[*view] = {info->image, isBRDF};
				if (WindowOpen() || isBRDF)
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} create_view view={:#x} image={:#x} format={}", ++s_Trace.Sequence, HandleID(*view), HandleID(info->image), static_cast<uint32_t>(info->format));
			}
			return result;
		}

		VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
		{
			{
				std::scoped_lock lock(s_Trace.Mutex);
				auto it = s_Trace.Views.find(view);
				if (WindowOpen() || (it != s_Trace.Views.end() && it->second.IsBRDF))
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} destroy_view view={:#x} image={:#x}", ++s_Trace.Sequence, HandleID(view), it == s_Trace.Views.end() ? 0 : HandleID(it->second.Image));
				s_Trace.Views.erase(view);
			}
			s_Original.vkDestroyImageView(device, view, allocator);
		}

		VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory)
		{
			const VkResult result = s_Original.vkAllocateMemory(device, info, allocator, memory);
			if (result == VK_SUCCESS && info && memory)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				s_Trace.MemorySizes[*memory] = info->allocationSize;
				if (WindowOpen())
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} allocate memory={:#x} bytes={} type={}", ++s_Trace.Sequence, HandleID(*memory), info->allocationSize, info->memoryTypeIndex);
			}
			return result;
		}

		VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator)
		{
			{
				std::scoped_lock lock(s_Trace.Mutex);
				if (WindowOpen() || s_Trace.BRDFMemories.contains(memory))
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} free memory={:#x} bytes={}", ++s_Trace.Sequence, HandleID(memory), AllocationSize(memory));
				s_Trace.MemorySizes.erase(memory);
				s_Trace.BRDFMemories.erase(memory);
			}
			s_Original.vkFreeMemory(device, memory, allocator);
		}

		void RecordImageBinding(VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
		{
			auto it = s_Trace.Images.find(image);
			if (it != s_Trace.Images.end())
			{
				it->second.Memory = memory;
				it->second.Offset = offset;
			}
			const bool isBRDF = IsBRDF(image);
			if (isBRDF)
				s_Trace.BRDFMemories.insert(memory);
			if (WindowOpen() || isBRDF || s_Trace.BRDFMemories.contains(memory))
				PX_CORE_WARN("[BRDF Vulkan trace] seq={} bind_image image={:#x} memory={:#x} offset={} allocationBytes={}", ++s_Trace.Sequence, HandleID(image), HandleID(memory), offset, AllocationSize(memory));
		}

		void RecordBufferBinding(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
		{
			if (WindowOpen() || s_Trace.BRDFMemories.contains(memory))
				PX_CORE_WARN("[BRDF Vulkan trace] seq={} bind_buffer buffer={:#x} memory={:#x} offset={} allocationBytes={}", ++s_Trace.Sequence, HandleID(buffer), HandleID(memory), offset, AllocationSize(memory));
		}

		VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
		{
			const VkResult result = s_Original.vkBindImageMemory(device, image, memory, offset);
			if (result == VK_SUCCESS)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				RecordImageBinding(image, memory, offset);
			}
			return result;
		}

		template<typename Function>
		VkResult BindImages(Function function, VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos)
		{
			const VkResult result = function(device, count, infos);
			if (result == VK_SUCCESS && infos)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				for (uint32_t i = 0; i < count; i++)
					RecordImageBinding(infos[i].image, infos[i].memory, infos[i].memoryOffset);
			}
			return result;
		}

		VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos)
		{
			return BindImages(s_Original.vkBindImageMemory2, device, count, infos);
		}
		VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2KHR(VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos)
		{
			return BindImages(s_Original.vkBindImageMemory2KHR, device, count, infos);
		}

		VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
		{
			const VkResult result = s_Original.vkBindBufferMemory(device, buffer, memory, offset);
			if (result == VK_SUCCESS)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				RecordBufferBinding(buffer, memory, offset);
			}
			return result;
		}

		template<typename Function>
		VkResult BindBuffers(Function function, VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
		{
			const VkResult result = function(device, count, infos);
			if (result == VK_SUCCESS && infos)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				for (uint32_t i = 0; i < count; i++)
					RecordBufferBinding(infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
			}
			return result;
		}

		VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
		{
			return BindBuffers(s_Original.vkBindBufferMemory2, device, count, infos);
		}
		VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2KHR(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
		{
			return BindBuffers(s_Original.vkBindBufferMemory2KHR, device, count, infos);
		}

		void RecordBarrier(VkCommandBuffer commands, VkImage image, VkImageLayout before, VkImageLayout after,
						   VkPipelineStageFlags2 sourceStages, VkPipelineStageFlags2 destinationStages, VkAccessFlags2 sourceAccess, VkAccessFlags2 destinationAccess,
						   const VkImageSubresourceRange& range)
		{
			if (IsBRDF(image))
				PX_CORE_WARN("[BRDF Vulkan trace] seq={} barrier command={:#x} image={:#x} oldLayout={} newLayout={} srcStage={:#x} dstStage={:#x} srcAccess={:#x} dstAccess={:#x} mip={} mips={} layer={} layers={}",
							 ++s_Trace.Sequence, HandleID(commands), HandleID(image), static_cast<uint32_t>(before), static_cast<uint32_t>(after), sourceStages, destinationStages, sourceAccess, destinationAccess,
							 range.baseMipLevel, range.levelCount, range.baseArrayLayer, range.layerCount);
		}

		template<typename Function>
		void Barrier2(Function function, VkCommandBuffer commands, const VkDependencyInfo* info)
		{
			if (info && info->pImageMemoryBarriers)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++)
				{
					const auto& barrier = info->pImageMemoryBarriers[i];
					RecordBarrier(commands, barrier.image, barrier.oldLayout, barrier.newLayout, barrier.srcStageMask, barrier.dstStageMask, barrier.srcAccessMask, barrier.dstAccessMask, barrier.subresourceRange);
				}
			}
			function(commands, info);
		}

		VKAPI_ATTR void VKAPI_CALL PipelineBarrier2(VkCommandBuffer commands, const VkDependencyInfo* info)
		{
			Barrier2(s_Original.vkCmdPipelineBarrier2, commands, info);
		}
		VKAPI_ATTR void VKAPI_CALL PipelineBarrier2KHR(VkCommandBuffer commands, const VkDependencyInfo* info)
		{
			Barrier2(s_Original.vkCmdPipelineBarrier2KHR, commands, info);
		}

		VKAPI_ATTR void VKAPI_CALL PipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags sourceStages, VkPipelineStageFlags destinationStages, VkDependencyFlags flags,
												   uint32_t memoryCount, const VkMemoryBarrier* memory, uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, uint32_t imageCount, const VkImageMemoryBarrier* images)
		{
			if (images)
			{
				std::scoped_lock lock(s_Trace.Mutex);
				for (uint32_t i = 0; i < imageCount; i++)
					RecordBarrier(commands, images[i].image, images[i].oldLayout, images[i].newLayout, sourceStages, destinationStages, images[i].srcAccessMask, images[i].dstAccessMask, images[i].subresourceRange);
			}
			s_Original.vkCmdPipelineBarrier(commands, sourceStages, destinationStages, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
		}

		VKAPI_ATTR void VKAPI_CALL CopyImageToBuffer(VkCommandBuffer commands, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions)
		{
			{
				std::scoped_lock lock(s_Trace.Mutex);
				auto it = s_Trace.Images.find(image);
				if (it != s_Trace.Images.end() && it->second.IsBRDF)
				{
					it->second.ReadbackCopies++;
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} readback command={:#x} image={:#x} memory={:#x} offset={} buffer={:#x} layout={} ordinal={}", ++s_Trace.Sequence, HandleID(commands), HandleID(image), HandleID(it->second.Memory), it->second.Offset, HandleID(buffer), static_cast<uint32_t>(layout), it->second.ReadbackCopies);
					if (it->second.ReadbackCopies == 2)
					{
						s_Trace.PendingBRDFs--;
						PX_CORE_WARN("[BRDF Vulkan trace] seq={} window_end image={:#x} reason=second_lut_readback", ++s_Trace.Sequence, HandleID(image));
					}
				}
			}
			s_Original.vkCmdCopyImageToBuffer(commands, image, layout, buffer, count, regions);
		}

		VKAPI_ATTR void VKAPI_CALL ClearColorImage(VkCommandBuffer commands, VkImage image, VkImageLayout layout, const VkClearColorValue* color, uint32_t count, const VkImageSubresourceRange* ranges)
		{
			{
				std::scoped_lock lock(s_Trace.Mutex);
				if (IsBRDF(image))
					PX_CORE_WARN("[BRDF Vulkan trace] seq={} clear command={:#x} image={:#x} layout={}", ++s_Trace.Sequence, HandleID(commands), HandleID(image), static_cast<uint32_t>(layout));
			}
			s_Original.vkCmdClearColorImage(commands, image, layout, color, count, ranges);
		}

	}

	bool EnableBRDFTrace()
	{
		const char* setting = std::getenv("PX_TRACE_BRDF_BARRIERS");
		if (!setting || std::strcmp(setting, "1") != 0)
			return true;

		auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
		if (!dispatcher.vkCreateImage || !dispatcher.vkDestroyImage || !dispatcher.vkCreateImageView || !dispatcher.vkDestroyImageView ||
			!dispatcher.vkAllocateMemory || !dispatcher.vkFreeMemory || !dispatcher.vkBindImageMemory || !dispatcher.vkBindBufferMemory ||
			!dispatcher.vkCmdPipelineBarrier || !dispatcher.vkCmdCopyImageToBuffer || !dispatcher.vkCmdClearColorImage)
		{
			PX_CORE_ERROR("Cannot enable BRDF Vulkan trace: required Vulkan entry points are missing");
			return false;
		}
		s_Original = dispatcher;
		{
			std::scoped_lock lock(s_Trace.Mutex);
			s_Trace.Sequence = 0;
			s_Trace.PendingBRDFs = 0;
			s_Trace.Images.clear();
			s_Trace.Views.clear();
			s_Trace.MemorySizes.clear();
			s_Trace.BRDFMemories.clear();
		}
		auto hook = [](auto& entry, auto replacement) { if (entry) entry = replacement; };
		hook(dispatcher.vkCreateImage, CreateImage);
		hook(dispatcher.vkDestroyImage, DestroyImage);
		hook(dispatcher.vkCreateImageView, CreateImageView);
		hook(dispatcher.vkDestroyImageView, DestroyImageView);
		hook(dispatcher.vkAllocateMemory, AllocateMemory);
		hook(dispatcher.vkFreeMemory, FreeMemory);
		hook(dispatcher.vkBindImageMemory, BindImageMemory);
		hook(dispatcher.vkBindImageMemory2, BindImageMemory2);
		hook(dispatcher.vkBindImageMemory2KHR, BindImageMemory2KHR);
		hook(dispatcher.vkBindBufferMemory, BindBufferMemory);
		hook(dispatcher.vkBindBufferMemory2, BindBufferMemory2);
		hook(dispatcher.vkBindBufferMemory2KHR, BindBufferMemory2KHR);
		hook(dispatcher.vkCmdPipelineBarrier, PipelineBarrier);
		hook(dispatcher.vkCmdPipelineBarrier2, PipelineBarrier2);
		hook(dispatcher.vkCmdPipelineBarrier2KHR, PipelineBarrier2KHR);
		hook(dispatcher.vkCmdCopyImageToBuffer, CopyImageToBuffer);
		hook(dispatcher.vkCmdClearColorImage, ClearColorImage);
		PX_CORE_WARN("[BRDF Vulkan trace] enabled; memory window ends at the second LUT readback; Vulkan calls are forwarded unchanged");
		return true;
	}

}
