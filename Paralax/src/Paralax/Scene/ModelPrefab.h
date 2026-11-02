#pragma once

#include "Paralax/Asset/MeshImporter.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Scene/Prefab.h"

#include <string>
#include <vector>

namespace Paralax {

	// Builds a prefab that mirrors an imported model's scene (MeshImportResult::Nodes), so a file exported from a tool
	// such as Blender arrives as editable entities rather than one combined mesh. The prefab's root entity is named
	// rootName; under it is one entity per node, nested as the nodes are, each with the node's name and local
	// transform and:
	// - a Mesh component showing the node's mesh (a sub-asset of the model, so it follows re-imports), casting
	//   shadows unless the node says otherwise (ImportedNode::CastShadows);
	// - a Camera component for a node with a camera (not primary, so it never takes over a scene by itself);
	// - a DirectionalLight, PointLight or SpotLight component for a node with a light;
	// - an Animation component, playing and looping, on the entity each animation is meant to be played by (see
	//   ImportedAnimation; an entity holds one clip, so a further clip for the same entity is left out, with a warning
	//   in outWarnings if given, and stays available as an asset).
	// Returns nullptr (with outError set) if the model has no nodes. Main-thread-only.
	Ref<PrefabAsset> BuildModelPrefab(const MeshImportResult& model, const std::string& rootName, std::string& outError, std::vector<std::string>* outWarnings = nullptr);

}
