#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/ModelPrefab.h>
#include <Paralax/Scene/Scene.h>

#include <string>
#include <vector>

using namespace Paralax;

namespace {

	MeshImportResult ImportSceneModel(const std::string& name)
	{
		const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory(name);
		nlohmann::json document = Paralax::Testing::MakeTriangleDocument(directory);
		Paralax::Testing::AddCamerasAndLights(document);
		MeshImportResult result;
		std::string error;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
		return result;
	}

	std::vector<Entity> Children(Scene& scene, Entity parent)
	{
		std::vector<Entity> children;
		for (UUID id : parent.GetComponent<RelationshipComponent>().Children)
			children.push_back(scene.GetEntityByUUID(id));
		return children;
	}

	Entity Child(Scene& scene, Entity parent, const std::string& name)
	{
		for (Entity child : Children(scene, parent))
		{
			if (child.GetName() == name)
				return child;
		}
		FAIL("no child named ", name);
		return {};
	}

}

TEST_CASE("ModelPrefab: the model's nodes become nested entities with meshes, cameras and lights")
{
	const MeshImportResult model = ImportSceneModel("ModelPrefabNodes");
	std::string error;
	Ref<PrefabAsset> prefab = BuildModelPrefab(model, "Bay", error);
	REQUIRE_MESSAGE(prefab, error);
	CHECK(prefab->GetRootName() == "Bay");
	CHECK(prefab->GetEntityCount() == 8); // The root and seven nodes.

	Scene scene;
	Entity root = prefab->Instantiate(scene);
	std::vector<std::string> roots;
	for (Entity child : Children(scene, root))
		roots.push_back(child.GetName());
	CHECK(roots == std::vector<std::string>{"Node0", "Camera", "Sun"});

	// Meshes: both instances of the shared mesh show the per-node mesh, at their local transforms.
	Entity node0 = Child(scene, root, "Node0");
	Entity node1 = Child(scene, node0, "Node1");
	CHECK(node0.GetComponent<MeshComponent>().Mesh == model.NodeMeshes[0]->Handle);
	CHECK(node1.GetComponent<MeshComponent>().Mesh == model.NodeMeshes[0]->Handle);
	CHECK(node1.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 5.0f, 0.0f));
	CHECK(glm::vec3(scene.GetWorldTransform(node1)[3]) == glm::vec3(10.0f, 5.0f, 0.0f));

	// Cameras, never primary.
	Entity camera = Child(scene, root, "Camera");
	Entity topView = Child(scene, camera, "TopView");
	CHECK_FALSE(camera.HasComponent<MeshComponent>());
	CHECK(camera.GetComponent<CameraComponent>().Camera.Projection == SceneCamera::ProjectionType::Perspective);
	CHECK(camera.GetComponent<CameraComponent>().Camera.PerspectiveFOV == doctest::Approx(0.8));
	CHECK_FALSE(camera.GetComponent<CameraComponent>().Primary);
	CHECK(topView.GetComponent<CameraComponent>().Camera.Projection == SceneCamera::ProjectionType::Orthographic);
	CHECK(topView.GetComponent<CameraComponent>().Camera.OrthographicSize == doctest::Approx(6.0));
	CHECK_FALSE(topView.GetComponent<CameraComponent>().Primary);

	// Lights.
	Entity sun = Child(scene, root, "Sun");
	CHECK(sun.GetComponent<DirectionalLightComponent>().Intensity == doctest::Approx(3.0));
	Entity lamp = Child(scene, sun, "Lamp");
	CHECK(lamp.GetComponent<PointLightComponent>().Intensity == doctest::Approx(80.0));
	CHECK(lamp.GetComponent<PointLightComponent>().Range == doctest::Approx(static_cast<double>(model.Nodes[5].Light->Range)));
	Entity spot = Child(scene, sun, "Spot");
	const auto& spotLight = spot.GetComponent<SpotLightComponent>();
	CHECK(spotLight.Range == doctest::Approx(7.0));
	CHECK(spotLight.InnerConeAngle == doctest::Approx(0.2));
	CHECK(spotLight.OuterConeAngle == doctest::Approx(0.5));
	CHECK(spot.GetComponent<TransformComponent>().Scale.x == doctest::Approx(2.0));

	// The prefab survives its file format.
	Ref<PrefabAsset> reloaded = PrefabAsset::FromJson(prefab->ToJson(), error);
	REQUIRE_MESSAGE(reloaded, error);
	CHECK(reloaded->GetEntityCount() == 8);
	CHECK(reloaded->ToJson() == prefab->ToJson());
}

TEST_CASE("ModelPrefab: each animation plays on the entity it is meant for, one clip per entity")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("ModelPrefabAnimations");
	nlohmann::json document = Paralax::Testing::MakeTriangleDocument(directory);
	Paralax::Testing::AddCamerasAndLights(document);
	Paralax::Testing::AddAnimations(document);
	// A second clip for the lamp, which already plays LampBob.
	document["animations"].push_back(document["animations"][0]);
	document["animations"][3]["name"] = "LampBobAgain";
	MeshImportResult model;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), model, error), error);
	REQUIRE(model.Animations.size() == 4);

	std::vector<std::string> warnings;
	Ref<PrefabAsset> prefab = BuildModelPrefab(model, "Rig", error, &warnings);
	REQUIRE_MESSAGE(prefab, error);
	Scene scene;
	Entity root = prefab->Instantiate(scene);
	CHECK(root.GetComponent<AnimationComponent>().Clip == model.Animations[2].Clip->Handle); // Mixed: on the root.
	Entity camera = Child(scene, root, "Camera");
	CHECK(camera.GetComponent<AnimationComponent>().Clip == model.Animations[1].Clip->Handle);
	Entity lamp = Child(scene, Child(scene, root, "Sun"), "Lamp");
	const auto& bob = lamp.GetComponent<AnimationComponent>();
	CHECK(bob.Clip == model.Animations[0].Clip->Handle);
	CHECK(bob.Playing);
	CHECK(bob.Loop);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].find("LampBobAgain") != std::string::npos);
	CHECK_FALSE(Child(scene, root, "Node0").HasComponent<AnimationComponent>());
}

TEST_CASE("ModelPrefab: a node's cast_shadow extra sets its Mesh component's CastShadows")
{
	// As Blender writes an object's custom property: the bay's bulb proxies must not shadow the lights inside them.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("ModelPrefabCastShadow");
	nlohmann::json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["nodes"] = {{{"name", "Bulb"}, {"mesh", 0}, {"extras", {{"cast_shadow", false}}}},
						 {{"name", "Wall"}, {"mesh", 0}, {"extras", {{"cast_shadow", true}}}},
						 {{"name", "Floor"}, {"mesh", 0}}};
	document["scenes"][0]["nodes"] = {0, 1, 2};
	MeshImportResult model;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), model, error), error);
	Ref<PrefabAsset> prefab = BuildModelPrefab(model, "Bay", error);
	REQUIRE_MESSAGE(prefab, error);

	const auto castShadows = [](const PrefabAsset& asset) {
		Scene scene;
		Entity root = asset.Instantiate(scene);
		std::vector<bool> values;
		for (const char* name : {"Bulb", "Wall", "Floor"})
			values.push_back(Child(scene, root, name).GetComponent<MeshComponent>().CastShadows);
		return values;
	};
	CHECK(castShadows(*prefab) == std::vector<bool>{false, true, true}); // Unspecified: the default.

	// The setting survives the prefab's file format.
	const std::filesystem::path file = directory / "Bay.pxprefab";
	REQUIRE(prefab->SaveToFile(file));
	Ref<PrefabAsset> reloaded = PrefabAsset::LoadFromFile(file);
	REQUIRE(reloaded);
	CHECK(reloaded->ToJson() == prefab->ToJson());
	CHECK(castShadows(*reloaded) == std::vector<bool>{false, true, true});
}

TEST_CASE("ModelPrefab: a model without nodes is refused, and an empty root name is replaced")
{
	MeshImportResult model;
	std::string error;
	CHECK_FALSE(BuildModelPrefab(model, "Empty", error));
	CHECK(error.find("no nodes") != std::string::npos);

	model.Nodes.push_back(ImportedNode{"Only", 7, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 0, std::nullopt, std::nullopt, std::nullopt});
	Ref<PrefabAsset> prefab = BuildModelPrefab(model, "", error); // A parent index that is not earlier: under the root.
	REQUIRE_MESSAGE(prefab, error);
	CHECK(prefab->GetRootName() == "Model");
	CHECK(prefab->GetEntityCount() == 2);
}

TEST_CASE("ModelPrefab: a root name longer than an entity name may be is refused")
{
	MeshImportResult model;
	model.Nodes.push_back(ImportedNode{"Only", std::nullopt, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 0, std::nullopt, std::nullopt});
	std::string error;
	// The prefab file would refuse the name when loaded, so the prefab is never built with it.
	CHECK_FALSE(BuildModelPrefab(model, std::string(TagComponent::MaxLength + 1, 'r'), error));
	CHECK(error.find("at most 256 bytes") != std::string::npos);
	Ref<PrefabAsset> prefab = BuildModelPrefab(model, std::string(TagComponent::MaxLength, 'r'), error);
	REQUIRE_MESSAGE(prefab, error);
	CHECK(prefab->GetRootName() == std::string(TagComponent::MaxLength, 'r'));
}
