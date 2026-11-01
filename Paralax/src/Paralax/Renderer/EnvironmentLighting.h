#pragma once

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

namespace Paralax {

	// GPU cubemaps for image-based lighting.
	struct EnvironmentMaps
	{
		nvrhi::TextureHandle Radiance; // Source environment with a full mip chain (also drawn as the sky).
		nvrhi::TextureHandle Specular; // GGX-prefiltered: mip m = roughness m / (mips - 1).
		nvrhi::TextureHandle Diffuse;  // Cosine-convolved irradiance / PI.
		uint32_t SpecularMipCount = 0;
	};

	// Builds EnvironmentMaps with compute shaders (Paralax/shaders/IBL.slang) and owns the split-sum BRDF lookup table.
	// Main-thread-only; work is recorded into the command list passed in, which must be open.
	class EnvironmentLighting
	{
	public:
		static constexpr uint32_t RadianceSize = 256;
		static constexpr uint32_t SpecularSize = 128;
		static constexpr uint32_t SpecularMips = 6;
		static constexpr uint32_t DiffuseSize = 32;

		explicit EnvironmentLighting(nvrhi::IDevice* device);

		// Creates pipelines and computes the BRDF LUT. Returns false (with an error logged) on failure.
		bool Initialize(nvrhi::ICommandList* commandList);

		nvrhi::ITexture* GetBRDFLut() const { return m_BRDFLut; }
		// Temporary main-thread diagnostic view, owned here; null before pipeline creation succeeds.
		nvrhi::IComputePipeline* GetBRDFLutPipeline() const { return m_LutPipeline; }

		// Fills maps with a procedural clear sky lit by a sun travelling along sunDirection.
		// sunDiskIntensity of 0 draws no sun disk.
		void GenerateProceduralSky(nvrhi::ICommandList* commandList, EnvironmentMaps& maps, const glm::vec3& sunDirection, const glm::vec3& sunColor, float intensity, float sunDiskIntensity);
		// Fills maps from an equirectangular (latitude-longitude) HDR texture.
		void GenerateFromEquirect(nvrhi::ICommandList* commandList, EnvironmentMaps& maps, nvrhi::ITexture* equirect, float intensity);

	private:
		void EnsureMaps(EnvironmentMaps& maps);
		void Prefilter(nvrhi::ICommandList* commandList, EnvironmentMaps& maps);
		// Builds radiance mips 1..N from mip 0 by 2x2 averaging.
		void DownsampleRadiance(nvrhi::ICommandList* commandList, EnvironmentMaps& maps);
		nvrhi::ComputePipelineHandle CreatePipeline(const char* entryPoint, nvrhi::BindingLayoutHandle& outLayout, const nvrhi::BindingLayoutDesc& layoutDesc);
		void Dispatch(nvrhi::ICommandList* commandList, nvrhi::IComputePipeline* pipeline, nvrhi::IBindingSet* bindings, const void* constants, uint32_t size, uint32_t layers);

	private:
		nvrhi::DeviceHandle m_Device;
		nvrhi::SamplerHandle m_LinearClamp;
		nvrhi::SamplerHandle m_LinearWrapU;
		nvrhi::TextureHandle m_BRDFLut;

		nvrhi::BindingLayoutHandle m_SkyLayout, m_EquirectLayout, m_FilterLayout, m_LutLayout;
		nvrhi::ComputePipelineHandle m_DownsamplePipeline, m_SkyPipeline, m_EquirectPipeline, m_PrefilterPipeline, m_IrradiancePipeline, m_LutPipeline;
	};

}
