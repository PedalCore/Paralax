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
																 MakeField<&TagComponent::Name>("Name"),
																 MakeField<&TagComponent::Tag>("Tag", FieldOptions().Describe("Label for grouping and lookup, e.g. \"Enemy\".")),
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
