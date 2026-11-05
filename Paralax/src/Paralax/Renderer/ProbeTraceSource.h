#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Renderer/ProbeTraceTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Paralax {

	class Asset;
	class AssetManager;
	class MeshAsset;
	class Scene;
	class ProbeTraceScene;
	struct GraphicsDeviceLimits;
	struct ProbeTraceSceneIdentity;

	struct ProbeTraceLocalTriangle
	{
		glm::vec3 Position[3]{};
		glm::vec3 Normal[3]{};
		glm::vec2 UV[3]{};
		uint32_t OriginalTriangleOrdinal = 0;
	};

	struct ProbeTraceSourceInstance
	{
		uint64_t EntityUUID = 0;
		uint64_t OriginalSubmeshOrdinal = 0;
		size_t GeometryIndex = 0;
		uint32_t MaterialIndex = 0;
		uint32_t FirstCanonicalRank = 0;
		glm::mat4 World{1.0f};
		glm::mat3 NormalMatrix{1.0f};
		bool Mirrored = false;
		bool CastShadows = true;
	};

	// Immutable owned local values, borrowed from ProbeTraceSource on the main thread. Reacquire after every
	// Update, including unchanged or rejected updates. Source assets are retained only for identity, never read by materialization.
	class ProbeTraceSourceGeometry
	{
	public:
		ProbeTraceSourceGeometry(const ProbeTraceSourceGeometry&) = delete;
		ProbeTraceSourceGeometry& operator=(const ProbeTraceSourceGeometry&) = delete;
		ProbeTraceSourceGeometry(ProbeTraceSourceGeometry&&) = delete;
		ProbeTraceSourceGeometry& operator=(ProbeTraceSourceGeometry&&) = delete;
		uint64_t GetMeshHandle() const { return m_MeshHandle; }
		uint64_t GetMeshVersion() const { return m_MeshVersion; }
		uint64_t GetOriginalSubmeshOrdinal() const { return m_OriginalSubmeshOrdinal; }
		uint32_t GetIndexOffset() const { return m_IndexOffset; }
		uint32_t GetIndexCount() const { return m_IndexCount; }
		uint32_t GetVertexOffset() const { return m_VertexOffset; }
		uint32_t GetVertexCount() const { return m_VertexCount; }
		std::span<const ProbeTraceLocalTriangle> GetTriangles() const { return m_Triangles; }

	private:
		friend class ProbeTraceSource;
		ProbeTraceSourceGeometry() = default;
		Ref<const MeshAsset> m_MeshIdentity;
		uint64_t m_MeshHandle = 0, m_MeshVersion = 0;
		uint64_t m_OriginalSubmeshOrdinal = 0;
		uint32_t m_IndexOffset = 0, m_IndexCount = 0, m_VertexOffset = 0, m_VertexCount = 0;
		std::vector<ProbeTraceLocalTriangle> m_Triangles;
	};

	// Immutable semantic snapshot; main-thread-only, borrowed until any source Update. Retaining its small
	// scene token is safe; the snapshot itself cannot be copied to retain unbounded old geometry generations.
	class ProbeTraceSourceSnapshot
	{
	public:
		ProbeTraceSourceSnapshot(const ProbeTraceSourceSnapshot&) = delete;
		ProbeTraceSourceSnapshot& operator=(const ProbeTraceSourceSnapshot&) = delete;
		ProbeTraceSourceSnapshot(ProbeTraceSourceSnapshot&&) = delete;
		ProbeTraceSourceSnapshot& operator=(ProbeTraceSourceSnapshot&&) = delete;
		const Ref<const ProbeTraceSceneIdentity>& GetSceneIdentity() const { return m_SceneIdentity; }
		uint64_t GetGeneration() const { return m_Generation; }
		uint32_t GetExpandedTriangleCount() const { return m_ExpandedTriangleCount; }
		size_t GetGeometryCount() const { return m_Geometries.size(); }
		// Programmer precondition: index < GetGeometryCount(), verified in every build configuration.
		const ProbeTraceSourceGeometry& GetGeometry(size_t index) const;
		std::span<const ProbeTraceSourceInstance> GetInstances() const { return m_Instances; }
		const std::vector<GPU::ProbeTraceMaterial>& GetMaterials() const { return m_Materials; }
		const std::vector<glm::vec4>& GetTexels() const { return m_Texels; }

	private:
		friend class ProbeTraceSource;
		ProbeTraceSourceSnapshot() = default;
		Ref<const ProbeTraceSceneIdentity> m_SceneIdentity;
		uint64_t m_Generation = 0;
		uint32_t m_ExpandedTriangleCount = 0;
		std::vector<Ref<const ProbeTraceSourceGeometry>> m_Geometries;
		std::vector<ProbeTraceSourceInstance> m_Instances;
		std::vector<GPU::ProbeTraceMaterial> m_Materials;
		std::vector<glm::vec4> m_Texels;
		std::vector<Ref<const Asset>> m_IdentityAssets;
	};

	// Explicitly materialized world representation, borrowed on the main thread until any source Update.
	// Materials/texels belong to the matching semantic snapshot, avoiding a second decoded texture cache.
	class ProbeTraceLegacyRepresentation
	{
	public:
		ProbeTraceLegacyRepresentation(const ProbeTraceLegacyRepresentation&) = delete;
		ProbeTraceLegacyRepresentation& operator=(const ProbeTraceLegacyRepresentation&) = delete;
		ProbeTraceLegacyRepresentation(ProbeTraceLegacyRepresentation&&) = delete;
		ProbeTraceLegacyRepresentation& operator=(ProbeTraceLegacyRepresentation&&) = delete;
		uint64_t GetGeneration() const { return m_Generation; }
		const std::vector<GPU::ProbeTraceNode>& GetNodes() const { return m_Nodes; }
		const std::vector<GPU::ProbeTraceTriangle>& GetTriangles() const { return m_Triangles; }

	private:
		friend class ProbeTraceSource;
		ProbeTraceLegacyRepresentation() = default;
		uint64_t m_Generation = 0;
		std::vector<GPU::ProbeTraceNode> m_Nodes;
		std::vector<GPU::ProbeTraceTriangle> m_Triangles;
	};

	struct ProbeTraceSourceStatistics
	{
		uint64_t AcceptedChanges = 0;
		uint64_t AcceptedCacheHits = 0;
		uint64_t RejectedCacheHits = 0;
		uint64_t ValidationVertexVisits = 0;
		uint64_t UniqueGeometryCopies = 0;
		uint64_t CopiedLocalTriangles = 0;
		uint64_t ReusedGeometryReferences = 0;
		uint64_t LegacyExpandedTriangles = 0;
		uint64_t LegacySortCalls = 0;
		uint64_t LegacyBuiltNodes = 0;
	};

	// Main-thread-only source owner. Update captures accepted owned values without expanding/sorting a world
	// tree, and returns false for malformed/oversized input. Reacquire EVERY borrowed view after ANY Update.
	// Rejection retains old storage internally but exposes no current snapshot. Asset edits follow Version.
	class ProbeTraceSource
	{
	public:
		static constexpr uint32_t MaxTriangles = 1024 * 1024;
		static constexpr uint32_t MaxTexels = 16 * 1024 * 1024;
		static constexpr float MaxCoordinate = 1000000.0f;
		static constexpr float MaxMaterialValue = 1000000.0f;

		ProbeTraceSource();
		explicit ProbeTraceSource(const GraphicsDeviceLimits& limits);
		~ProbeTraceSource();
		ProbeTraceSource(const ProbeTraceSource&) = delete;
		ProbeTraceSource& operator=(const ProbeTraceSource&) = delete;
		bool Update(Scene& scene, AssetManager& assets, bool& changed);
		// Uses only captured values; false without current accepted input. Preserves semantic borrowed views
		// and content generation. Repeated calls reuse the same representation without expansion/sorting.
		bool MaterializeLegacy();
		const ProbeTraceSourceSnapshot* GetCurrentSnapshot() const;
		const ProbeTraceLegacyRepresentation* GetCurrentLegacy() const;
		// Cumulative work, including rejected staging attempts; no reset or invalidation side effects.
		ProbeTraceSourceStatistics GetStatistics() const { return m_Statistics; }

	private:
		friend class ProbeTraceScene;
		struct AcceptedState;
		enum class CaptureResult
		{
			Rejected,
			Unchanged,
			Changed
		};
		enum class RepresentationRequirement
		{
			SourceOnly,
			Legacy
		};
		enum class RejectionKind
		{
			Mesh,
			Material,
			BaseColorTexture,
			EmissiveTexture,
			MetallicRoughnessTexture,
			Transform,
			Budget
		};
		CaptureResult Prepare(Scene& scene, AssetManager& assets, Scope<AcceptedState>& candidate);
		bool BuildLegacy(AcceptedState& state);
		bool UpdateImpl(Scene& scene, AssetManager& assets, bool& changed, RepresentationRequirement requirement);
		void Commit(Scope<AcceptedState> candidate);
		void ClearRejection();
		const ProbeTraceSourceSnapshot* GetAcceptedSnapshot() const;
		const ProbeTraceLegacyRepresentation* GetAcceptedLegacy() const;

		Scope<AcceptedState> m_Current;
		bool m_CurrentInputAccepted = false;
		uint64_t m_MaxStorageBufferBytes;
		ProbeTraceSourceStatistics m_Statistics;
		Ref<const ProbeTraceSceneIdentity> m_RejectedSceneIdentity;
		std::vector<uint64_t> m_RejectedKey, m_RejectedTransform;
		// Weak guards retain only last-rejection control blocks, never rejected assets or geometry.
		std::vector<std::weak_ptr<const Asset>> m_RejectedDependencies;
		RejectionKind m_RejectedKind = RejectionKind::Mesh;
		std::vector<std::vector<uint64_t>> m_InvalidTransforms;
	};

}
