#include "pxpch.h"
#include "Paralax/Renderer/TextureReadback.h"

#include <cstring>

namespace Paralax {

	Image ReadbackTexture(nvrhi::IDevice* device, nvrhi::ITexture* texture)
	{
		PX_CORE_VERIFY(device && texture, "ReadbackTexture requires a device and a texture");

		const nvrhi::TextureDesc& source = texture->getDesc();
		const bool isRGBA = source.format == nvrhi::Format::RGBA8_UNORM || source.format == nvrhi::Format::SRGBA8_UNORM;
		const bool isBGRA = source.format == nvrhi::Format::BGRA8_UNORM || source.format == nvrhi::Format::SBGRA8_UNORM;
		if (source.dimension != nvrhi::TextureDimension::Texture2D || source.sampleCount != 1)
		{
			PX_CORE_ERROR("ReadbackTexture: '{}' must be a single-sampled 2D texture", source.debugName);
			return {};
		}
		if (!isRGBA && !isBGRA)
		{
			PX_CORE_ERROR("ReadbackTexture: unsupported format {} for '{}'", static_cast<int>(source.format), source.debugName);
			return {};
		}

		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = source.width;
		stagingDesc.height = source.height;
		stagingDesc.format = source.format;
		stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
		stagingDesc.debugName = "ReadbackStaging";
		stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
		stagingDesc.keepInitialState = true;

		nvrhi::StagingTextureHandle staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		nvrhi::CommandListHandle commandList = device->createCommandList();
		if (!staging || !commandList)
		{
			PX_CORE_ERROR("ReadbackTexture: failed to create staging resources");
			return {};
		}

		commandList->open();
		commandList->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
		commandList->close();
		device->executeCommandList(commandList);
		device->waitForIdle();

		size_t rowPitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (!mapped)
		{
			PX_CORE_ERROR("ReadbackTexture: failed to map staging texture");
			return {};
		}

		Image image(source.width, source.height);
		std::vector<uint8_t>& pixels = image.GetPixels();
		const size_t rowBytes = static_cast<size_t>(source.width) * 4;
		for (uint32_t y = 0; y < source.height; y++)
		{
			uint8_t* destination = pixels.data() + y * rowBytes;
			std::memcpy(destination, mapped + y * rowPitch, rowBytes);
			if (isBGRA)
			{
				for (size_t x = 0; x < rowBytes; x += 4)
					std::swap(destination[x], destination[x + 2]);
			}
		}

		device->unmapStagingTexture(staging);
		return image;
	}

}
