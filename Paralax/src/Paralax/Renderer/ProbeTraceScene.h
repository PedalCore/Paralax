#pragma once

#include "Paralax/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Paralax {

	class Scene;
	class AssetManager;
	struct ProbeTraceSceneIdentity;

	namespace GPU {

		// Mirrors ProbeGI.slang. Preorder nodes use an escape index for bounded, stackless traversal.
		struct ProbeTraceNode
		{
			glm::vec4 Minimum{0.0f};
			glm::vec4 Maximum{0.0f};
			glm::uvec4 Range{0}; // first triangle, triangle count (zero for internal nodes), escape node, unused
		};
		struct ProbeTraceTriangle
		{
			glm::vec4 Position[3]{};
			glm::vec4 Normal[3]{};
			glm::vec4 UV01{0.0f};
			glm::vec4 UV2{0.0f};
			glm::uvec4 Material{0}; // x: material index; y: exclude from direct-light shadow rays
		};
		struct ProbeTraceMaterial
		{
			glm::vec4 BaseColor{1.0f};
			glm::vec4 Emissive{0.0f};   // w: double-sided
			glm::vec4 Parameters{0.0f}; // metallic, roughness, alpha cutoff, alpha mode
			glm::uvec4 Textures[3]{};   // base colour, emissive, metal/rough: offset, width, height, unused
		};
		static_assert(sizeof(ProbeTraceNode) == 48);
		static_assert(sizeof(ProbeTraceTriangle) == 144);
		static_assert(sizeof(ProbeTraceMaterial) == 96);

	}

	// Owned CPU snapshot for portable probe tracing. Main-thread-only; includes off-screen geometry. Asset
	// changes follow the engine Version contract. Update returns false on invalid or oversized input and never
	// exposes a partial snapshot. A first snapshot may synchronously load off-screen material textures. Blended
	// surfaces and nonstatic rigid-body hierarchies are excluded; alpha masks are sampled by the tracer.
	class ProbeTraceScene
	{
	public:
		static constexpr uint32_t MaxTriangles = 1024 * 1024;
		static constexpr uint32_t MaxTexels = 16 * 1024 * 1024;
		static constexpr float MaxCoordinate = 1000000.0f;
		static constexpr float MaxMaterialValue = 1000000.0f;

		bool Update(Scene& scene, AssetManager& assets, bool& changed);
		// Opaque shared lifetime token for the accepted scene; retaining it distinguishes address reuse.
		const Ref<const ProbeTraceSceneIdentity>& GetSceneIdentity() const { return m_SceneIdentity; }
		const std::vector<GPU::ProbeTraceNode>& GetNodes() const { return m_Nodes; }
		const std::vector<GPU::ProbeTraceTriangle>& GetTriangles() const { return m_Triangles; }
		const std::vector<GPU::ProbeTraceMaterial>& GetMaterials() const { return m_Materials; }
		const std::vector<glm::vec4>& GetTexels() const { return m_Texels; }

	private:
		Ref<const ProbeTraceSceneIdentity> m_SceneIdentity, m_RejectedSceneIdentity;
		std::vector<uint64_t> m_Snapshot;
		std::vector<uint64_t> m_RejectedSnapshot;
		std::vector<GPU::ProbeTraceNode> m_Nodes;
		std::vector<GPU::ProbeTraceTriangle> m_Triangles;
		std::vector<GPU::ProbeTraceMaterial> m_Materials;
		std::vector<glm::vec4> m_Texels;
	};

}
