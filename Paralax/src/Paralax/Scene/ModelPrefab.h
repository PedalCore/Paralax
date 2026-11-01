#pragma once

#include "Paralax/Asset/MeshImporter.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Scene/Prefab.h"

#include <string>

namespace Paralax {

	// Builds a prefab that mirrors an imported model's scene (MeshImportResult::Nodes), so a file exported from a tool
	// such as Blender arrives as editable entities rather than one combined mesh. The prefab's root entity is named
	// rootName; under it is one entity per node, nested as the nodes are, each with the node's name and local
	// transform and:
	// - a Mesh component showing the node's mesh (a sub-asset of the model, so it follows re-imports);
	// - a Camera component for a node with a camera (not primary, so it never takes over a scene by itself);
	// - a DirectionalLight, PointLight or SpotLight component for a node with a light.
	// Returns nullptr (with outError set) if the model has no nodes. Main-thread-only.
	Ref<PrefabAsset> BuildModelPrefab(const MeshImportResult& model, const std::string& rootName, std::string& outError);

}
