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
		// The longest name or tag, in bytes. The registry enforces it, so scene files, the inspector, scripts and
		// agents all share the one limit.
		static constexpr size_t MaxLength = 256;

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

	// A grid of light probes that cache indirect (bounced) lighting for the renderer's global illumination. Probe
	// (0, 0, 0) sits at the entity's world position and the grid extends along the world axes, ProbeSpacing metres
	// between probes (the entity's rotation and scale are ignored). One volume lights a scene: the enabled one on the
	// entity with the lowest UUID; further enabled volumes are ignored, with a warning.
	struct GIVolumeComponent
	{
		bool Enabled = true;
		float ProbeSpacing = 1.0f; // Metres.
		int ProbeCountX = 4;       // 2 to 12 along each axis (at most 1728 probes).
		int ProbeCountY = 4;
		int ProbeCountZ = 4;
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

	enum class RigidbodyType
	{
		Static = 0,    // Never moves (walls, ground). Entities with only a Collider are static too.
		Kinematic = 1, // Moved by its Transform (scripts, animation); pushes dynamic bodies, unaffected by forces.
		Dynamic = 2    // Simulated: gravity, forces and collisions move it, and the simulation writes its Transform.
	};

	// Makes the entity a rigid body in the physics simulation (needs a ColliderComponent for its shape).
	struct RigidbodyComponent
	{
		RigidbodyType Type = RigidbodyType::Dynamic;
		float Mass = 1.0f; // kg; dynamic bodies only.
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityScale = 1.0f;
		// Keeps the body upright (rotation is not simulated), e.g. for characters.
		bool LockRotation = false;
		// Continuous collision detection, for small fast bodies that would otherwise pass through thin walls.
		bool ContinuousCollision = false;
	};

	enum class ColliderShape
	{
		Box = 0,
		Sphere = 1,
		Capsule = 2, // Upright (along Y).
		Cylinder = 3 // Upright (along Y).
	};

	// The physical shape of an entity. Size is the shape's bounding box in local units (before the Transform's scale,
	// which applies too): a box uses it directly; a sphere's diameter is its largest component; capsules and cylinders
	// take their diameter from X and Z and their total height from Y. The defaults match the built-in unit meshes
	// (the built-in capsule is 1 x 2 x 1).
	struct ColliderComponent
	{
		ColliderShape Shape = ColliderShape::Box;
		glm::vec3 Size{1.0f};
		glm::vec3 Offset{0.0f}; // Shape centre relative to the entity, in local units.
		float Friction = 0.5f;
		float Restitution = 0.0f; // Bounciness, 0..1.
		// Triggers detect overlaps (OnTriggerEnter/Exit) without colliding.
		bool IsTrigger = false;
	};

	// Plays an audio clip from the entity while the scene is running (see AudioSystem). A spatial source is heard from
	// its world position, fading with distance from the listener; a non-spatial one (music, UI) plays as is.
	struct AudioSourceComponent
	{
		AssetHandle Clip = 0;
		float Volume = 1.0f; // Linear gain.
		float Pitch = 1.0f;  // Playback speed (2 = an octave up, twice as fast).
		bool Loop = false;
		bool PlayOnStart = true; // Start when the scene starts running, or when the source is created while running.
		bool Spatial = true;
		float MinDistance = 1.0f;  // Full volume up to this distance from the listener.
		float MaxDistance = 50.0f; // No further fading beyond this distance.
	};

	// Where the scene is heard from (position and facing). Without one, the primary camera listens.
	struct AudioListenerComponent
	{
		bool Active = true;
	};

	enum class TextAlignment : int
	{
		Left = 0,
		Center = 1,
		Right = 2
	};

	// Text drawn by the renderer after tone mapping (so exposure never dims it), in the engine's font. Screen space:
	// the entity's X and Y are the anchor in normalised viewport coordinates (0, 0 top-left to 1, 1 bottom-right) and
	// Size is the line height in pixels at a 1080-pixel-high viewport, scaled with the viewport, so a HUD keeps its
	// place and proportion at any resolution. World space: the text lies in the entity's local XY plane facing +Z
	// (readable from the front, mirrored from behind), Size is the line height in metres, and it is depth-tested
	// against the scene. Text is UTF-8 (ASCII and Latin-1 have glyphs; other characters show as boxes), with
	// newlines, and is aligned about the anchor.
	struct TextComponent
	{
		std::string Text = "Text";
		float Size = 48.0f;
		glm::vec4 Color{1.0f}; // Linear RGBA.
		TextAlignment Alignment = TextAlignment::Left;
		bool ScreenSpace = true;
	};

	// Plays an animation clip on the entity and the entities below it while the scene is running (see AnimationSystem).
	struct AnimationComponent
	{
		AssetHandle Clip = 0;
		bool Playing = true; // Cleared when a clip that does not loop reaches its end.
		bool Loop = true;
		float Speed = 1.0f; // Negative plays backwards.
		float Time = 0.0f;  // Seconds into the clip; set it to start part-way or to seek.
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
