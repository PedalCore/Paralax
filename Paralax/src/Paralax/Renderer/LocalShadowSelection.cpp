#include "pxpch.h"
#include "Paralax/Renderer/LocalShadowSelection.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace Paralax {

	uint32_t LocalShadowLayerCapacity(uint32_t resolution, uint32_t requestedLights, uint32_t adapterLayers)
	{
		if ((resolution != 512 && resolution != 1024 && resolution != 2048) || requestedLights > 16)
			return 0;
		constexpr uint64_t BudgetBytes = 256ull * 1024 * 1024;
		const uint64_t layerBytes = uint64_t(resolution) * resolution * 4;
		return std::min({requestedLights * 6, static_cast<uint32_t>(BudgetBytes / layerBytes), adapterLayers});
	}

	LocalShadowSelection SelectLocalShadows(std::span<const LocalShadowCandidate> candidates, glm::vec3 camera,
											uint32_t maxLights, uint32_t layerCapacity, const Ref<const void>& sceneIdentity, LocalShadowHistory* history)
	{
		LocalShadowSelection result;
		if (history && history->SceneIdentity != sceneIdentity)
		{
			history->SceneIdentity = sceneIdentity;
			history->Lights.clear();
		}
		auto finite = [](glm::vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
		if (!finite(camera) || maxLights > 16)
		{
			if (history)
				history->Lights.clear();
			return result;
		}
		struct Ranked
		{
			uint32_t Index;
			double RangeDistance, Distance;
			uint64_t ID;
			uint32_t Kind;
		};
		std::vector<Ranked> ranked;
		ranked.reserve(candidates.size());
		for (size_t index = 0; index < candidates.size(); ++index)
		{
			const auto& candidate = candidates[index];
			if (!finite(candidate.Position) || !std::isfinite(candidate.Range) || candidate.Kind > 1 ||
				candidate.Range <= 0.0f || candidate.Range > 100000.0f || candidate.ID == 0 ||
				(candidate.Layers != 1 && candidate.Layers != 6) || index > UINT32_MAX)
				continue;
			const double distance = glm::length(glm::dvec3(candidate.Position) - glm::dvec3(camera));
			ranked.push_back({static_cast<uint32_t>(index), std::max(0.0, distance - static_cast<double>(candidate.Range)), distance, candidate.ID, candidate.Kind});
		}
		auto key = [](const Ranked& item) { return std::tuple(item.RangeDistance, item.Distance, item.ID, item.Kind); };
		std::sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& b) { return key(a) < key(b); });
		// The direct-light limit cannot depend on component pool iteration order or on a previous viewport capture.
		if (ranked.size() > 256)
			ranked.resize(256);
		for (const auto& item : ranked)
			result.DirectLights.push_back(item.Index);
		// The same direct-light array feeds GI outside the camera frustum; only shadow allocation is view-culled.
		std::erase_if(ranked, [&](const Ranked& item) { return !candidates[item.Index].CastShadows || !candidates[item.Index].Visible || candidates[item.Index].Range <= 0.05f; });
		result.Requested = static_cast<uint32_t>(ranked.size());
		if (history)
		{
			auto retainedKey = [&](const Ranked& item) {
				const bool retained = std::find(history->Lights.begin(), history->Lights.end(), LocalShadowLightKey{item.ID, item.Kind}) != history->Lights.end();
				const double factor = retained ? 0.8 : 1.0;
				return std::tuple(item.RangeDistance * factor, item.Distance * factor, item.ID, item.Kind);
			};
			std::sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& b) { return retainedKey(a) < retainedKey(b); });
			history->Lights.clear();
		}
		for (const auto& item : ranked)
		{
			const auto& candidate = candidates[item.Index];
			if (result.Shadows.size() >= maxLights)
				break;
			if (candidate.Layers > layerCapacity - result.LayersUsed)
				continue;
			result.Shadows.push_back({item.Index, result.LayersUsed, candidate.Layers});
			result.LayersUsed += candidate.Layers;
			if (history)
				history->Lights.push_back({item.ID, item.Kind});
		}
		return result;
	}

}
