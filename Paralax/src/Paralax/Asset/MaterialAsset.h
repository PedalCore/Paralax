#pragma once

#include "Paralax/Asset/Asset.h"

#include <glm/glm.hpp>

namespace Paralax {

	enum class AlphaMode
	{
		Opaque = 0,
		Mask = 1, // alpha-tested against AlphaCutoff
		Blend = 2 // alpha-blended (forward pass)
	};

	// PBR metallic-roughness material (glTF 2.0 semantics), plus parallax occlusion mapping. Texture handles of 0
	// mean "none" (the factor alone is used). Fields are reflected (see MaterialAsset reflection in
	// ComponentRegistry) so materials can be edited by the inspector, scripts and agents.
	class MaterialAsset : public Asset
	{
	public:
		AssetType GetType() const override { return AssetType::Material; }

		glm::vec4 BaseColorFactor{1.0f};
		AssetHandle BaseColorTexture = 0;

		float MetallicFactor = 0.0f;
		float RoughnessFactor = 0.5f;
		// glTF layout: roughness in G, metallic in B.
		AssetHandle MetallicRoughnessTexture = 0;

		AssetHandle NormalTexture = 0;
		float NormalScale = 1.0f;

		AssetHandle OcclusionTexture = 0;
		float OcclusionStrength = 1.0f;

		glm::vec3 EmissiveFactor{0.0f};
		float EmissiveStrength = 1.0f;
		AssetHandle EmissiveTexture = 0;

		// Parallax occlusion mapping: height in R (1 = top surface). Disabled when HeightTexture is 0.
		AssetHandle HeightTexture = 0;
		float HeightScale = 0.05f;

		AlphaMode Alpha = AlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
	};

}
