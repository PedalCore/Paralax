#include "pxpch.h"
#include "Paralax/Renderer/EnvironmentLighting.h"

#include "Paralax/Renderer/SceneRendererTypes.h"
#include "Paralax/Renderer/ShaderLibrary.h"

namespace Paralax {

	namespace {

		nvrhi::TextureHandle CreateCube(nvrhi::IDevice* device, uint32_t size, uint32_t mips, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = size;
			desc.height = size;
			desc.arraySize = 6;
			desc.mipLevels = mips;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isUAV = true;
			desc.debugName = name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			return device->createTexture(desc);
		}

		uint32_t MipCount(uint32_t size)
		{
			uint32_t count = 1;
			while (size > 1)
			{
				size /= 2;
				count++;
			}
			return count;
		}

		nvrhi::BindingSetItem CubeMipUAV(nvrhi::ITexture* texture, uint32_t mip)
		{
			return nvrhi::BindingSetItem::Texture_UAV(0, texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mip, 1, 0, 6), nvrhi::TextureDimension::Texture2DArray);
		}

	}

	EnvironmentLighting::EnvironmentLighting(nvrhi::IDevice* device)
		: m_Device(device)
	{
		PX_CORE_VERIFY(device, "EnvironmentLighting requires a device");
	}

	nvrhi::ComputePipelineHandle EnvironmentLighting::CreatePipeline(const char* entryPoint, nvrhi::BindingLayoutHandle& outLayout, const nvrhi::BindingLayoutDesc& layoutDesc)
	{
		if (!outLayout)
			outLayout = m_Device->createBindingLayout(layoutDesc);
		nvrhi::ShaderHandle shader = ShaderLibrary::CreateShader(m_Device, "IBL", entryPoint, nvrhi::ShaderType::Compute);
		if (!shader || !outLayout)
			return nullptr;
		nvrhi::ComputePipelineDesc desc;
		desc.CS = shader;
		desc.bindingLayouts = {outLayout};
		return m_Device->createComputePipeline(desc);
	}

	bool EnvironmentLighting::Initialize(nvrhi::ICommandList* commandList)
	{
		nvrhi::SamplerDesc clamp;
		clamp.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_LinearClamp = m_Device->createSampler(clamp);
		nvrhi::SamplerDesc wrapU = clamp;
		wrapU.setAddressU(nvrhi::SamplerAddressMode::Wrap);
		m_LinearWrapU = m_Device->createSampler(wrapU);

		const auto push = nvrhi::BindingLayoutItem::PushConstants(0, sizeof(GPU::IBLConstants));
		nvrhi::BindingLayoutDesc sky;
		sky.visibility = nvrhi::ShaderType::Compute;
		sky.bindings = {push, nvrhi::BindingLayoutItem::Texture_UAV(0)};

		nvrhi::BindingLayoutDesc equirect = sky;
		equirect.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(1));
		equirect.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(1));

		nvrhi::BindingLayoutDesc filter = sky;
		filter.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(0));
		filter.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(0));

		nvrhi::BindingLayoutDesc lut;
		lut.visibility = nvrhi::ShaderType::Compute;
		lut.bindings = {push, nvrhi::BindingLayoutItem::Texture_UAV(1)};

		m_SkyPipeline = CreatePipeline("ProceduralSkyCS", m_SkyLayout, sky);
		m_DownsamplePipeline = CreatePipeline("DownsampleCubeCS", m_FilterLayout, filter);
		m_EquirectPipeline = CreatePipeline("EquirectToCubeCS", m_EquirectLayout, equirect);
		m_PrefilterPipeline = CreatePipeline("PrefilterSpecularCS", m_FilterLayout, filter);
		m_IrradiancePipeline = CreatePipeline("IrradianceCS", m_FilterLayout, filter);
		m_LutPipeline = CreatePipeline("BRDFLutCS", m_LutLayout, lut);
		if (!m_SkyPipeline || !m_DownsamplePipeline || !m_EquirectPipeline || !m_PrefilterPipeline || !m_IrradiancePipeline || !m_LutPipeline)
		{
			PX_CORE_ERROR("Failed to create environment lighting pipelines");
			return false;
		}

		nvrhi::TextureDesc lutDesc;
		lutDesc.width = 128;
		lutDesc.height = 128;
		lutDesc.format = nvrhi::Format::RG16_FLOAT;
		lutDesc.isUAV = true;
		lutDesc.debugName = "BRDFLut";
		lutDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		lutDesc.keepInitialState = true;
		m_BRDFLut = m_Device->createTexture(lutDesc);

		nvrhi::BindingSetDesc lutSet;
		lutSet.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::IBLConstants)), nvrhi::BindingSetItem::Texture_UAV(1, m_BRDFLut)};
		nvrhi::BindingSetHandle lutBindings = m_Device->createBindingSet(lutSet, m_LutLayout);
		GPU::IBLConstants constants{};
		Dispatch(commandList, m_LutPipeline, lutBindings, &constants, 128, 1);
		return true;
	}

	void EnvironmentLighting::Dispatch(nvrhi::ICommandList* commandList, nvrhi::IComputePipeline* pipeline, nvrhi::IBindingSet* bindings, const void* constants, uint32_t size, uint32_t layers)
	{
		nvrhi::ComputeState state;
		state.pipeline = pipeline;
		state.bindings = {bindings};
		commandList->setComputeState(state);
		commandList->setPushConstants(constants, sizeof(GPU::IBLConstants));
		const uint32_t groups = (size + 7) / 8;
		commandList->dispatch(groups, groups, layers);
	}

	void EnvironmentLighting::EnsureMaps(EnvironmentMaps& maps)
	{
		if (!maps.Radiance)
			maps.Radiance = CreateCube(m_Device, RadianceSize, MipCount(RadianceSize), "EnvironmentRadiance");
		if (!maps.Specular)
			maps.Specular = CreateCube(m_Device, SpecularSize, SpecularMips, "EnvironmentSpecular");
		if (!maps.Diffuse)
			maps.Diffuse = CreateCube(m_Device, DiffuseSize, 1, "EnvironmentDiffuse");
		maps.SpecularMipCount = SpecularMips;
	}

	void EnvironmentLighting::GenerateProceduralSky(nvrhi::ICommandList* commandList, EnvironmentMaps& maps, const glm::vec3& sunDirection, const glm::vec3& sunColor, float intensity, float sunDiskIntensity)
	{
		EnsureMaps(maps);
		{
			const uint32_t mip = 0;
			const uint32_t size = RadianceSize;
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::IBLConstants)), CubeMipUAV(maps.Radiance, mip)};
			nvrhi::BindingSetHandle bindings = m_Device->createBindingSet(set, m_SkyLayout);

			GPU::IBLConstants constants{};
			constants.SunDirection = glm::vec4(-glm::normalize(sunDirection), sunDiskIntensity);
			constants.SunColor = glm::vec4(sunColor, 1.0f);
			constants.SkyParams = glm::vec4(intensity, 0.0f, static_cast<float>(size), 0.0f);
			Dispatch(commandList, m_SkyPipeline, bindings, &constants, size, 6);
		}
		DownsampleRadiance(commandList, maps);
		Prefilter(commandList, maps);
	}

	void EnvironmentLighting::GenerateFromEquirect(nvrhi::ICommandList* commandList, EnvironmentMaps& maps, nvrhi::ITexture* equirect, float intensity)
	{
		EnsureMaps(maps);
		const uint32_t mips = MipCount(RadianceSize);
		for (uint32_t mip = 0; mip < mips; mip++)
		{
			const uint32_t size = std::max(RadianceSize >> mip, 1u);
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::IBLConstants)), CubeMipUAV(maps.Radiance, mip),
							nvrhi::BindingSetItem::Texture_SRV(1, equirect), nvrhi::BindingSetItem::Sampler(1, m_LinearWrapU)};
			nvrhi::BindingSetHandle bindings = m_Device->createBindingSet(set, m_EquirectLayout);

			GPU::IBLConstants constants{};
			constants.SkyParams = glm::vec4(intensity, 0.0f, static_cast<float>(size), 0.0f);
			constants.SourceSize = static_cast<float>(equirect->getDesc().width);
			Dispatch(commandList, m_EquirectPipeline, bindings, &constants, size, 6);
		}
		DownsampleRadiance(commandList, maps);
		Prefilter(commandList, maps);
	}

	void EnvironmentLighting::DownsampleRadiance(nvrhi::ICommandList* commandList, EnvironmentMaps& maps)
	{
		const uint32_t mips = MipCount(RadianceSize);
		for (uint32_t mip = 1; mip < mips; mip++)
		{
			const uint32_t size = std::max(RadianceSize >> mip, 1u);
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::IBLConstants)), CubeMipUAV(maps.Radiance, mip),
							nvrhi::BindingSetItem::Texture_SRV(0, maps.Radiance, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 6)),
							nvrhi::BindingSetItem::Sampler(0, m_LinearClamp)};
			nvrhi::BindingSetHandle bindings = m_Device->createBindingSet(set, m_FilterLayout);
			GPU::IBLConstants constants{};
			constants.SkyParams = glm::vec4(1.0f, 0.0f, static_cast<float>(size), 0.0f);
			Dispatch(commandList, m_DownsamplePipeline, bindings, &constants, size, 6);
		}
	}

	void EnvironmentLighting::Prefilter(nvrhi::ICommandList* commandList, EnvironmentMaps& maps)
	{
		const uint32_t radianceMips = MipCount(RadianceSize);
		for (uint32_t mip = 0; mip < SpecularMips; mip++)
		{
			const uint32_t size = std::max(SpecularSize >> mip, 1u);
			nvrhi::BindingSetDesc set;
			set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::IBLConstants)), CubeMipUAV(maps.Specular, mip),
							nvrhi::BindingSetItem::Texture_SRV(0, maps.Radiance), nvrhi::BindingSetItem::Sampler(0, m_LinearClamp)};
			nvrhi::BindingSetHandle bindings = m_Device->createBindingSet(set, m_FilterLayout);

			GPU::IBLConstants constants{};
			const float roughness = static_cast<float>(mip) / static_cast<float>(SpecularMips - 1);
			const float samples = mip == 0 ? 1.0f : 64.0f + 32.0f * static_cast<float>(mip);
			constants.SkyParams = glm::vec4(1.0f, roughness, static_cast<float>(size), samples);
			constants.SourceMipCount = radianceMips;
			constants.SourceSize = static_cast<float>(RadianceSize);
			Dispatch(commandList, m_PrefilterPipeline, bindings, &constants, size, 6);
		}

		nvrhi::BindingSetDesc set;
		set.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(GPU::IBLConstants)), CubeMipUAV(maps.Diffuse, 0),
						nvrhi::BindingSetItem::Texture_SRV(0, maps.Radiance), nvrhi::BindingSetItem::Sampler(0, m_LinearClamp)};
		nvrhi::BindingSetHandle bindings = m_Device->createBindingSet(set, m_FilterLayout);
		GPU::IBLConstants constants{};
		constants.SkyParams = glm::vec4(1.0f, 0.0f, static_cast<float>(DiffuseSize), 0.0f);
		Dispatch(commandList, m_IrradiancePipeline, bindings, &constants, DiffuseSize, 6);
	}

}
