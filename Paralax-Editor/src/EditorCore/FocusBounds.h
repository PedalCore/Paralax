#pragma once

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Math/AABB.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>

namespace Paralax::Editor {

	// World-space bounds that frame an entity in the editor camera: its mesh and every mesh below it, so a group
	// without a mesh of its own (such as an imported model's prefab root) frames what it holds. An entity with no mesh
	// at or below it gets a 1 m box at its position. Meshes that fail to load are skipped. Main-thread-only.
	AABB ComputeFocusBounds(Scene& scene, AssetManager& assets, Entity entity);

}
