#pragma once

#include "Paralax/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Paralax {

	struct LocalShadowCandidate
	{
		uint64_t ID = 0;
		glm::vec3 Position{0.0f};
		float Range = 0.0f;
		uint32_t Layers = 6; // Six for points/wide spots, one for ordinary spots.
		bool CastShadows = false;
		bool Visible = true;
		uint32_t Kind = 0; // Point = 0, spot = 1; an entity may have both components.
	};

	struct LocalShadowLightKey
	{
		uint64_t ID = 0;
		uint32_t Kind = 0;
		bool operator==(const LocalShadowLightKey&) const = default;
	};

	// Caller-owned, main-thread-only retention for one view. Captures omit it for history-free selection.
	// The opaque scene token prevents replacement scenes at the same address from inheriting stale selections.
	struct LocalShadowHistory
	{
		Ref<const void> SceneIdentity;
		std::vector<LocalShadowLightKey> Lights;
	};

	struct LocalShadowAssignment
	{
		uint32_t Candidate = 0;
		uint32_t FirstLayer = 0;
		uint32_t Layers = 0;
	};

	struct LocalShadowSelection
	{
		std::vector<uint32_t> DirectLights;
		std::vector<LocalShadowAssignment> Shadows;
		uint32_t LayersUsed = 0;
		uint32_t Requested = 0;
	};

	// Pure, thread-safe budget calculation. Invalid settings produce zero capacity; no allocation is performed.
	uint32_t LocalShadowLayerCapacity(uint32_t resolution, uint32_t requestedLights, uint32_t adapterLayers);

	// Deterministic selection before direct-light truncation. Invalid candidates are excluded without asserting.
	// Result indices refer to candidates. Calls sharing history must be serialized; without history this is pure.
	LocalShadowSelection SelectLocalShadows(std::span<const LocalShadowCandidate> candidates, glm::vec3 camera,
											uint32_t maxLights, uint32_t layerCapacity, const Ref<const void>& sceneIdentity = {}, LocalShadowHistory* history = nullptr);

}
