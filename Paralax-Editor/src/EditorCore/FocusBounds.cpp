#include "EditorCore/FocusBounds.h"

#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Scene/Components.h>

#include <vector>

namespace Paralax::Editor {

	AABB ComputeFocusBounds(Scene& scene, AssetManager& assets, Entity entity)
	{
		AABB bounds;
		// Iterative walk: hierarchies can be very deep.
		std::vector<Entity> pending{entity};
		while (!pending.empty())
		{
			const Entity current = pending.back();
			pending.pop_back();
			if (const auto* mesh = current.TryGetComponent<MeshComponent>())
			{
				if (Ref<MeshAsset> asset = assets.GetAsset<MeshAsset>(mesh->Mesh); asset && asset->Bounds.IsValid())
					bounds.Expand(asset->Bounds.Transformed(scene.GetWorldTransform(current)));
			}
			for (UUID child : current.GetComponent<RelationshipComponent>().Children)
			{
				if (std::optional<Entity> found = scene.TryGetEntityByUUID(child))
					pending.push_back(*found);
			}
		}
		if (!bounds.IsValid())
		{
			const glm::vec3 position(scene.GetWorldTransform(entity)[3]);
			bounds.Expand(position - glm::vec3(0.5f));
			bounds.Expand(position + glm::vec3(0.5f));
		}
		return bounds;
	}

}
