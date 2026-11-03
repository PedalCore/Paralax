#pragma once

#include "Paralax/Renderer/GraphicsDevice.h"

#include <vector>

namespace Paralax {

	// Main-thread-only backing-memory budget shared by all effects and view histories of one renderer.
	// Reservations count staged and in-flight allocations too. The budget outlives every reservation.
	class PostEffectBudget
	{
	public:
		static constexpr uint64_t DefaultCapacity = 512ull * 1024 * 1024;

		class Reservation
		{
		public:
			Reservation() = default;
			~Reservation();
			Reservation(const Reservation&) = delete;
			Reservation& operator=(const Reservation&) = delete;
			Reservation(Reservation&& other) noexcept;
			Reservation& operator=(Reservation&& other) noexcept;
			explicit operator bool() const { return m_Budget != nullptr; }
			uint64_t GetBytes() const { return m_Bytes; }
			// Release only after the corresponding backing allocation and its GPU uses have been destroyed.
			void Reset();

		private:
			friend class PostEffectBudget;
			Reservation(PostEffectBudget& budget, uint64_t bytes)
				: m_Budget(&budget)
				, m_Bytes(bytes)
			{
			}
			PostEffectBudget* m_Budget = nullptr;
			uint64_t m_Bytes = 0;
		};

		explicit PostEffectBudget(uint64_t capacity = DefaultCapacity)
			: m_Capacity(capacity)
		{
		}
		~PostEffectBudget();
		PostEffectBudget(const PostEffectBudget&) = delete;
		PostEffectBudget& operator=(const PostEffectBudget&) = delete;
		// Zero or an exhausted capacity returns an empty reservation without changing accounting.
		Reservation Reserve(uint64_t bytes);
		// Retired textures and buffers stay charged while a binding, framebuffer or command list still retains them.
		// Call after device garbage collection to reclaim completed command-buffer references.
		void CollectRetired();
		uint64_t GetCapacity() const { return m_Capacity; }
		uint64_t GetUsedBytes() const { return m_Used; }
		uint64_t GetPeakBytes() const { return m_Peak; }
		bool Fits(uint64_t bytes) const { return bytes > 0 && bytes <= m_Capacity - m_Used; }

	private:
		friend class PostEffectTextures;
		friend class PostEffectBuffers;
		struct RetiredResource
		{
			Reservation Memory;
			nvrhi::RefCountPtr<nvrhi::IResource> Resource;
		};
		void Retire(Reservation memory, nvrhi::RefCountPtr<nvrhi::IResource> resource);
		uint64_t m_Capacity;
		uint64_t m_Used = 0, m_Peak = 0;
		std::vector<RetiredResource> m_Retired;
	};

	// Non-owning, main-thread-only allocation seam. Null handles/false report recoverable failures.
	class PostEffectResourceFactory
	{
	public:
		virtual ~PostEffectResourceFactory() = default;
		virtual bool SupportsVirtualResources(nvrhi::IDevice* device) { return device->queryFeatureSupport(nvrhi::Feature::VirtualResources); }
		virtual nvrhi::FormatSupport QueryFormatSupport(nvrhi::IDevice* device, nvrhi::Format format) { return device->queryFormatSupport(format); }
		virtual nvrhi::TextureHandle CreateTexture(nvrhi::IDevice* device, const nvrhi::TextureDesc& desc) { return device->createTexture(desc); }
		virtual nvrhi::MemoryRequirements GetTextureMemoryRequirements(nvrhi::IDevice* device, nvrhi::ITexture* texture) { return device->getTextureMemoryRequirements(texture); }
		virtual nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc) { return device->createBuffer(desc); }
		virtual nvrhi::MemoryRequirements GetBufferMemoryRequirements(nvrhi::IDevice* device, nvrhi::IBuffer* buffer) { return device->getBufferMemoryRequirements(buffer); }
		virtual nvrhi::SamplerHandle CreateSampler(nvrhi::IDevice* device, const nvrhi::SamplerDesc& desc) { return device->createSampler(desc); }
		virtual nvrhi::BindingLayoutHandle CreateBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc) { return device->createBindingLayout(desc); }
		virtual nvrhi::FramebufferHandle CreateFramebuffer(nvrhi::IDevice* device, const nvrhi::FramebufferDesc& desc) { return device->createFramebuffer(desc); }
		virtual nvrhi::BindingSetHandle CreateBindingSet(nvrhi::IDevice* device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) { return device->createBindingSet(desc, layout); }
		virtual nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IDevice* device, const nvrhi::GraphicsPipelineDesc& desc, const nvrhi::FramebufferInfo& info) { return device->createGraphicsPipeline(desc, info); }
	};

	// Owns optional committed 2D textures and their shared budget reservations. Main-thread-only.
	// Graphics, budget and optional factory outlive this object. Returned pointers are non-owning.
	// Clear waits for GPU retirement; externally retained textures remain charged to the budget.
	// Unsubmitted commands must retain textures explicitly (setTextureState); NVRHI clears alone do not.
	// Release external references and collect retired textures before destroying the budget.
	class PostEffectTextures
	{
	public:
		PostEffectTextures(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~PostEffectTextures();
		PostEffectTextures(const PostEffectTextures&) = delete;
		PostEffectTextures& operator=(const PostEffectTextures&) = delete;
		// Queries actual memory size without backing allocation, reserves it, then creates a committed texture.
		// Failure leaves previous allocations intact and supplies a reason; callers roll back partial groups with Clear.
		nvrhi::ITexture* Create(const nvrhi::TextureDesc& desc);
		// Retains a raw texture through command submission and records the required barrier.
		// Use before clears/copies/manual accesses; framebuffer and binding-set draws retain their inputs.
		static void TrackUsage(nvrhi::ICommandList* commands, nvrhi::ITexture* texture, nvrhi::ResourceStates state);
		void Clear();
		uint64_t GetOwnedBytes() const;
		uint64_t GetAllocationAttempts() const { return m_AllocationAttempts; }
		uint64_t GetBackingAttempts() const { return m_BackingAttempts; }
		const char* GetFailureReason() const { return m_FailureReason; }

	private:
		struct Allocation
		{
			// Reverse destruction order releases backing memory before its reservation.
			PostEffectBudget::Reservation Reservation;
			nvrhi::TextureHandle Texture;
		};
		GraphicsDevice& m_Graphics;
		PostEffectBudget& m_Budget;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		std::vector<Allocation> m_Allocations;
		uint64_t m_AllocationAttempts = 0, m_BackingAttempts = 0;
		const char* m_FailureReason = "";
	};

	// Main-thread-only structured-buffer allocations under the same peak-live budget as textures.
	// Graphics/budget/factory outlive it. Clear waits for retirement; external references stay charged.
	class PostEffectBuffers
	{
	public:
		PostEffectBuffers(GraphicsDevice& graphics, PostEffectBudget& budget, PostEffectResourceFactory* factory = nullptr);
		~PostEffectBuffers();
		PostEffectBuffers(const PostEffectBuffers&) = delete;
		PostEffectBuffers& operator=(const PostEffectBuffers&) = delete;
		// Ordinary GPU-only structured buffers only. Invalid input or allocation failure returns null.
		nvrhi::IBuffer* Create(uint64_t bytes, uint32_t stride, const char* name);
		void Clear();
		uint64_t GetOwnedBytes() const;
		uint64_t GetAllocationAttempts() const { return m_AllocationAttempts; }
		uint64_t GetBackingAttempts() const { return m_BackingAttempts; }
		const char* GetFailureReason() const { return m_FailureReason; }

	private:
		struct Allocation
		{
			PostEffectBudget::Reservation Reservation;
			nvrhi::BufferHandle Buffer;
		};
		GraphicsDevice& m_Graphics;
		PostEffectBudget& m_Budget;
		PostEffectResourceFactory m_DefaultFactory;
		PostEffectResourceFactory& m_Factory;
		std::vector<Allocation> m_Allocations;
		uint64_t m_AllocationAttempts = 0, m_BackingAttempts = 0;
		const char* m_FailureReason = "";
	};

}
