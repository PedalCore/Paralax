#pragma once

#include "Paralax/Renderer/ProbeTraceSource.h"

namespace Paralax {

	// Owned CPU snapshot for portable probe tracing. Main-thread-only; includes off-screen geometry. Asset
	// changes follow the engine Version contract. Update returns false on invalid or oversized input and never
	// exposes a partial snapshot. A first snapshot may synchronously load off-screen material textures. Blended
	// surfaces and nonstatic rigid-body hierarchies are excluded; alpha masks are sampled by the tracer.
	class ProbeTraceScene
	{
	public:
		static constexpr uint32_t MaxTriangles = ProbeTraceSource::MaxTriangles;
		static constexpr uint32_t MaxTexels = ProbeTraceSource::MaxTexels;
		static constexpr float MaxCoordinate = ProbeTraceSource::MaxCoordinate;
		static constexpr float MaxMaterialValue = ProbeTraceSource::MaxMaterialValue;

		// Default construction uses Vulkan's guaranteed storage-buffer range. The supplied limits are copied.
		ProbeTraceScene();
		explicit ProbeTraceScene(const GraphicsDeviceLimits& limits);
		ProbeTraceScene(const ProbeTraceScene&) = delete;
		ProbeTraceScene& operator=(const ProbeTraceScene&) = delete;
		bool Update(Scene& scene, AssetManager& assets, bool& changed);
		// Opaque shared lifetime token for the accepted scene; retaining it distinguishes address reuse.
		const Ref<const ProbeTraceSceneIdentity>& GetSceneIdentity() const;
		const std::vector<GPU::ProbeTraceNode>& GetNodes() const;
		const std::vector<GPU::ProbeTraceTriangle>& GetTriangles() const;
		const std::vector<GPU::ProbeTraceMaterial>& GetMaterials() const;
		const std::vector<glm::vec4>& GetTexels() const;

	private:
		ProbeTraceSource m_Source;
	};

}
