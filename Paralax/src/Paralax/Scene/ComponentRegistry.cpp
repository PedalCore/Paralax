#include "pxpch.h"
#include "Paralax/Scene/ComponentRegistry.h"

#include "Paralax/Scene/Components.h"

#include <spdlog/fmt/ranges.h>

#include <charconv>
#include <cmath>
#include <type_traits>

namespace Paralax {

	namespace {

		using json = nlohmann::json;
		using Reflection::FieldOptions;
		using Reflection::MakeField;
		using Reflection::MakeNestedField;

		template<typename T>
		ComponentInfo MakeComponent(std::string name, std::string description, std::vector<FieldInfo> fields, bool userEditable = true)
		{
			ComponentInfo info;
			info.Name = std::move(name);
			info.Description = std::move(description);
			info.Fields = std::move(fields);
			info.UserEditable = userEditable;
			info.Has = [](const entt::registry& registry, entt::entity entity) { return registry.all_of<T>(entity); };
			info.Get = [](entt::registry& registry, entt::entity entity) -> void* { return registry.try_get<T>(entity); };
			info.Add = [](entt::registry& registry, entt::entity entity) -> void* { return &registry.get_or_emplace<T>(entity); };
			if (userEditable)
				info.Remove = [](entt::registry& registry, entt::entity entity) { registry.remove<T>(entity); };
			info.Copy = [](const entt::registry& source, entt::entity from, entt::registry& destination, entt::entity to) {
				destination.emplace_or_replace<T>(to, source.get<T>(from));
			};
			return info;
		}

		std::vector<ComponentInfo> BuildRegistry()
		{
			const FieldOptions color = FieldOptions().Color();
			const FieldOptions intensity = FieldOptions().Range(0.0f, 100000.0f);
			const FieldOptions range = FieldOptions().Range(0.0f, 100000.0f);

			std::vector<ComponentInfo> components;

			components.push_back(MakeComponent<TagComponent>("Tag", "Display name and free-form tag.",
															 {
																 MakeField<&TagComponent::Name>("Name", FieldOptions().MaxLength(TagComponent::MaxLength)),
																 MakeField<&TagComponent::Tag>("Tag", FieldOptions().MaxLength(TagComponent::MaxLength).Describe("Label for grouping and lookup, e.g. \"Enemy\".")),
															 },
															 false));

			components.push_back(MakeComponent<TransformComponent>("Transform", "Position, rotation and scale relative to the parent.",
																   {
																	   MakeField<&TransformComponent::Translation>("Translation"),
																	   MakeField<&TransformComponent::Rotation>("Rotation", FieldOptions().Describe("Unit quaternion [x, y, z, w].")),
																	   MakeField<&TransformComponent::Scale>("Scale"),
																   },
																   false));

			components.push_back(MakeComponent<CameraComponent>("Camera", "Renders the scene from the entity's transform.",
																{
																	MakeNestedField<&CameraComponent::Camera, &SceneCamera::Projection>("Projection", FieldOptions().Enum({"Perspective", "Orthographic"})),
																	MakeNestedField<&CameraComponent::Camera, &SceneCamera::PerspectiveFOV>("FieldOfView",
																																			FieldOptions().Angle().Range(glm::radians(1.0f), glm::radians(179.0f)).Describe("Vertical field of view.")),
																	MakeNestedField<&CameraComponent::Camera, &SceneCamera::PerspectiveNear>("PerspectiveNear", FieldOptions().Range(0.0001f, 100000.0f)),
																	MakeNestedField<&CameraComponent::Camera, &SceneCamera::PerspectiveFar>("PerspectiveFar", FieldOptions().Range(0.001f, 1000000.0f)),
																	MakeNestedField<&CameraComponent::Camera, &SceneCamera::OrthographicSize>("OrthographicSize", FieldOptions().Range(0.0001f, 1000000.0f)),
																	MakeNestedField<&CameraComponent::Camera, &SceneCamera::OrthographicNear>("OrthographicNear"),
																	MakeNestedField<&CameraComponent::Camera, &SceneCamera::OrthographicFar>("OrthographicFar"),
																	MakeField<&CameraComponent::Primary>("Primary"),
																}));

			components.push_back(MakeComponent<DirectionalLightComponent>("DirectionalLight", "Sun-like light shining along the entity's -Z axis.",
																		  {
																			  MakeField<&DirectionalLightComponent::Color>("Color", color),
																			  MakeField<&DirectionalLightComponent::Intensity>("Intensity", intensity),
																			  MakeField<&DirectionalLightComponent::CastShadows>("CastShadows"),
																		  }));

			components.push_back(MakeComponent<PointLightComponent>("PointLight", "Omnidirectional light.",
																	{
																		MakeField<&PointLightComponent::Color>("Color", color),
																		MakeField<&PointLightComponent::Intensity>("Intensity", intensity),
																		MakeField<&PointLightComponent::Range>("Range", range),
																		MakeField<&PointLightComponent::CastShadows>("CastShadows"),
																	}));

			components.push_back(MakeComponent<SpotLightComponent>("SpotLight", "Cone light shining along the entity's -Z axis.",
																   {
																	   MakeField<&SpotLightComponent::Color>("Color", color),
																	   MakeField<&SpotLightComponent::Intensity>("Intensity", intensity),
																	   MakeField<&SpotLightComponent::Range>("Range", range),
																	   MakeField<&SpotLightComponent::InnerConeAngle>("InnerConeAngle", FieldOptions().Angle().Range(0.0f, glm::radians(89.0f))),
																	   MakeField<&SpotLightComponent::OuterConeAngle>("OuterConeAngle", FieldOptions().Angle().Range(0.0f, glm::radians(89.0f))),
																	   MakeField<&SpotLightComponent::CastShadows>("CastShadows"),
																   }));

			components.push_back(MakeComponent<GIVolumeComponent>("GIVolume", "A world-aligned grid of light probes caching indirect lighting (global illumination); one per scene.",
																  {
																	  MakeField<&GIVolumeComponent::Enabled>("Enabled"),
																	  MakeField<&GIVolumeComponent::ProbeSpacing>("ProbeSpacing", FieldOptions().Range(0.1f, 100.0f).Describe("Metres between probes.")),
																	  MakeField<&GIVolumeComponent::ProbeCountX>("ProbeCountX", FieldOptions().Range(2.0f, 12.0f).Describe("Probes along world X, from the entity's position.")),
																	  MakeField<&GIVolumeComponent::ProbeCountY>("ProbeCountY", FieldOptions().Range(2.0f, 12.0f).Describe("Probes along world Y.")),
																	  MakeField<&GIVolumeComponent::ProbeCountZ>("ProbeCountZ", FieldOptions().Range(2.0f, 12.0f).Describe("Probes along world Z.")),
																  }));

			components.push_back(MakeComponent<SkyLightComponent>("SkyLight", "Image-based lighting and background from an HDR environment map.",
																  {
																	  MakeField<&SkyLightComponent::Environment>("Environment", FieldOptions().Describe("Environment (HDR) texture asset.")),
																	  MakeField<&SkyLightComponent::Intensity>("Intensity", intensity),
																	  MakeField<&SkyLightComponent::RotationDegrees>("Rotation", FieldOptions().Range(-360.0f, 360.0f).Describe("Rotation about the up axis, degrees.")),
																	  MakeField<&SkyLightComponent::ShowBackground>("ShowBackground"),
																  }));

			components.push_back(MakeComponent<MeshComponent>("Mesh", "Renders a mesh asset with optional material overrides.",
															  {
																  MakeField<&MeshComponent::Mesh>("Mesh"),
																  MakeField<&MeshComponent::Materials>("Materials", FieldOptions().Describe("Per-submesh material overrides.")),
																  MakeField<&MeshComponent::CastShadows>("CastShadows"),
															  }));

			components.push_back(MakeComponent<RigidbodyComponent>("Rigidbody", "Simulates the entity as a rigid body (needs a Collider).",
																   {
																	   MakeField<&RigidbodyComponent::Type>("Type", FieldOptions().Enum({"Static", "Kinematic", "Dynamic"})),
																	   MakeField<&RigidbodyComponent::Mass>("Mass", FieldOptions().Range(0.001f, 1000000.0f).Describe("Mass in kg (dynamic bodies).")),
																	   MakeField<&RigidbodyComponent::LinearDamping>("LinearDamping", FieldOptions().Range(0.0f, 100.0f)),
																	   MakeField<&RigidbodyComponent::AngularDamping>("AngularDamping", FieldOptions().Range(0.0f, 100.0f)),
																	   MakeField<&RigidbodyComponent::GravityScale>("GravityScale", FieldOptions().Range(-100.0f, 100.0f)),
																	   MakeField<&RigidbodyComponent::LockRotation>("LockRotation", FieldOptions().Describe("Keep the body upright (e.g. characters).")),
																	   MakeField<&RigidbodyComponent::ContinuousCollision>("ContinuousCollision", FieldOptions().Describe("For small, fast bodies.")),
																   }));

			components.push_back(MakeComponent<ColliderComponent>("Collider", "The entity's physical shape (static unless it has a Rigidbody).",
																  {
																	  MakeField<&ColliderComponent::Shape>("Shape", FieldOptions().Enum({"Box", "Sphere", "Capsule", "Cylinder"})),
																	  MakeField<&ColliderComponent::Size>("Size", FieldOptions().Range(0.001f, 100000.0f).Describe("Bounding box in local units (scaled by the Transform).")),
																	  MakeField<&ColliderComponent::Offset>("Offset", FieldOptions().Describe("Shape centre relative to the entity.")),
																	  MakeField<&ColliderComponent::Friction>("Friction", FieldOptions().Range(0.0f, 10.0f)),
																	  MakeField<&ColliderComponent::Restitution>("Restitution", FieldOptions().Range(0.0f, 1.0f).Describe("Bounciness.")),
																	  MakeField<&ColliderComponent::IsTrigger>("IsTrigger", FieldOptions().Describe("Detect overlaps without colliding.")),
																  }));

			components.push_back(MakeComponent<AudioSourceComponent>("AudioSource", "Plays an audio clip from the entity while the scene is running.",
																	 {
																		 MakeField<&AudioSourceComponent::Clip>("Clip", FieldOptions().Describe("Audio asset (.wav, .flac or .mp3).")),
																		 MakeField<&AudioSourceComponent::Volume>("Volume", FieldOptions().Range(0.0f, 10.0f).Describe("Linear gain.")),
																		 MakeField<&AudioSourceComponent::Pitch>("Pitch", FieldOptions().Range(0.1f, 4.0f).Describe("Playback speed; 2 is an octave up.")),
																		 MakeField<&AudioSourceComponent::Loop>("Loop"),
																		 MakeField<&AudioSourceComponent::PlayOnStart>("PlayOnStart", FieldOptions().Describe("Start playing when the scene starts (or when created while running).")),
																		 MakeField<&AudioSourceComponent::Spatial>("Spatial", FieldOptions().Describe("Heard from the entity's position, fading with distance.")),
																		 MakeField<&AudioSourceComponent::MinDistance>("MinDistance", FieldOptions().Range(0.01f, 100000.0f).Describe("Full volume up to this distance.")),
																		 MakeField<&AudioSourceComponent::MaxDistance>("MaxDistance", FieldOptions().Range(0.01f, 100000.0f).Describe("No further fading beyond this distance.")),
																	 }));

			components.push_back(MakeComponent<AudioListenerComponent>("AudioListener", "Hears the scene from this entity (otherwise from the primary camera).",
																	   {
																		   MakeField<&AudioListenerComponent::Active>("Active"),
																	   }));

			components.push_back(MakeComponent<TextComponent>("Text", "Text drawn on screen (a HUD) or in the world, unaffected by exposure.",
															  {
																  MakeField<&TextComponent::Text>("Text", FieldOptions().MaxLength(4096).Describe("UTF-8; newlines start new lines.")),
																  MakeField<&TextComponent::Size>("Size", FieldOptions().Range(0.0f, 10000.0f).Describe("Line height: pixels at a 1080-pixel-high viewport (screen space) or metres (world space).")),
																  MakeField<&TextComponent::Color>("Color", color),
																  MakeField<&TextComponent::Alignment>("Alignment", FieldOptions().Enum({"Left", "Center", "Right"}).Describe("Which side of the text the entity's position anchors.")),
																  MakeField<&TextComponent::ScreenSpace>("ScreenSpace", FieldOptions().Describe("On screen at normalised X/Y (0..1 from the top-left), or in the world at the Transform.")),
															  }));

			components.push_back(MakeComponent<AnimationComponent>("Animation", "Plays an animation clip on the entity and the entities below it while the scene is running.",
																   {
																	   MakeField<&AnimationComponent::Clip>("Clip", FieldOptions().Describe("Animation clip asset (from a glTF model).")),
																	   MakeField<&AnimationComponent::Playing>("Playing", FieldOptions().Describe("Cleared when a clip that does not loop ends.")),
																	   MakeField<&AnimationComponent::Loop>("Loop"),
																	   MakeField<&AnimationComponent::Speed>("Speed", FieldOptions().Range(-100.0f, 100.0f).Describe("Playback rate; negative plays backwards.")),
																	   MakeField<&AnimationComponent::Time>("Time", FieldOptions().Range(0.0f, 1000000.0f).Describe("Seconds into the clip; set it to seek.")),
																   }));

			components.push_back(MakeComponent<ScriptComponent>("Script", "Runs a Lua script (OnCreate, OnUpdate, OnDestroy) while the scene is running.",
																{
																	MakeField<&ScriptComponent::Script>("Script", FieldOptions().Describe("Script asset (.lua) returning a table with the callbacks.")),
																}));

			components.push_back(MakeComponent<PrefabInstanceComponent>("PrefabInstance", "Marks an entity instantiated from a prefab.",
																		{
																			MakeField<&PrefabInstanceComponent::Prefab>("Prefab"),
																		},
																		false));

			return components;
		}

		const std::vector<ComponentInfo>& GetRegistry()
		{
			static const std::vector<ComponentInfo> s_Registry = BuildRegistry();
			return s_Registry;
		}

	}

	const FieldInfo* ComponentInfo::FindField(std::string_view name) const
	{
		return Paralax::FindField(Fields, name);
	}

	std::span<const ComponentInfo> ComponentRegistry::GetAll()
	{
		return GetRegistry();
	}

	const ComponentInfo* ComponentRegistry::Find(std::string_view name)
	{
		for (const ComponentInfo& info : GetRegistry())
		{
			if (info.Name == name)
				return &info;
		}
		return nullptr;
	}

	json ComponentRegistry::GetField(const FieldInfo& field, const void* component)
	{
		return Reflection::GetField(field, component);
	}

	bool ComponentRegistry::SetField(const FieldInfo& field, void* component, const json& value, std::string& outError)
	{
		return Reflection::SetField(field, component, value, outError);
	}

	json ComponentRegistry::ToJson(const ComponentInfo& info, const void* component)
	{
		return Reflection::ToJson(info.Fields, component);
	}

	bool ComponentRegistry::FromJson(const ComponentInfo& info, void* component, const json& object, std::string& outError)
	{
		if (!object.is_object())
		{
			outError = fmt::format("Component '{}' expects a JSON object", info.Name);
			return false;
		}
		return Reflection::FromJson(info.Fields, info.Name, component, object, outError);
	}

}
