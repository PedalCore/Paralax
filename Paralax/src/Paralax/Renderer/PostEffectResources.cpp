#include "pxpch.h"
#include "Paralax/Renderer/PostEffectResources.h"

#include "Paralax/Core/Assert.h"

#include <limits>
#include <utility>

namespace Paralax {

	PostEffectBudget::Reservation::~Reservation()
	{
		Reset();
	}

	PostEffectBudget::Reservation::Reservation(Reservation&& other) noexcept
		: m_Budget(std::exchange(other.m_Budget, nullptr))
		, m_Bytes(std::exchange(other.m_Bytes, 0))
	{
	}

	PostEffectBudget::Reservation& PostEffectBudget::Reservation::operator=(Reservation&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Budget = std::exchange(other.m_Budget, nullptr);
			m_Bytes = std::exchange(other.m_Bytes, 0);
		}
		return *this;
	}

	void PostEffectBudget::Reservation::Reset()
	{
		if (m_Budget)
		{
			PX_CORE_VERIFY(m_Bytes <= m_Budget->m_Used, "Invalid post-effect memory reservation");
			m_Budget->m_Used -= m_Bytes;
			m_Budget = nullptr;
			m_Bytes = 0;
		}
	}

	PostEffectBudget::Reservation PostEffectBudget::Reserve(uint64_t bytes)
	{
		if (!Fits(bytes))
			return {};
		m_Used += bytes;
		m_Peak = std::max(m_Peak, m_Used);
		return Reservation(*this, bytes);
	}

	PostEffectBudget::~PostEffectBudget()
	{
		CollectRetired();
		PX_CORE_VERIFY(m_Retired.empty() && m_Used == 0, "Post-effect budget must outlive all resource references and reservations");
	}

	void PostEffectBudget::Retire(Reservation memory, nvrhi::RefCountPtr<nvrhi::IResource> resource)
	{
		m_Retired.push_back({std::move(memory), std::move(resource)});
	}

	void PostEffectBudget::CollectRetired()
	{
		for (auto it = m_Retired.begin(); it != m_Retired.end();)
		{
			if (it->Resource->GetRefCount() != 1)
			{
				++it;
				continue;
			}
			// Destroy backing before releasing its charge, including during vector compaction.
			it->Resource = nullptr;
			it->Memory.Reset();
			it = m_Retired.erase(it);
		}
	}

	PostEffectTextures::PostEffectTextures(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Budget(budget)
		, m_Factory(factory ? *factory : m_DefaultFactory)
	{
	}

	PostEffectTextures::~PostEffectTextures()
	{
		Clear();
	}

	void PostEffectTextures::TrackUsage(nvrhi::ICommandList* commands, nvrhi::ITexture* texture, nvrhi::ResourceStates state)
	{
		PX_CORE_VERIFY(commands && texture, "Effect usage requires an open command list and texture");
		commands->setTextureState(texture, nvrhi::AllSubresources, state);
	}

	void PostEffectTextures::Clear()
	{
		if (!m_Allocations.empty())
		{
			m_Graphics.WaitForIdle();
			m_Graphics.CollectGarbage();
			for (auto& allocation : m_Allocations)
				m_Budget.Retire(std::move(allocation.Reservation), std::move(allocation.Texture));
			m_Allocations.clear();
			m_Budget.CollectRetired();
		}
	}

	uint64_t PostEffectTextures::GetOwnedBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& allocation : m_Allocations)
			bytes += allocation.Reservation.GetBytes();
		return bytes;
	}

	nvrhi::ITexture* PostEffectTextures::Create(const nvrhi::TextureDesc& desc)
	{
		m_FailureReason = "invalid effect texture description";
		if (!desc.width || !desc.height || desc.width > 8192 || desc.height > 8192 || desc.depth != 1 || desc.arraySize != 1 ||
			desc.sampleCount != 1 || desc.dimension != nvrhi::TextureDimension::Texture2D || desc.isTiled ||
			desc.format == nvrhi::Format::UNKNOWN || desc.mipLevels == 0)
			return nullptr;
		uint32_t maxMips = 1;
		for (uint32_t size = std::max(desc.width, desc.height); size > 1; size /= 2)
			++maxMips;
		if (desc.mipLevels > maxMips)
			return nullptr;
		auto* device = m_Graphics.GetDevice();
		m_FailureReason = "effect memory preflight unsupported";
		if (!m_Factory.SupportsVirtualResources(device))
			return nullptr;
		auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderLoad;
		if (desc.isRenderTarget)
			required = required | (nvrhi::getFormatInfo(desc.format).hasDepth ? nvrhi::FormatSupport::DepthStencil : nvrhi::FormatSupport::RenderTarget);
		if (desc.isUAV)
			required = required | nvrhi::FormatSupport::ShaderUavStore;
		m_FailureReason = "effect texture format unsupported";
		if ((m_Factory.QueryFormatSupport(device, desc.format) & required) != required)
			return nullptr;

		Allocation allocation;
		auto virtualDesc = desc;
		virtualDesc.isVirtual = true;
		++m_AllocationAttempts;
		allocation.Texture = m_Factory.CreateTexture(device, virtualDesc);
		m_FailureReason = "effect texture creation failed";
		if (!allocation.Texture)
			return nullptr;
		const auto requirements = m_Factory.GetTextureMemoryRequirements(device, allocation.Texture);
		m_FailureReason = "invalid effect memory requirements";
		if (!requirements.size || !requirements.alignment || requirements.size > std::numeric_limits<uint64_t>::max() - (requirements.alignment - 1))
			return nullptr;
		const uint64_t bytes = ((requirements.size + requirements.alignment - 1) / requirements.alignment) * requirements.alignment;
		allocation.Reservation = m_Budget.Reserve(bytes);
		m_FailureReason = "renderer effect memory budget exhausted";
		if (!allocation.Reservation)
			return nullptr;
		allocation.Texture = nullptr;
		auto committedDesc = desc;
		committedDesc.isVirtual = false;
		++m_AllocationAttempts;
		++m_BackingAttempts;
		allocation.Texture = m_Factory.CreateTexture(device, committedDesc);
		m_FailureReason = "effect backing allocation failed";
		if (!allocation.Texture)
			return nullptr;
		// Vulkan guarantees identical sizes for identical image creation parameters. NVRHI's
		// isVirtual changes allocation only; committed creation selects compatible memory itself.
		// The pinned backend does not propagate bind errors, so driver/device-loss recovery is
		// outside this factory's null-handle failure contract.
		PX_CORE_VERIFY(device->getTextureMemoryRequirements(allocation.Texture).size == requirements.size, "Effect memory preflight descriptor mismatch");
		auto* texture = allocation.Texture.Get();
		m_Allocations.push_back(std::move(allocation));
		m_FailureReason = "";
		return texture;
	}

	PostEffectBuffers::PostEffectBuffers(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory)
		: m_Graphics(graphics)
		, m_Budget(budget)
		, m_Factory(factory ? *factory : m_DefaultFactory)
	{
	}

	PostEffectBuffers::~PostEffectBuffers()
	{
		Clear();
	}

	void PostEffectBuffers::Clear()
	{
		if (m_Allocations.empty())
			return;
		m_Graphics.WaitForIdle();
		m_Graphics.CollectGarbage();
		for (auto& allocation : m_Allocations)
			m_Budget.Retire(std::move(allocation.Reservation), std::move(allocation.Buffer));
		m_Allocations.clear();
		m_Budget.CollectRetired();
	}

	uint64_t PostEffectBuffers::GetOwnedBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& allocation : m_Allocations)
			bytes += allocation.Reservation.GetBytes();
		return bytes;
	}

	nvrhi::IBuffer* PostEffectBuffers::Create(uint64_t bytes, uint32_t stride, const char* name)
	{
		m_FailureReason = "invalid effect buffer description";
		if (!name || !bytes || !stride || stride % 4 || bytes % stride || bytes > PostEffectBudget::DefaultCapacity)
			return nullptr;
		auto* device = m_Graphics.GetDevice();
		m_FailureReason = "effect buffer preflight unsupported";
		if (!m_Factory.SupportsVirtualResources(device))
			return nullptr;
		nvrhi::BufferDesc desc;
		desc.byteSize = bytes;
		desc.structStride = stride;
		desc.debugName = name;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.isVirtual = true;
		Allocation allocation;
		++m_AllocationAttempts;
		allocation.Buffer = m_Factory.CreateBuffer(device, desc);
		m_FailureReason = "effect buffer creation failed";
		if (!allocation.Buffer)
			return nullptr;
		const auto requirements = m_Factory.GetBufferMemoryRequirements(device, allocation.Buffer);
		m_FailureReason = "invalid effect buffer memory requirements";
		if (!requirements.size || !requirements.alignment || requirements.size > std::numeric_limits<uint64_t>::max() - (requirements.alignment - 1))
			return nullptr;
		const uint64_t reserved = ((requirements.size + requirements.alignment - 1) / requirements.alignment) * requirements.alignment;
		allocation.Reservation = m_Budget.Reserve(reserved);
		m_FailureReason = "renderer effect memory budget exhausted";
		if (!allocation.Reservation)
			return nullptr;
		allocation.Buffer = nullptr;
		desc.isVirtual = false;
		++m_AllocationAttempts;
		++m_BackingAttempts;
		allocation.Buffer = m_Factory.CreateBuffer(device, desc);
		m_FailureReason = "effect buffer backing allocation failed";
		if (!allocation.Buffer)
			return nullptr;
		// The pinned backend changes allocation only for nonvolatile isVirtual buffers.
		PX_CORE_VERIFY(device->getBufferMemoryRequirements(allocation.Buffer).size == requirements.size, "Effect buffer preflight descriptor mismatch");
		auto* result = allocation.Buffer.Get();
		m_Allocations.push_back(std::move(allocation));
		m_FailureReason = "";
		return result;
	}

}
