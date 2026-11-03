#include "pxpch.h"
#include "Paralax/Renderer/TemporalState.h"

#include "Paralax/Core/Assert.h"

#include <cmath>
#include <limits>

namespace Paralax {

	namespace {

		bool FiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					if (!std::isfinite(matrix[column][row]))
						return false;
			return true;
		}

		bool InvertibleFiniteMatrix(const glm::mat4& matrix)
		{
			if (!FiniteMatrix(matrix))
				return false;
			const float determinant = glm::determinant(matrix);
			return std::isfinite(determinant) && determinant != 0 && FiniteMatrix(glm::inverse(matrix));
		}

		float Halton(uint32_t index, uint32_t base)
		{
			float result = 0, fraction = 1;
			while (index)
			{
				fraction /= static_cast<float>(base);
				result += fraction * static_cast<float>(index % base);
				index /= base;
			}
			return result;
		}

	}

	size_t TemporalState::KeyHash::operator()(const TemporalDrawKey& key) const
	{
		uint64_t hash = key.Entity;
		for (uint64_t value : {key.Mesh, static_cast<uint64_t>(key.Submesh)})
			hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		return static_cast<size_t>(hash);
	}

	glm::vec2 TemporalState::GetJitter() const
	{
		if (m_SampleIndex == 0)
			return glm::vec2(0);
		return {Halton(m_SampleIndex, 2) - 0.5f, Halton(m_SampleIndex, 3) - 0.5f};
	}

	glm::mat4 TemporalState::JitterProjection(const glm::mat4& projection, glm::uvec2 size, glm::vec2 jitter)
	{
		PX_CORE_VERIFY(size.x && size.y && std::isfinite(jitter.x) && std::isfinite(jitter.y), "Temporal jitter requires nonzero dimensions and finite displacement");
		glm::mat4 result = projection;
		const glm::vec2 ndc(2 * jitter.x / static_cast<float>(size.x), -2 * jitter.y / static_cast<float>(size.y));
		for (int column = 0; column < 4; ++column)
		{
			result[column][0] += ndc.x * projection[column][3];
			result[column][1] += ndc.y * projection[column][3];
		}
		return result;
	}

	void TemporalState::Reset(const char* reason)
	{
		m_Valid = false;
		m_UseHistory = false;
		m_Staged = false;
		m_SampleIndex = 0;
		m_Reason = reason;
		m_Pending.clear();
		m_DrawRecords.clear();
		m_PendingFrame = {};
	}

	void TemporalState::Abort(const char* reason)
	{
		// No pending camera, IDs or transforms can leak into the next committed frame.
		Reset(reason);
	}

	bool TemporalState::Stage(const TemporalFrameSnapshot& frame, std::span<const TemporalDrawSnapshot> draws)
	{
		PX_CORE_VERIFY(!m_Staged, "Temporal history already has an uncommitted frame");
		if (!frame.Scene || !frame.InternalSize.x || !frame.InternalSize.y || !frame.OutputSize.x || !frame.OutputSize.y ||
			!InvertibleFiniteMatrix(frame.View) || !InvertibleFiniteMatrix(frame.Projection))
		{
			Abort("invalid temporal camera or dimensions");
			return false;
		}
		if (draws.size() > MaxDraws)
		{
			Abort("temporal draw capacity exceeded");
			return false;
		}
		if (m_Valid)
		{
			if (frame.Scene != m_PreviousFrame.Scene)
				Reset("scene replaced");
			else if (frame.CameraIdentity != m_PreviousFrame.CameraIdentity)
				Reset("camera identity changed");
			else if (frame.InternalSize != m_PreviousFrame.InternalSize || frame.OutputSize != m_PreviousFrame.OutputSize)
				Reset("view dimensions changed");
			else if (frame.Settings != m_PreviousFrame.Settings)
				Reset("render settings changed");
			else if (frame.Projection != m_PreviousFrame.Projection)
				Reset("projection changed");
			else
			{
				const auto previousWorld = glm::inverse(m_PreviousFrame.View);
				const auto currentWorld = glm::inverse(frame.View);
				if (frame.CameraCut || glm::distance(glm::vec3(previousWorld[3]), glm::vec3(currentWorld[3])) > 5 ||
					glm::dot(glm::normalize(glm::vec3(previousWorld[2])), glm::normalize(glm::vec3(currentWorld[2]))) < 0.5f)
					Reset("camera cut");
			}
		}
		m_Pending.clear();
		m_Pending.reserve(draws.size());
		m_DrawRecords.clear();
		m_DrawRecords.reserve(draws.size());
		m_PendingNextID = m_NextID;
		bool reuse = false;
		for (const auto& draw : draws)
		{
			if (!FiniteMatrix(draw.World) || m_Pending.contains(draw.Key))
			{
				Abort("invalid or duplicate temporal draw");
				return false;
			}
			const auto previous = m_Previous.find(draw.Key);
			const bool exists = previous != m_Previous.end();
			if (!exists && m_PendingNextID == 0x80000000u)
				reuse = true;
			const uint32_t id = exists ? previous->second.ID : m_PendingNextID++;
			m_Pending.emplace(draw.Key, Entry{draw, id});
		}
		for (const auto& [key, entry] : m_Previous)
		{
			(void)entry;
			if (!m_Pending.contains(key))
			{
				reuse = true;
				break;
			}
		}
		if (reuse || frame.Scene != m_PreviousFrame.Scene)
		{
			if (frame.Scene != m_PreviousFrame.Scene)
				m_Reason = "scene replaced";
			else if (m_Valid)
				m_Reason = "surface table changed";
			m_Valid = false;
			m_SampleIndex = 0;
			m_PendingNextID = 1;
			// Assign in draw order, never unordered-map iteration order.
			for (const auto& draw : draws)
				m_Pending.at(draw.Key).ID = m_PendingNextID++;
		}
		for (const auto& draw : draws)
		{
			const auto previous = m_Previous.find(draw.Key);
			const bool compatible = m_Valid && previous != m_Previous.end() && previous->second.Snapshot.Version == draw.Version &&
									!draw.Deformed && !previous->second.Snapshot.Deformed && glm::mat3(previous->second.Snapshot.World) == glm::mat3(draw.World);
			m_DrawRecords.push_back({compatible ? previous->second.Snapshot.World : draw.World, m_Pending.at(draw.Key).ID, !compatible});
		}
		m_UseHistory = m_Valid;
		m_PendingFrame = frame;
		m_Staged = true;
		return true;
	}

	void TemporalState::Commit()
	{
		PX_CORE_VERIFY(m_Staged, "Temporal history commit requires a completely staged frame");
		m_Previous.swap(m_Pending);
		m_Pending.clear();
		m_PreviousFrame = m_PendingFrame;
		m_PendingFrame = {};
		m_NextID = m_PendingNextID;
		m_Valid = true;
		m_Staged = false;
		m_UseHistory = false;
		m_SampleIndex = (m_SampleIndex + 1) % 8;
		++m_CompletedFrames;
	}

}
