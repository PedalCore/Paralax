#include "pxpch.h"
#include "Paralax/Renderer/LocalShadowMaps.h"

#include "Paralax/Renderer/LocalShadowSelection.h"

namespace Paralax {

	LocalShadowMaps::LocalShadowMaps(nvrhi::IDevice* device, const GraphicsDeviceLimits& limits, LocalShadowResourceFactory* factory)
		: m_Device(device)
		, m_Limits(limits)
		, m_Factory(factory ? *factory : m_DefaultFactory)
	{
		PX_CORE_VERIFY(device, "LocalShadowMaps requires a device");
	}

	bool LocalShadowMaps::Initialize(nvrhi::ICommandList* commands)
	{
		PX_CORE_VERIFY(commands, "LocalShadowMaps::Initialize requires an open command list");
		nvrhi::TextureDesc desc;
		desc.width = desc.height = desc.arraySize = 1;
		desc.dimension = nvrhi::TextureDimension::Texture2DArray;
		desc.format = nvrhi::Format::D32;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "LocalShadowFallback";
		desc.setClearValue(nvrhi::Color(0.0f));
		m_Fallback = m_Factory.CreateTexture(m_Device, desc);
		if (m_Fallback)
			m_FallbackFramebuffer = m_Factory.CreateFramebuffer(m_Device, nvrhi::FramebufferDesc().setDepthAttachment(m_Fallback));
		if (!m_Fallback || !m_FallbackFramebuffer)
		{
			PX_CORE_ERROR("Failed to create local-shadow fallback resources");
			return false;
		}
		commands->clearDepthStencilTexture(m_Fallback, nvrhi::AllSubresources, true, 0.0f, false, 0);
		return true;
	}

	bool LocalShadowMaps::NeedsUpdate(uint32_t resolution, uint32_t requestedLights) const
	{
		return !m_Ready || resolution != m_Resolution || requestedLights != m_RequestedLights;
	}

	bool LocalShadowMaps::Ensure(uint32_t resolution, uint32_t requestedLights)
	{
		if (!NeedsUpdate(resolution, requestedLights))
			return true;
		const uint32_t layers = LocalShadowLayerCapacity(resolution, requestedLights, m_Limits.MaxTextureArrayLayers);
		nvrhi::TextureHandle texture;
		std::vector<nvrhi::FramebufferHandle> framebuffers;
		if (layers > 0)
		{
			nvrhi::TextureDesc desc;
			desc.width = desc.height = resolution;
			desc.arraySize = layers;
			desc.dimension = nvrhi::TextureDimension::Texture2DArray;
			desc.format = nvrhi::Format::D32;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "LocalShadowMaps";
			desc.setClearValue(nvrhi::Color(0.0f));
			++m_AllocationAttempts;
			texture = m_Factory.CreateTexture(m_Device, desc);
			for (uint32_t layer = 0; texture && layer < layers; ++layer)
			{
				++m_AllocationAttempts;
				auto framebuffer = m_Factory.CreateFramebuffer(m_Device, nvrhi::FramebufferDesc().setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(texture).setArraySlice(layer)));
				if (!framebuffer)
					break;
				framebuffers.push_back(framebuffer);
			}
			if (!texture || framebuffers.size() != layers)
			{
				m_Ready = false;
				// Ensure runs after the caller waits for idle. Do not retain an unusable pool
				// that could prevent a subsequent allocation from fitting in device memory.
				m_Framebuffers.clear();
				m_Texture = nullptr;
				if (!m_ReportedFailure)
					PX_CORE_ERROR("Local-shadow allocation failed; rendering local lights unshadowed until resources recover");
				m_ReportedFailure = true;
				return false;
			}
		}
		m_Framebuffers = std::move(framebuffers);
		m_Texture = std::move(texture);
		m_Resolution = resolution;
		m_RequestedLights = requestedLights;
		m_Ready = true;
		m_ReportedFailure = false;
		return true;
	}

	nvrhi::IFramebuffer* LocalShadowMaps::GetFramebuffer(uint32_t layer) const
	{
		return m_Ready && layer < m_Framebuffers.size() ? m_Framebuffers[layer].Get() : nullptr;
	}

}
