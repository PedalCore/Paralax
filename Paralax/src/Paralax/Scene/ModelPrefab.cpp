#include "pxpch.h"
#include "Paralax/Scene/ModelPrefab.h"

#include "Paralax/Scene/ComponentRegistry.h"
#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Scene.h"

#include <vector>

namespace Paralax {

	Ref<PrefabAsset> BuildModelPrefab(const MeshImportResult& model, const std::string& rootName, std::string& outError, std::vector<std::string>* outWarnings)
	{
		outError.clear();
		if (model.Nodes.empty())
		{
			outError = "the model has no nodes";
			return nullptr;
		}

		Scene scene("ModelPrefab");
		Entity root = scene.CreateEntity("Model");
		// Through the registry, so the prefab never holds a name its own file would refuse when loaded.
		if (!rootName.empty() && !ComponentRegistry::SetField(*ComponentRegistry::Find("Tag")->FindField("Name"), &root.GetComponent<TagComponent>(), rootName, outError))
			return nullptr;
		std::vector<Entity> entities;
		entities.reserve(model.Nodes.size());
		for (const ImportedNode& node : model.Nodes)
		{
			Entity entity = scene.CreateEntity(node.Name);
			// Parents precede children (MeshImporter records nodes in pre-order); anything else is attached to the root.
			const bool hasParent = node.Parent.has_value() && *node.Parent < entities.size();
			scene.SetParent(entity, hasParent ? entities[*node.Parent] : root, false);

			auto& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = node.Translation;
			transform.Rotation = node.Rotation;
			transform.Scale = node.Scale;

			if (node.Mesh.IsValid())
			{
				auto& mesh = entity.AddComponent<MeshComponent>();
				mesh.Mesh = node.Mesh;
				// Unspecified: the component's default (on).
				if (node.CastShadows)
					mesh.CastShadows = *node.CastShadows;
			}

			if (node.Camera)
			{
				auto& camera = entity.AddComponent<CameraComponent>();
				camera.Primary = false;
				SceneCamera& projection = camera.Camera;
				if (node.Camera->Orthographic)
				{
					projection.Projection = SceneCamera::ProjectionType::Orthographic;
					projection.OrthographicSize = node.Camera->OrthographicSize;
					projection.OrthographicNear = node.Camera->Near;
					projection.OrthographicFar = node.Camera->Far;
				}
				else
				{
					projection.Projection = SceneCamera::ProjectionType::Perspective;
					projection.PerspectiveFOV = node.Camera->FieldOfView;
					projection.PerspectiveNear = node.Camera->Near;
					projection.PerspectiveFar = node.Camera->Far;
				}
			}

			if (node.Light)
			{
				const ImportedLight& light = *node.Light;
				switch (light.Type)
				{
					case ImportedLightType::Directional:
					{
						auto& component = entity.AddComponent<DirectionalLightComponent>();
						component.Color = light.Color;
						component.Intensity = light.Intensity;
						break;
					}
					case ImportedLightType::Point:
					{
						auto& component = entity.AddComponent<PointLightComponent>();
						component.Color = light.Color;
						component.Intensity = light.Intensity;
						component.Range = light.Range;
						break;
					}
					case ImportedLightType::Spot:
					{
						auto& component = entity.AddComponent<SpotLightComponent>();
						component.Color = light.Color;
						component.Intensity = light.Intensity;
						component.Range = light.Range;
						component.InnerConeAngle = light.InnerConeAngle;
						component.OuterConeAngle = light.OuterConeAngle;
						break;
					}
				}
			}
			entities.push_back(entity);
		}

		// Each animation on the entity it is meant to be played by, playing and looping. An entity can hold one clip:
		// further clips for it stay available as assets.
		for (const ImportedAnimation& animation : model.Animations)
		{
			Entity host = animation.Host && *animation.Host < entities.size() ? entities[*animation.Host] : root;
			if (host.HasComponent<AnimationComponent>())
			{
				if (outWarnings)
					outWarnings->push_back(fmt::format("animation '{}' is not attached: '{}' already plays another; set it on an Animation component to play it", animation.Clip->Name, host.GetName()));
				continue;
			}
			host.AddComponent<AnimationComponent>().Clip = animation.Clip->Handle;
		}
		return PrefabAsset::CreateFromEntity(scene, root);
	}

}
