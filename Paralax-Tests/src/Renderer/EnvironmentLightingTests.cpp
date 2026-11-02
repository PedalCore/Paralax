#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/EnvironmentLighting.h>
#include <Paralax/Renderer/GPUResourceCache.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
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
		REQUIRE(device->waitForIdle());
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

TEST_CASE("EnvironmentLighting: constructing it without a device is an asserted precondition")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(EnvironmentLighting(nullptr), Paralax::Testing::AssertionFailedError);
}

TEST_CASE("GPUResourceCache: constructing it without a device is an asserted precondition")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(GPUResourceCache(nullptr), Paralax::Testing::AssertionFailedError);
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
	INFO("adapter: ", graphics.GetInfo().AdapterName, ", driver: ", graphics.GetInfo().DriverName);
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
					std::array<double, 6> faceEnergies{};
					size_t invalid = 0;
					for (uint32_t face = 0; face < 6; face++)
					{
						CAPTURE(face);
						const auto values = ReadHalfTexture(device, texture, mip, face, 4);
						double faceEnergy = 0;
						float minimumAlpha = 1.0f, maximumAlpha = 0.0f;
						for (size_t i = 0; i < values.size(); i++)
						{
							const float value = values[i];
							invalid += !std::isfinite(value) || value < 0.0f || value > 65.0f ? 1 : 0;
							if (i % 4 != 3)
								faceEnergy += static_cast<double>(value);
							else
							{
								minimumAlpha = std::min(minimumAlpha, value);
								maximumAlpha = std::max(maximumAlpha, value);
							}
						}
						INFO("face RGB energy: ", faceEnergy, ", alpha range: [", minimumAlpha, ", ", maximumAlpha, "]");
						CHECK(minimumAlpha == 1.0f);
						CHECK(maximumAlpha == 1.0f);
						faceEnergies[face] = faceEnergy;
						totalEnergy += faceEnergy;
					}
					CHECK(invalid == 0);
					INFO("face RGB energies: ", faceEnergies[0], ", ", faceEnergies[1], ", ", faceEnergies[2], ", ", faceEnergies[3], ", ", faceEnergies[4], ", ", faceEnergies[5]);
					CHECK(totalEnergy > 0.01);
				}
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}

TEST_CASE("EnvironmentLighting: filtered HDR maps survive later radiance readback allocations")
{
	if (!Testing::RequireTestGraphicsDevice())
		return;
	auto& graphics = *Testing::GetTestGraphicsDevice();
	const auto initialErrors = graphics.GetValidationErrorCount();
	INFO("adapter: ", graphics.GetInfo().AdapterName, ", driver: ", graphics.GetInfo().DriverName);
	{
		auto* device = graphics.GetDevice();
		AssetManager assets;
		GPUResourceCache resources(device);
		EnvironmentLighting lighting(device);
		auto commands = device->createCommandList();
		auto checkpoint = device->createCommandList();
		REQUIRE(commands);
		REQUIRE(checkpoint);
		commands->open();
		REQUIRE(lighting.Initialize(commands));
		commands->close();
		device->executeCommandList(commands);
		struct Snapshot
		{
			nvrhi::StagingTextureHandle Early, AfterRadiance;
			uint32_t Map = 0, Mip = 0, Face = 0, Size = 0;
		};
		std::vector<Snapshot> snapshots;
		// Allocate both checkpoints before generation. Mapping them after the ordinary readbacks
		// distinguishes loss of stored lighting from new staging allocations/copies returning zero.
		for (uint32_t map = 0; map < 2; ++map)
			for (uint32_t mip = 0; mip < (map == 0 ? 1 : EnvironmentLighting::SpecularMips); ++mip)
				for (uint32_t face = 0; face < 6; ++face)
				{
					nvrhi::TextureDesc desc;
					desc.width = desc.height = (map == 0 ? EnvironmentLighting::DiffuseSize : EnvironmentLighting::SpecularSize) >> mip;
					desc.format = nvrhi::Format::RGBA16_FLOAT;
					desc.initialState = nvrhi::ResourceStates::CopyDest;
					desc.keepInitialState = true;
					auto early = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
					auto late = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
					REQUIRE(early);
					REQUIRE(late);
					snapshots.push_back({early, late, map, mip, face, desc.width});
				}
		const auto source = assets.GetAsset<TextureAsset>(AssetHandle(AssetManager::BuiltinSkyEnvironment));
		REQUIRE(source);
		EnvironmentMaps maps;
		commands->open();
		auto* uploaded = resources.GetTexture(*source, commands);
		REQUIRE(uploaded);
		lighting.GenerateFromEquirect(commands, maps, uploaded, 1.0f);
		auto copy = [&](nvrhi::ICommandList* list, bool early) {
			for (const auto& snapshot : snapshots)
				list->copyTexture(early ? snapshot.Early : snapshot.AfterRadiance, {}, snapshot.Map == 0 ? maps.Diffuse : maps.Specular,
								  nvrhi::TextureSlice().setMipLevel(snapshot.Mip).setArraySlice(snapshot.Face));
		};
		copy(commands, true);
		commands->close();
		device->executeCommandList(commands);
		for (uint32_t mip = 0; mip < maps.Radiance->getDesc().mipLevels; ++mip)
			for (uint32_t face = 0; face < 6; ++face)
				REQUIRE_FALSE(ReadHalfTexture(device, maps.Radiance, mip, face, 4).empty());
		checkpoint->open();
		copy(checkpoint, false);
		checkpoint->close();
		device->executeCommandList(checkpoint);
		REQUIRE(device->waitForIdle());
		for (const auto& snapshot : snapshots)
		{
			CAPTURE(snapshot.Map);
			CAPTURE(snapshot.Mip);
			CAPTURE(snapshot.Face);
			const auto ordinary = ReadHalfTexture(device, snapshot.Map == 0 ? maps.Diffuse : maps.Specular, snapshot.Mip, snapshot.Face, 4);
			for (const auto& staging : {snapshot.Early, snapshot.AfterRadiance})
			{
				INFO((staging == snapshot.Early ? "immediate snapshot" : "snapshot after radiance reads"));
				size_t pitch = 0;
				const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &pitch));
				REQUIRE(mapped);
				size_t mismatch = 0, invalidAlpha = 0;
				double energy = 0;
				for (uint32_t y = 0; y < snapshot.Size; ++y)
					for (uint32_t x = 0; x < snapshot.Size * 4; ++x)
					{
						uint16_t bits;
						std::memcpy(&bits, mapped + y * pitch + x * sizeof(bits), sizeof(bits));
						const float value = glm::unpackHalf1x16(bits);
						mismatch += value != ordinary[static_cast<size_t>(y) * snapshot.Size * 4 + x] ? 1u : 0u;
						if (x % 4 == 3)
							invalidAlpha += value != 1.0f ? 1u : 0u;
						else
							energy += static_cast<double>(value);
					}
				device->unmapStagingTexture(staging);
				INFO("snapshot RGB energy: ", energy, ", invalid alpha: ", invalidAlpha, ", mismatches with ordinary readback: ", mismatch);
				CHECK(invalidAlpha == 0);
				CHECK(energy > 0.0);
				CHECK(mismatch == 0);
			}
		}
	}
	CHECK(graphics.GetValidationErrorCount() == initialErrors);
}
