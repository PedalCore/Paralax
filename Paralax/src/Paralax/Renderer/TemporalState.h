#pragma once

#include "Paralax/Core/Base.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Paralax {

	struct TemporalDrawKey
	{
		uint64_t Entity = 0, Mesh = 0;
		uint32_t Submesh = 0;
		bool operator==(const TemporalDrawKey&) const = default;
	};

	struct TemporalDrawSnapshot
	{
		TemporalDrawKey Key;
		glm::mat4 World{1};
		uint64_t Version = 0;
		bool Deformed = false;
	};

	struct TemporalFrameSnapshot
	{
		Ref<const void> Scene;
		glm::mat4 View{1}, Projection{1};
		glm::uvec2 InternalSize{0}, OutputSize{0};
		uint64_t Settings = 0, CameraIdentity = 0;
		bool CameraCut = false;
	};

	struct TemporalDrawRecord
	{
		glm::mat4 PreviousWorld{1};
		uint32_t ID = 0;
		bool Reactive = true;
	};

	// Main-thread-only transactional history metadata. Scene tokens are retained strongly; no scene is owned.
	// Stage never advances the completed frame. Commit follows successful GPU submission; Abort rejects history.
	class TemporalState
	{
	public:
		static constexpr uint32_t MaxDraws = 65536;
		TemporalState() = default;
		TemporalState(const TemporalState&) = delete;
		TemporalState& operator=(const TemporalState&) = delete;
		bool Stage(const TemporalFrameSnapshot& frame, std::span<const TemporalDrawSnapshot> draws);
		void Commit();
		void Abort(const char* reason);
		void Reset(const char* reason);
		bool IsValid() const { return m_Valid; }
		bool IsStaged() const { return m_Staged; }
		bool CanUseHistory() const { return m_Staged && m_UseHistory; }
		uint64_t GetCompletedFrames() const { return m_CompletedFrames; }
		uint32_t GetSampleIndex() const { return m_SampleIndex; }
		glm::vec2 GetJitter() const;
		const TemporalFrameSnapshot& GetPreviousFrame() const { return m_PreviousFrame; }
		const std::vector<TemporalDrawRecord>& GetDrawRecords() const { return m_DrawRecords; }
		const std::string& GetResetReason() const { return m_Reason; }
		static glm::mat4 JitterProjection(const glm::mat4& projection, glm::uvec2 size, glm::vec2 jitter);

	private:
		struct KeyHash
		{
			size_t operator()(const TemporalDrawKey& key) const;
		};
		struct Entry
		{
			TemporalDrawSnapshot Snapshot;
			uint32_t ID = 0;
		};
		using Entries = std::unordered_map<TemporalDrawKey, Entry, KeyHash>;
		Entries m_Previous, m_Pending;
		TemporalFrameSnapshot m_PreviousFrame, m_PendingFrame;
		std::vector<TemporalDrawRecord> m_DrawRecords;
		uint32_t m_NextID = 1, m_PendingNextID = 1, m_SampleIndex = 0;
		uint64_t m_CompletedFrames = 0;
		bool m_Valid = false, m_Staged = false, m_UseHistory = false;
		std::string m_Reason = "new history";
	};

}
