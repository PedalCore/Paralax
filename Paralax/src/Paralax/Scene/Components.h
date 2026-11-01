#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Core/UUID.h"
#include "Paralax/Math/Math.h"
#include "Paralax/Scene/SceneCamera.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

// Component types. Components are plain data; behaviour lives in systems (Scene, renderer, physics, scripting).
// Every component except IDComponent and RelationshipComponent is registered with ComponentRegistry
// (ComponentRegistry.cpp), which drives serialization, the editor inspector, scripting and the agent API.
// Adding a component: define it here, register it there, and extend the feature test scene.

namespace Paralax {

	// Persistent identity. Every entity has exactly one; never added or removed by users.
	struct IDComponent
	{
		UUID ID;
	};

	struct TagComponent
	{
		std::string Name = "Entity";
		// Free-form label for grouping and lookup from scripts (e.g. "Enemy").
		std::string Tag;
	};

	// Local transform relative to the parent (or the world for root entities).
	struct TransformComponent
	{
		glm::vec3 Translation{0.0f};
		glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 Scale{1.0f};

		glm::mat4 GetLocalMatrix() const { return Math::ComposeTransform(Translation, Rotation, Scale); }
	};

	// Scene hierarchy links. Maintained by Scene; use Scene::SetParent rather than editing directly.
	struct RelationshipComponent
	{
		UUID Parent = 0;
		std::vector<UUID> Children;
	};

	struct CameraComponent
	{
		SceneCamera Camera;
		// The primary camera is used by the runtime to render the scene.
		bool Primary = true;
	};

	struct DirectionalLightComponent
	{
		glm::vec3 Color{1.0f};
		float Intensity = 3.0f; // Illuminance-like scale.
		bool CastShadows = true;
	};

	struct PointLightComponent
	{
		glm::vec3 Color{1.0f};
		float Intensity = 10.0f;
		float Range = 10.0f;
		bool CastShadows = false;
	};

	struct SpotLightComponent
	{
		glm::vec3 Color{1.0f};
		float Intensity = 10.0f;
		float Range = 10.0f;
		float InnerConeAngle = glm::radians(20.0f);
		float OuterConeAngle = glm::radians(30.0f);
		bool CastShadows = false;
	};

	// Image-based lighting and background from an HDR environment.
	struct SkyLightComponent
	{
		AssetHandle Environment = 0;
		float Intensity = 1.0f;
		float RotationDegrees = 0.0f;
		bool ShowBackground = true;
	};

	struct MeshComponent
	{
		AssetHandle Mesh = 0;
		// Per-submesh material overrides; 0 (or missing entries) uses the mesh's own material.
		std::vector<AssetHandle> Materials;
		bool CastShadows = true;
	};

	// Runs a Lua script on the entity while the scene is running (see ScriptSystem).
	struct ScriptComponent
	{
		AssetHandle Script = 0;
	};

	// Marks the root entity of a prefab instance (set by PrefabAsset::Instantiate).
	struct PrefabInstanceComponent
	{
		AssetHandle Prefab = 0;
	};

}
