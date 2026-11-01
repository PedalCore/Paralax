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

TEST_CASE("ModelPrefab: a model without nodes is refused, and an empty root name is replaced")
{
	MeshImportResult model;
	std::string error;
	CHECK_FALSE(BuildModelPrefab(model, "Empty", error));
	CHECK(error.find("no nodes") != std::string::npos);

	model.Nodes.push_back(ImportedNode{"Only", 7, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 0, std::nullopt, std::nullopt});
	Ref<PrefabAsset> prefab = BuildModelPrefab(model, "", error); // A parent index that is not earlier: under the root.
	REQUIRE_MESSAGE(prefab, error);
	CHECK(prefab->GetRootName() == "Model");
	CHECK(prefab->GetEntityCount() == 2);
}
