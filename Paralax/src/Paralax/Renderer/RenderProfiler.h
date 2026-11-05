#pragma once

#include "Paralax/Renderer/GraphicsDevice.h"

#include <array>
#include <optional>
#include <string>

namespace Paralax {

	enum class RenderTimingSection : size_t
	{
		Total,
		Shadows,
		Scene,
		AmbientOcclusion,
		Bloom,
		Tonemap,
		Overlays,
		AntiAliasing,
		DepthHierarchy,
		Reflections,
		ClusterBuild,
		Count
	};

	struct RenderTimingContext
	{
		uint64_t Frame = 0;
		uint32_t InternalWidth = 0, InternalHeight = 0, OutputWidth = 0, OutputHeight = 0;
		uint32_t RequestedPath = 0, EffectivePath = 0, EffectiveAA = 0;
		bool AmbientOcclusionActive = false, BloomActive = false, ReflectionsActive = false, ReflectionHistoryActive = false, ClusteredLightingActive = false;
		std::string Settings;
	};

	struct RenderTimingSample
	{
		RenderTimingContext Context;
		// Missing sections were not recorded, rather than measured as zero. Scene includes AO and reflections; reflections includes the depth hierarchy. ClusterBuild is outside Scene, inside Total.
		std::array<std::optional<double>, static_cast<size_t>(RenderTimingSection::Count)> Milliseconds;
	};

	// Non-owning, main-thread-only query seam; used to test submission/availability races without wall-clock tests.
	class RenderQueryFactory
	{
	public:
		virtual ~RenderQueryFactory() = default;
		virtual nvrhi::TimerQueryHandle CreateTimer(nvrhi::IDevice* device) { return device->createTimerQuery(); }
		virtual nvrhi::EventQueryHandle CreateEvent(nvrhi::IDevice* device) { return device->createEventQuery(); }
		virtual void Begin(nvrhi::ICommandList* commands, nvrhi::ITimerQuery* query) { commands->beginTimerQuery(query); }
		virtual void End(nvrhi::ICommandList* commands, nvrhi::ITimerQuery* query) { commands->endTimerQuery(query); }
		virtual void SetEvent(nvrhi::IDevice* device, nvrhi::IEventQuery* query) { device->setEventQuery(query, nvrhi::CommandQueue::Graphics); }
		virtual void ResetEvent(nvrhi::IDevice* device, nvrhi::IEventQuery* query) { device->resetEventQuery(query); }
		virtual bool PollEvent(nvrhi::IDevice* device, nvrhi::IEventQuery* query) { return device->pollEventQuery(query); }
		virtual bool PollTimer(nvrhi::IDevice* device, nvrhi::ITimerQuery* query) { return device->pollTimerQuery(query); }
		virtual float ReadSeconds(nvrhi::IDevice* device, nvrhi::ITimerQuery* query) { return device->getTimerQueryTime(query); }
		virtual void Reset(nvrhi::IDevice* device, nvrhi::ITimerQuery* query) { device->resetTimerQuery(query); }
	};

	// Main-thread-only bounded, nonblocking GPU profiling. Device/factory and submitted command resources outlive it.
	// Owner waits for GPU idle and discards unsubmitted command lists before destruction. Disable never frees pending queries.
	class RenderProfiler
	{
	public:
		static constexpr size_t SlotCount = 4;
		static constexpr size_t SectionCount = static_cast<size_t>(RenderTimingSection::Count);
		RenderProfiler(nvrhi::IDevice* device, uint32_t timestampValidBits, float timestampPeriodNanoseconds, RenderQueryFactory* factory = nullptr);
		RenderProfiler(const RenderProfiler&) = delete;
		RenderProfiler& operator=(const RenderProfiler&) = delete;
		RenderProfiler(RenderProfiler&&) = delete;
		RenderProfiler& operator=(RenderProfiler&&) = delete;
		// Polls prior submissions even when disabled. Full rings skip this sample, never wait.
		bool BeginFrame(bool enabled, nvrhi::ICommandList* commands, const RenderTimingContext& context, double hostSeconds);
		void BeginSection(nvrhi::ICommandList* commands, RenderTimingSection section);
		void EndSection(nvrhi::ICommandList* commands, RenderTimingSection section);
		void EndFrame(nvrhi::ICommandList* commands, uint32_t effectivePath, bool ambientOcclusionActive = false, bool bloomActive = false, uint32_t effectiveAA = 0, bool reflectionsActive = false, bool reflectionHistoryActive = false, bool clusteredLightingActive = false);
		// Call immediately AFTER executeCommandList; hostSeconds was measured BEFORE that submission call.
		void Submitted(double hostSeconds);
		// Retain abandoned query indices until owner destruction; an unsubmitted command may still reference them.
		void AbandonFrame();
		void Poll(double hostSeconds);
		const std::optional<RenderTimingSample>& GetLatest() const { return m_Latest; }
		const char* GetStatus() const { return m_Status; }
		uint32_t GetPendingCount() const;
		uint64_t GetSkippedCount() const { return m_Skipped; }
		uint64_t GetInvalidCount() const { return m_Invalid; }
		const char* GetLastResultStatus() const { return m_LastResultStatus; }
		static const char* GetSectionName(RenderTimingSection section);

	private:
		enum class SlotState
		{
			Free,
			Recording,
			Submitted,
			Abandoned
		};
		struct Slot
		{
			std::array<nvrhi::TimerQueryHandle, SectionCount> Queries;
			nvrhi::EventQueryHandle Completion;
			std::array<bool, SectionCount> Used{}, Open{};
			RenderTimingContext Context;
			double SubmittedAt = 0;
			SlotState State = SlotState::Free;
		};
		bool InitializeSlot(Slot& slot);
		nvrhi::IDevice* m_Device;
		RenderQueryFactory m_DefaultFactory;
		RenderQueryFactory& m_Factory;
		std::array<Slot, SlotCount> m_Slots;
		std::optional<RenderTimingSample> m_Latest;
		size_t m_Recording = SlotCount;
		double m_WrapSeconds = 0;
		uint64_t m_Skipped = 0, m_Invalid = 0;
		const char* m_LastResultStatus = "none";
		const char* m_Status = "disabled";
	};

}
