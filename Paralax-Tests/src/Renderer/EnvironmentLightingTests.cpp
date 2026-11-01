#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/EnvironmentLighting.h>
#include <Paralax/Renderer/GPUResourceCache.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace Paralax;

namespace {

	std::vector<float> ReadHalfTexture(nvrhi::IDevice* device, nvrhi::ITexture* texture, uint32_t mip, uint32_t face, uint32_t channels)
	{
		const auto& source = texture->getDesc();
		nvrhi::TextureDesc desc;
		desc.width = std::max(1u, source.width >> mip);
		desc.height = std::max(1u, source.height >> mip);
		desc.format = source.format;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice().setMipLevel(mip).setArraySlice(face));
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t pitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		REQUIRE(mapped);
		std::vector<float> values(static_cast<size_t>(desc.width) * desc.height * channels);
		for (uint32_t y = 0; y < desc.height; y++)
			for (uint32_t x = 0; x < desc.width * channels; x++)
			{
				uint16_t bits;
				std::memcpy(&bits, mapped + y * pitch + x * sizeof(bits), sizeof(bits));
				values[(static_cast<size_t>(y) * desc.width * channels) + x] = glm::unpackHalf1x16(bits);
			}
		device->unmapStagingTexture(staging);
		return values;
	}

}

TEST_CASE("EnvironmentLighting: BRDF integration and every specular mip retain finite nonzero energy")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	INFO("adapter: ", graphics.GetInfo().AdapterName, ", driver: ", graphics.GetInfo().DriverName);
	{
		auto* device = graphics.GetDevice();
		EnvironmentLighting lighting(device);
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		REQUIRE(lighting.Initialize(commands));
		commands->close();
		device->executeCommandList(commands);

		EnvironmentMaps maps;
		commands->open();
		lighting.GenerateProceduralSky(commands, maps, glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f)), glm::vec3(1.0f), 1.0f, 0.0f);
		commands->close();
		device->executeCommandList(commands);

		const auto lut = ReadHalfTexture(device, lighting.GetBRDFLut(), 0, 0, 2);
		size_t invalidLutTexels = 0;
		float minimumEnergy = std::numeric_limits<float>::max();
		for (size_t i = 0; i < lut.size(); i += 2)
		{
			const float energy = lut[i] + lut[i + 1];
			minimumEnergy = std::min(minimumEnergy, energy);
			invalidLutTexels += !std::isfinite(energy) || lut[i] < 0 || lut[i + 1] < 0 || energy < 0.01f || energy > 2.0f ? 1 : 0;
		}
		INFO("minimum LUT energy: ", minimumEnergy);
		CHECK(invalidLutTexels == 0);

		for (uint32_t mip = 0; mip < maps.SpecularMipCount; mip++)
			for (uint32_t face = 0; face < 6; face++)
			{
				CAPTURE(mip);
				CAPTURE(face);
				const auto values = ReadHalfTexture(device, maps.Specular, mip, face, 4);
				size_t invalidChannels = 0;
				float minimum = std::numeric_limits<float>::max();
				for (float value : values)
				{
					minimum = std::min(minimum, value);
					invalidChannels += !std::isfinite(value) || value < 0.02f || value > 2.0f ? 1 : 0;
				}
				INFO("minimum specular value: ", minimum);
				CHECK(invalidChannels == 0);
			}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("EnvironmentLighting: packaged HDR environments generate finite radiance diffuse and specular maps")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	{
		auto* device = graphics.GetDevice();
		AssetManager assets;
		GPUResourceCache resources(device);
		EnvironmentLighting lighting(device);
		auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		REQUIRE(lighting.Initialize(commands));
		commands->close();
		device->executeCommandList(commands);
		for (uint64_t handle : {AssetManager::BuiltinSkyEnvironment, AssetManager::BuiltinStudioEnvironment})
		{
			CAPTURE(handle);
			auto source = assets.GetAsset<TextureAsset>(AssetHandle(handle));
			REQUIRE(source);
			EnvironmentMaps maps;
			commands->open();
			auto* uploaded = resources.GetTexture(*source, commands);
			REQUIRE(uploaded);
			lighting.GenerateFromEquirect(commands, maps, uploaded, 1.0f);
			commands->close();
			device->executeCommandList(commands);
			for (const auto& texture : {maps.Radiance, maps.Diffuse, maps.Specular})
			{
				REQUIRE(texture);
				CAPTURE(texture->getDesc().debugName);
				for (uint32_t mip = 0; mip < texture->getDesc().mipLevels; mip++)
				{
					CAPTURE(mip);
					double totalEnergy = 0;
					size_t invalid = 0;
					for (uint32_t face = 0; face < 6; face++)
					{
						const auto values = ReadHalfTexture(device, texture, mip, face, 4);
						for (size_t i = 0; i < values.size(); i++)
						{
							const float value = values[i];
							invalid += !std::isfinite(value) || value < 0.0f || value > 65.0f ? 1 : 0;
							if (i % 4 != 3)
								totalEnergy += static_cast<double>(value);
						}
					}
					CHECK(invalid == 0);
					CHECK(totalEnergy > 0.01);
				}
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}
