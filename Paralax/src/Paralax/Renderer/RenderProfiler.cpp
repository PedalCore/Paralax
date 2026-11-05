#include "pxpch.h"
#include "Paralax/Renderer/RenderProfiler.h"

#include "Paralax/Core/Assert.h"

#include <cmath>

namespace Paralax {

	RenderProfiler::RenderProfiler(nvrhi::IDevice* device, uint32_t timestampValidBits, float timestampPeriodNanoseconds, RenderQueryFactory* factory)
		: m_Device(device)
		, m_Factory(factory ? *factory : m_DefaultFactory)
	{
		// Pinned NVRHI reads 32-bit timestamp results. Shorter counters would need backend masking.
		if (timestampValidBits >= 32 && std::isfinite(timestampPeriodNanoseconds) && timestampPeriodNanoseconds > 0)
			m_WrapSeconds = 4294967296.0 * static_cast<double>(timestampPeriodNanoseconds) * 1e-9;
	}

	const char* RenderProfiler::GetSectionName(RenderTimingSection section)
	{
		constexpr std::array names = {"total", "shadows", "sceneIncludingAO", "ambientOcclusion", "bloom", "tonemap", "overlays", "antiAliasing", "depthHierarchy", "reflections", "clusterBuild"};
		const auto index = static_cast<size_t>(section);
		return index < names.size() ? names[index] : "invalid";
	}

	bool RenderProfiler::InitializeSlot(Slot& slot)
	{
		if (slot.Completion)
			return true;
		// Nothing has been recorded yet, so partial allocation can release indices immediately.
		for (auto& query : slot.Queries)
		{
			query = m_Factory.CreateTimer(m_Device);
			if (!query)
			{
				slot.Queries = {};
				return false;
			}
		}
		slot.Completion = m_Factory.CreateEvent(m_Device);
		if (!slot.Completion)
			slot.Queries = {};
		return bool(slot.Completion);
	}

	bool RenderProfiler::BeginFrame(bool enabled, nvrhi::ICommandList* commands, const RenderTimingContext& context, double hostSeconds)
	{
		PX_CORE_VERIFY(m_Recording == SlotCount, "A profiling frame is already being recorded");
		Poll(hostSeconds);
		if (!enabled)
		{
			m_Status = "disabled";
			return false;
		}
		if (m_WrapSeconds <= 0)
		{
			m_Status = "timestamps unavailable";
			return false;
		}
		for (size_t i = 0; i < m_Slots.size(); ++i)
		{
			auto& slot = m_Slots[i];
			if (slot.State != SlotState::Free)
				continue;
			if (!InitializeSlot(slot))
			{
				m_Status = "query allocation failed";
				return false;
			}
			slot.Context = context;
			slot.Used = {};
			slot.Open = {};
			slot.State = SlotState::Recording;
			m_Recording = i;
			m_Status = m_Latest ? "ready" : "pending";
			BeginSection(commands, RenderTimingSection::Total);
			return true;
		}
		++m_Skipped;
		m_Status = "skipped: query ring full";
		return false;
	}

	void RenderProfiler::BeginSection(nvrhi::ICommandList* commands, RenderTimingSection section)
	{
		if (m_Recording == SlotCount)
			return;
		auto& slot = m_Slots[m_Recording];
		const auto index = static_cast<size_t>(section);
		PX_CORE_VERIFY(index < SectionCount && !slot.Used[index], "Profiling sections may be recorded once per frame");
		m_Factory.Begin(commands, slot.Queries[index]);
		slot.Open[index] = true;
		slot.Used[index] = true;
	}

	void RenderProfiler::EndSection(nvrhi::ICommandList* commands, RenderTimingSection section)
	{
		if (m_Recording == SlotCount)
			return;
		auto& slot = m_Slots[m_Recording];
		const auto index = static_cast<size_t>(section);
		PX_CORE_VERIFY(index < SectionCount && slot.Open[index], "Profiling section must be open before ending");
		m_Factory.End(commands, slot.Queries[index]);
		slot.Open[index] = false;
	}

	void RenderProfiler::EndFrame(nvrhi::ICommandList* commands, uint32_t effectivePath, bool ambientOcclusionActive, bool bloomActive, uint32_t effectiveAA, bool reflectionsActive, bool reflectionHistoryActive, bool clusteredLightingActive)
	{
		if (m_Recording != SlotCount)
		{
			m_Slots[m_Recording].Context.EffectivePath = effectivePath;
			m_Slots[m_Recording].Context.AmbientOcclusionActive = ambientOcclusionActive;
			m_Slots[m_Recording].Context.BloomActive = bloomActive;
			m_Slots[m_Recording].Context.EffectiveAA = effectiveAA;
			m_Slots[m_Recording].Context.ReflectionsActive = reflectionsActive;
			m_Slots[m_Recording].Context.ReflectionHistoryActive = reflectionHistoryActive;
			m_Slots[m_Recording].Context.ClusteredLightingActive = clusteredLightingActive;
		}
		EndSection(commands, RenderTimingSection::Total);
	}

	void RenderProfiler::Submitted(double hostSeconds)
	{
		if (m_Recording == SlotCount)
			return;
		auto& slot = m_Slots[m_Recording];
		PX_CORE_VERIFY(std::none_of(slot.Open.begin(), slot.Open.end(), [](bool open) { return open; }), "Close all profiling sections before submission");
		slot.SubmittedAt = hostSeconds;
		// This event captures the new submission ID. Timer availability alone may still describe old data.
		m_Factory.SetEvent(m_Device, slot.Completion);
		slot.State = SlotState::Submitted;
		m_Recording = SlotCount;
	}

	void RenderProfiler::AbandonFrame()
	{
		if (m_Recording == SlotCount)
			return;
		m_Slots[m_Recording].State = SlotState::Abandoned;
		m_Recording = SlotCount;
		m_Status = "invalid: recording abandoned";
	}

	void RenderProfiler::Poll(double hostSeconds)
	{
		for (auto& slot : m_Slots)
		{
			if (slot.State != SlotState::Submitted || !m_Factory.PollEvent(m_Device, slot.Completion))
				continue;
			bool ready = true;
			for (size_t i = 0; i < SectionCount; ++i)
				if (slot.Used[i] && !m_Factory.PollTimer(m_Device, slot.Queries[i]))
					ready = false;
			if (!ready)
				continue;
			RenderTimingSample sample;
			sample.Context = slot.Context;
			const double elapsed = hostSeconds - slot.SubmittedAt;
			bool valid = std::isfinite(elapsed) && elapsed >= 0 && elapsed < m_WrapSeconds;
			for (size_t i = 0; i < SectionCount; ++i)
			{
				if (!slot.Used[i])
					continue;
				const double seconds = static_cast<double>(m_Factory.ReadSeconds(m_Device, slot.Queries[i]));
				valid &= std::isfinite(seconds) && seconds >= 0 && seconds < m_WrapSeconds;
				sample.Milliseconds[i] = seconds * 1000.0;
				m_Factory.Reset(m_Device, slot.Queries[i]);
			}
			if (valid && (!m_Latest || sample.Context.Frame >= m_Latest->Context.Frame))
			{
				m_Latest = std::move(sample);
				m_LastResultStatus = "ready";
				m_Status = "ready";
			}
			else if (!valid)
			{
				++m_Invalid;
				m_LastResultStatus = "invalid: timestamp interval";
				m_Status = m_LastResultStatus;
			}
			m_Factory.ResetEvent(m_Device, slot.Completion);
			slot.State = SlotState::Free;
		}
	}

	uint32_t RenderProfiler::GetPendingCount() const
	{
		return static_cast<uint32_t>(std::count_if(m_Slots.begin(), m_Slots.end(), [](const Slot& slot) { return slot.State != SlotState::Free; }));
	}

}
