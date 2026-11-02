#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Core/JsonText.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Renderer/Image.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Components.h>

#include <glm/gtc/epsilon.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>

using namespace Paralax;
using json = nlohmann::json;

namespace {

	std::filesystem::path WriteTriangleGltf(const std::filesystem::path& directory, bool withNormals = true)
	{
		return Paralax::Testing::WriteGltf(directory, Paralax::Testing::MakeTriangleDocument(directory, withNormals));
	}

}

TEST_CASE("MeshImporter: imports geometry, node transforms, materials and textures from glTF")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfImport");
	const std::filesystem::path path = WriteTriangleGltf(directory);

	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(path, UUID(1000), result, error), error);
	CHECK(result.Warnings.empty());

	const MeshAsset& mesh = *result.Mesh;
	CHECK(mesh.Handle == UUID(1000));
	REQUIRE(mesh.Submeshes.size() == 2);
	CHECK(mesh.Vertices.size() == 6);
	CHECK(mesh.Indices.size() == 6);
	CHECK(mesh.Submeshes[0].Name == "Tri");

	// Root node at (10,0,0); child accumulates the parent transform: (10,5,0).
	CHECK(glm::vec3(mesh.Submeshes[0].Transform[3]) == glm::vec3(10.0f, 0.0f, 0.0f));
	CHECK(glm::vec3(mesh.Submeshes[1].Transform[3]) == glm::vec3(10.0f, 5.0f, 0.0f));
	CHECK(mesh.Bounds.Min == glm::vec3(10.0f, 0.0f, 0.0f));
	CHECK(mesh.Bounds.Max == glm::vec3(11.0f, 6.0f, 0.0f));
	CHECK(mesh.Vertices[1].UV == glm::vec2(1.0f, 0.0f));
	CHECK(mesh.Vertices[0].Normal == glm::vec3(0.0f, 0.0f, 1.0f));
	CHECK(glm::vec3(mesh.Vertices[0].Tangent) == glm::vec3(1.0f, 0.0f, 0.0f));

	REQUIRE(result.Materials.size() == 1);
	const MaterialAsset& material = *result.Materials[0];
	CHECK(mesh.Materials == std::vector<AssetHandle>{material.Handle});
	CHECK(material.BaseColorFactor == glm::vec4(0.5f, 0.25f, 1.0f, 1.0f));
	CHECK(material.MetallicFactor == 0.75f);
	CHECK(material.RoughnessFactor == 0.2f);
	CHECK(material.EmissiveFactor == glm::vec3(1.0f, 0.0f, 0.0f));
	CHECK(material.Alpha == AlphaMode::Mask);
	CHECK(material.AlphaCutoff == 0.3f);
	CHECK(material.DoubleSided);

	REQUIRE(result.Textures.size() == 1);
	CHECK(material.BaseColorTexture == result.Textures[0]->Handle);
	CHECK(result.Textures[0]->SRGB);
	CHECK(result.Textures[0]->Width == 2);
	CHECK(result.Textures[0]->Mips[0][0] == 255);

	// Sub-asset handles are deterministic: re-importing yields the same handles.
	MeshImportResult again;
	REQUIRE(MeshImporter::Import(path, UUID(1000), again, error));
	CHECK(again.Materials[0]->Handle == material.Handle);
	CHECK(again.Textures[0]->Handle == result.Textures[0]->Handle);
	CHECK(DeriveSubAssetHandle(UUID(1000), SubAssetKind::Material, 0) != DeriveSubAssetHandle(UUID(1001), SubAssetKind::Material, 0));
	CHECK(DeriveSubAssetHandle(UUID(1000), SubAssetKind::Material, 0) != DeriveSubAssetHandle(UUID(1000), SubAssetKind::Texture, 0));
}

TEST_CASE("MeshImporter: imports binary glTF (GLB)")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GlbImport");
	const std::vector<uint8_t> glb = Paralax::Testing::MakeTriangleGlb(directory);
	const std::filesystem::path path = directory / "triangle.glb";
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));

	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(path, UUID(1000), result, error), error);
	CHECK(result.Warnings.empty());
	REQUIRE(result.Mesh->Submeshes.size() == 2);
	CHECK(result.Mesh->Vertices[1].UV == glm::vec2(1.0f, 0.0f));
	CHECK(result.Mesh->Indices == std::vector<uint32_t>{0, 1, 2, 0, 1, 2});
	CHECK(result.Textures.size() == 1);
}

TEST_CASE("MeshImporter: records the scene's node hierarchy with per-node meshes, cameras and lights")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfNodes");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	Paralax::Testing::AddCamerasAndLights(document);
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);

	// Pre-order, parents first; nodes without names are named after their index.
	REQUIRE(result.Nodes.size() == 7);
	std::vector<std::string> names;
	std::vector<std::optional<size_t>> parents;
	for (const ImportedNode& node : result.Nodes)
	{
		names.push_back(node.Name);
		parents.push_back(node.Parent);
	}
	CHECK(names == std::vector<std::string>{"Node0", "Node1", "Camera", "TopView", "Sun", "Lamp", "Spot"});
	CHECK(parents == std::vector<std::optional<size_t>>{std::nullopt, 0, std::nullopt, 2, std::nullopt, 4, 4});

	// Local transforms, from TRS or decomposed from a matrix.
	CHECK(result.Nodes[0].Translation == glm::vec3(10.0f, 0.0f, 0.0f));
	CHECK(result.Nodes[1].Translation == glm::vec3(0.0f, 5.0f, 0.0f)); // Relative to its parent.
	const ImportedNode& spot = result.Nodes[6];
	CHECK(glm::all(glm::epsilonEqual(spot.Translation, glm::vec3(1.0f, 2.0f, 3.0f), 1e-5f)));
	CHECK(glm::all(glm::epsilonEqual(spot.Scale, glm::vec3(2.0f), 1e-5f)));
	CHECK(glm::all(glm::epsilonEqual(spot.Rotation * glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 1e-5f)));
	// TopView's TRS rotation (-90 degrees about X, as [x, y, z, w]) turns its view (-Z) straight down.
	CHECK(glm::all(glm::epsilonEqual(result.Nodes[3].Rotation * glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f), 1e-5f)));

	// One mesh shared by two nodes: one per-node mesh, in its own space, with the shared material slots.
	REQUIRE(result.NodeMeshes.size() == 1);
	const MeshAsset& nodeMesh = *result.NodeMeshes[0];
	CHECK(nodeMesh.Handle.IsValid());
	CHECK(nodeMesh.Handle != result.Mesh->Handle);
	CHECK(result.Nodes[0].Mesh == nodeMesh.Handle);
	CHECK(result.Nodes[1].Mesh == nodeMesh.Handle);
	CHECK(result.Nodes[2].Mesh == AssetHandle(0));
	CHECK(nodeMesh.Submeshes.size() == 1);
	CHECK(nodeMesh.Submeshes[0].Transform == glm::mat4(1.0f));
	CHECK(nodeMesh.Bounds.Max == glm::vec3(1.0f, 1.0f, 0.0f));
	CHECK(nodeMesh.Materials == result.Mesh->Materials);
	// The combined mesh is unchanged: both instances, with their world transforms.
	REQUIRE(result.Mesh->Submeshes.size() == 2);
	CHECK(glm::vec3(result.Mesh->Submeshes[1].Transform[3]) == glm::vec3(10.0f, 5.0f, 0.0f));

	// Cameras.
	REQUIRE(result.Nodes[2].Camera);
	CHECK_FALSE(result.Nodes[2].Camera->Orthographic);
	CHECK(result.Nodes[2].Camera->FieldOfView == doctest::Approx(0.8));
	CHECK(result.Nodes[2].Camera->Near == doctest::Approx(0.05));
	CHECK(result.Nodes[2].Camera->Far == doctest::Approx(500.0));
	REQUIRE(result.Nodes[3].Camera);
	CHECK(result.Nodes[3].Camera->Orthographic);
	CHECK(result.Nodes[3].Camera->OrthographicSize == doctest::Approx(6.0)); // Twice ymag.

	// Lights, back in the strengths they were exported from.
	REQUIRE(result.Nodes[4].Light);
	CHECK(result.Nodes[4].Light->Type == ImportedLightType::Directional);
	CHECK(result.Nodes[4].Light->Intensity == doctest::Approx(3.0));
	CHECK(glm::all(glm::epsilonEqual(result.Nodes[4].Light->Color, glm::vec3(1.0f, 0.9f, 0.8f), 1e-6f)));
	REQUIRE(result.Nodes[5].Light);
	CHECK(result.Nodes[5].Light->Type == ImportedLightType::Point);
	CHECK(result.Nodes[5].Light->Intensity == doctest::Approx(80.0));
	CHECK(result.Nodes[5].Light->Range == doctest::Approx(10.0 * std::sqrt(80.0))); // No range: where it falls to 0.01.
	REQUIRE(spot.Light);
	CHECK(spot.Light->Type == ImportedLightType::Spot);
	CHECK(spot.Light->Intensity == doctest::Approx(1.0));
	CHECK(spot.Light->Range == doctest::Approx(7.0));
	CHECK(spot.Light->InnerConeAngle == doctest::Approx(0.2));
	CHECK(spot.Light->OuterConeAngle == doctest::Approx(0.5));
	CHECK_FALSE(result.Nodes[0].Light);
	CHECK_FALSE(result.Nodes[0].Camera);
}

TEST_CASE("MeshImporter: hostile node, camera and light values are made safe")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfHostileNodes");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	Paralax::Testing::AddCamerasAndLights(document);
	document["nodes"][0]["translation"] = {1e39, 0.0, 0.0};  // Overflows a float.
	document["nodes"][2]["rotation"] = {0.0, 0.0, 0.0, 0.0}; // Not a rotation.
	document["nodes"][6]["matrix"] = json::array();          // A matrix of zeros: collapsed to a point.
	for (int i = 0; i < 16; i++)
		document["nodes"][6]["matrix"].push_back(0.0);
	// Flat on one axis (glTF allows a zero scale): collapsed where it is, like a matrix of zeros.
	document["nodes"][1].erase("translation");
	document["nodes"][1]["matrix"] = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 5.0, 0.0, 1.0};
	// Every entry a float, but an axis longer than a float can hold: not a collapse, an unusable transform.
	document["nodes"][4]["matrix"] = {3e38, 3e38, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
	document["nodes"][5]["name"] = std::string(255, 'x') + "\xC3\xA9" + std::string(10, 'y'); // A 2-byte character across byte 256.
	document["cameras"][0]["perspective"]["yfov"] = 9.0;                                      // More than 179 degrees.
	document["cameras"][1]["orthographic"]["znear"] = 60.0;                                   // Beyond far.
	document["cameras"][1]["orthographic"]["ymag"] = -3.0;                                    // Negative extent.
	auto& lights = document["extensions"]["KHR_lights_punctual"]["lights"];
	lights[0]["intensity"] = 1e30;
	lights[1]["color"] = {5.0, -1.0, 0.5};
	lights[2]["spot"] = {{"innerConeAngle", 1.5}, {"outerConeAngle", 1.56}}; // Valid glTF (under pi/2), wider than 89 degrees.

	Paralax::Testing::ScopedLogCapture capture;
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	REQUIRE(result.Nodes.size() == 7);
	for (size_t i : {size_t(0), size_t(2), size_t(4)})
	{
		CAPTURE(i);
		CHECK(result.Nodes[i].Translation == glm::vec3(0.0f));
		CHECK(result.Nodes[i].Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		CHECK(result.Nodes[i].Scale == glm::vec3(1.0f));
	}
	CHECK(std::count_if(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("unusable transform") != std::string::npos; }) == 3);
	// The collapsed nodes stay collapsed, where the combined mesh shows them.
	CHECK(result.Nodes[6].Scale == glm::vec3(0.0f));
	CHECK(result.Nodes[1].Scale == glm::vec3(0.0f));
	CHECK(result.Nodes[1].Translation == glm::vec3(0.0f, 5.0f, 0.0f));
	CHECK(result.Nodes[5].Name == std::string(255, 'x'));
	CHECK(result.Nodes[2].Camera->FieldOfView == doctest::Approx(glm::radians(179.0)));
	CHECK(result.Nodes[3].Camera->Far > result.Nodes[3].Camera->Near);
	CHECK(result.Nodes[3].Camera->OrthographicSize == doctest::Approx(6.0));
	CHECK(result.Nodes[4].Light->Intensity == doctest::Approx(100000.0));
	CHECK(result.Nodes[5].Light->Color == glm::vec3(1.0f, 0.0f, 0.5f));
	CHECK(result.Nodes[6].Light->OuterConeAngle == doctest::Approx(glm::radians(89.0)));
	CHECK(result.Nodes[6].Light->InnerConeAngle <= result.Nodes[6].Light->OuterConeAngle);
}

TEST_CASE("MeshImporter: orthographic planes at the edge of their range stay within it")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfOrthographicLimits");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	Paralax::Testing::AddCamerasAndLights(document);
	// Both planes far beyond the range: each is clamped to 1e6, and far, moved past near, must not leave it.
	document["cameras"][1]["orthographic"]["znear"] = 5e6;
	document["cameras"][1]["orthographic"]["zfar"] = 5e6;
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	REQUIRE(result.Nodes[3].Camera);
	const ImportedCamera& camera = *result.Nodes[3].Camera;
	CHECK(camera.Orthographic);
	CHECK(camera.Far > camera.Near);
	CHECK(camera.Far == 1000000.0f);
	// The scene format accepts both planes, so a prefab built from the model keeps them when it is loaded.
	const ComponentInfo& info = *ComponentRegistry::Find("Camera");
	CameraComponent component;
	CHECK(ComponentRegistry::SetField(*info.FindField("OrthographicNear"), &component, camera.Near, error));
	CHECK(ComponentRegistry::SetField(*info.FindField("OrthographicFar"), &component, camera.Far, error));
}

TEST_CASE("MeshImporter: per-node meshes keep their handles when a re-export renumbers them")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfMeshNames");
	auto import = [&](const json& document) {
		MeshImportResult result;
		std::string error;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
		std::map<std::string, AssetHandle> handles; // By node name.
		for (const ImportedNode& node : result.Nodes)
			handles[node.Name] = node.Mesh;
		return handles;
	};
	// Two named meshes on two nodes, then the same file with the meshes in the opposite order (as Blender writes it
	// after objects are reordered).
	json first = Paralax::Testing::MakeTriangleDocument(directory);
	first["meshes"].push_back(first["meshes"][0]);
	first["meshes"][0]["name"] = "Table";
	first["meshes"][1]["name"] = "Chair";
	first["nodes"] = {{{"name", "TableObject"}, {"mesh", 0}}, {{"name", "ChairObject"}, {"mesh", 1}}};
	first["scenes"][0]["nodes"] = {0, 1};
	json reordered = first;
	std::swap(reordered["meshes"][0], reordered["meshes"][1]);
	reordered["nodes"][0]["mesh"] = 1;
	reordered["nodes"][1]["mesh"] = 0;

	const auto before = import(first);
	const auto after = import(reordered);
	CHECK(before.at("TableObject") != before.at("ChairObject"));
	CHECK(after.at("TableObject") == before.at("TableObject"));
	CHECK(after.at("ChairObject") == before.at("ChairObject"));

	// Unnamed or repeated names fall back to the index, and stay distinct.
	json unnamed = first;
	unnamed["meshes"][0].erase("name");
	unnamed["meshes"][1]["name"] = "";
	const auto byIndex = import(unnamed);
	json repeated = first;
	repeated["meshes"][1]["name"] = "Table";
	const auto byRepeat = import(repeated);
	CHECK(byIndex.at("TableObject") != byIndex.at("ChairObject"));
	CHECK(byRepeat.at("TableObject") != byRepeat.at("ChairObject"));
	CHECK(byRepeat.at("TableObject") == byIndex.at("TableObject")); // Both keyed by index 0.
}

TEST_CASE("MeshImporter: malformed lights are skipped with a warning and the meshes still import")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfBadLights");
	for (const json& light : {json{{"type", "laser"}, {"intensity", 1.0}}, json{{"type", "point"}, {"range", 0.0}},
							  json{{"type", "spot"}, {"intensity", 1.0}}, json{{"type", "directional"}, {"range", 5.0}}})
	{
		CAPTURE(light.dump());
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		Paralax::Testing::AddCamerasAndLights(document);
		document["extensions"]["KHR_lights_punctual"]["lights"][1] = light;
		Paralax::Testing::ScopedLogCapture capture;
		MeshImportResult result;
		std::string error;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
		CHECK(result.Mesh->Submeshes.size() == 2);
		CHECK_FALSE(std::any_of(result.Nodes.begin(), result.Nodes.end(), [](const ImportedNode& node) { return node.Light.has_value(); }));
		CHECK(std::any_of(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("lights") != std::string::npos; }));
		CHECK(result.Nodes[2].Camera); // Everything else still comes through.
	}
}

TEST_CASE("MeshImporter: parallax settings come from material extras, with 16-bit height maps")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfParallax");
	std::filesystem::create_directories(directory / "textures");
	const std::vector<uint8_t> height = Paralax::Testing::MakePng16(2, 2, {65535, 30000, 30001, 0});
	std::ofstream(directory / "textures" / "stone_height.png", std::ios::binary).write(reinterpret_cast<const char*>(height.data()), static_cast<std::streamsize>(height.size()));

	// Three materials as Blender writes them for a POM / no-POM / geometry test strip; the first two share a map.
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	const json base = document["materials"][0];
	document["materials"] = json::array();
	for (const auto& [name, pom] : {std::pair{"stone_POM", true}, std::pair{"stone_POM_too", true}, std::pair{"stone_noPOM", false}})
	{
		json material = base;
		material["name"] = name;
		material["extras"] = {{"pom", pom}, {"parallax_mode", pom ? "POM" : "normal_map_only"}, {"height_map", "stone_height.png"}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.2}};
		document["materials"].push_back(material);
	}
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	CHECK(result.Warnings.empty());
	REQUIRE(result.Materials.size() == 3);
	const MaterialAsset& pom = *result.Materials[0];
	REQUIRE(pom.HeightTexture.IsValid());
	CHECK(pom.HeightScale == doctest::Approx(0.02 / 1.2));          // Depth in UV units: one repeat is one UV unit.
	CHECK(result.Materials[1]->HeightTexture == pom.HeightTexture); // Loaded once.
	CHECK_FALSE(result.Materials[2]->HeightTexture.IsValid());      // Parallax off.

	const auto texture = std::find_if(result.Textures.begin(), result.Textures.end(), [&](const Ref<TextureAsset>& t) { return t->Handle == pom.HeightTexture; });
	REQUIRE(texture != result.Textures.end());
	CHECK((*texture)->Format == TextureFormat::RGBA32F); // 16 bits kept, linear.
	const auto* texels = reinterpret_cast<const float*>((*texture)->Mips[0].data());
	CHECK(texels[4] != texels[8]); // 30000 and 30001 stay apart.
	CHECK(std::count_if(result.Textures.begin(), result.Textures.end(), [](const Ref<TextureAsset>& t) { return t->Format == TextureFormat::RGBA32F; }) == 1);
}

TEST_CASE("MeshImporter: a height map with a non-ASCII name is found beside a model in a non-ASCII folder")
{
	// "Modelle-é-日本" and "Stein-é-日.png", as UTF-8 bytes. glTF text is UTF-8, so the name must be looked up as
	// UTF-8 rather than in the platform's narrow encoding. It is in the textures folder, so the lookup beside the
	// model is tried (and misses) first.
	const std::string heightName = "Stein-\xC3\xA9-\xE6\x97\xA5.png";
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfParallaxUtf8") / FromUtf8("Modelle-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC");
	std::filesystem::create_directories(directory / "textures");
	const std::vector<uint8_t> height = Paralax::Testing::MakePng16(2, 2, {65535, 30000, 30001, 0});
	std::ofstream(directory / "textures" / FromUtf8(heightName), std::ios::binary).write(reinterpret_cast<const char*>(height.data()), static_cast<std::streamsize>(height.size()));

	json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["materials"][0]["extras"] = {{"pom", true}, {"height_map", heightName}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}};
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	CHECK(result.Warnings.empty());
	REQUIRE_FALSE(result.Materials.empty());
	CHECK(result.Materials[0]->HeightTexture.IsValid());
}

TEST_CASE("MeshImporter: unusable parallax extras are skipped with a warning")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfParallaxBad") / "model";
	std::filesystem::create_directories(directory);
	const std::vector<uint8_t> height = Paralax::Testing::MakePng16(1, 1, {40000});
	std::ofstream(directory.parent_path() / "outside.png", std::ios::binary).write(reinterpret_cast<const char*>(height.data()), static_cast<std::streamsize>(height.size()));
	std::ofstream(directory / "inside.png", std::ios::binary).write(reinterpret_cast<const char*>(height.data()), static_cast<std::streamsize>(height.size()));
	std::ofstream(directory / "garbage.png") << "not an image";

	for (const json& extras : {json{{"pom", true}, {"height_map", "missing.png"}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}},
							   json{{"pom", true}, {"height_map", "../outside.png"}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}},
							   json{{"pom", true}, {"height_map", "garbage.png"}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}},
							   json{{"pom", true}, {"height_map", "inside.png"}, {"height_depth_m", 0.02}, {"texture_repeat_m", 0.0}},
							   json{{"pom", true}, {"height_map", "inside.png"}, {"height_depth_m", -1.0}, {"texture_repeat_m", 1.0}},
							   json{{"pom", true}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}},
							   json{{"pom", "yes"}, {"height_map", 5}, {"height_depth_m", "deep"}, {"texture_repeat_m", 1.0}}})
	{
		CAPTURE(extras.dump());
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["materials"][0]["extras"] = extras;
		Paralax::Testing::ScopedLogCapture capture;
		MeshImportResult result;
		std::string error;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
		CHECK_FALSE(result.Materials[0]->HeightTexture.IsValid());
		const bool parallaxRequested = extras["pom"].is_boolean();
		CHECK(std::any_of(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("material") != std::string::npos; }) == parallaxRequested);
	}

	// Every material naming a missing map is told so (the map is looked for once).
	{
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["materials"].push_back(document["materials"][0]);
		for (json& material : document["materials"])
			material["extras"] = {{"pom", true}, {"height_map", "missing.png"}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}};
		Paralax::Testing::ScopedLogCapture capture;
		MeshImportResult result;
		std::string error;
		REQUIRE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error));
		CHECK(std::count_if(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("missing.png") != std::string::npos; }) == 2);
	}

	// A large depth is clamped to what the material allows.
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["materials"][0]["extras"] = {{"pom", true}, {"height_map", "inside.png"}, {"height_depth_m", 5.0}, {"texture_repeat_m", 1.0}};
	MeshImportResult result;
	std::string error;
	REQUIRE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error));
	CHECK(result.Materials[0]->HeightScale == doctest::Approx(0.5));
}

TEST_CASE("MeshImporter: extras of any type are accepted")
{
	// glTF allows extras to be any JSON value; with the material extras reader on, fastgltf v0.9.1 rejected files with
	// extras that were not objects (patched, see cmake/patches).
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfExtrasTypes");
	for (const json& value : {json("blender"), json::array({1, 2}), json(nullptr), json(3.5), json(true)})
	{
		CAPTURE(value.dump());
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		Paralax::Testing::AddCamerasAndLights(document);
		for (const char* category : {"nodes", "meshes", "materials", "accessors", "bufferViews", "buffers", "cameras", "textures", "images", "scenes"})
			document[category][0]["extras"] = value;
		document["extensions"]["KHR_lights_punctual"]["lights"][0]["extras"] = value;
		MeshImportResult result;
		std::string error;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
		CHECK(result.Nodes.size() == 7);
		CHECK(result.Nodes[4].Light); // Lights still come through.
	}
}

TEST_CASE("MeshImporter: a node's extras.cast_shadow says whether its mesh casts shadows")
{
	// As Blender's exporter writes an object's custom property: bulb meshes around point lights must not shadow them.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfCastShadow");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["nodes"] = {{{"name", "Bulb"}, {"mesh", 0}, {"extras", {{"cast_shadow", false}}}},
						 {{"name", "Wall"}, {"mesh", 0}, {"extras", {{"cast_shadow", true}}}},
						 {{"name", "Floor"}, {"mesh", 0}, {"extras", {{"note", "no cast_shadow here"}}}}};
	document["scenes"][0]["nodes"] = {0, 1, 2};
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	CHECK(result.Warnings.empty());
	REQUIRE(result.Nodes.size() == 3);
	CHECK(result.Nodes[0].CastShadows == std::optional<bool>(false));
	CHECK(result.Nodes[1].CastShadows == std::optional<bool>(true));
	CHECK_FALSE(result.Nodes[2].CastShadows.has_value()); // Not specified.

	// Material extras are still read beside them (one callback serves both).
	document["materials"][0]["extras"] = {{"pom", true}, {"height_map", "albedo.png"}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}};
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	CHECK(result.Materials[0]->HeightTexture.IsValid());
	CHECK(result.Nodes[0].CastShadows == std::optional<bool>(false));
}

TEST_CASE("MeshImporter: an extras.cast_shadow that is not a boolean is ignored with one warning")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfCastShadowHostile");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["nodes"] = json::array();
	document["nodes"].push_back({{"mesh", 0}, {"extras", {{"cast_shadow", false}}}}); // Valid, beside the rest.
	for (const json& value : {json(0), json("false"), json(nullptr), json::object(), json::array()})
		document["nodes"].push_back({{"mesh", 0}, {"extras", {{"cast_shadow", value}}}});
	document["nodes"].push_back({{"mesh", 0}, {"extras", "cast_shadow"}});            // Extras that are not an object.
	document["nodes"].push_back({{"mesh", 0}, {"extras", {{"cast_shadow", "off"}}}}); // Not in the scene: not counted.
	document["scenes"][0]["nodes"] = {0, 1, 2, 3, 4, 5, 6};

	Paralax::Testing::ScopedLogCapture capture;
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	REQUIRE(result.Nodes.size() == 7);
	CHECK(result.Nodes[0].CastShadows == std::optional<bool>(false));
	for (size_t i = 1; i < result.Nodes.size(); i++)
	{
		CAPTURE(i);
		CHECK_FALSE(result.Nodes[i].CastShadows.has_value());
	}
	const auto mentions = [](const std::string& warning) { return warning.find("cast_shadow") != std::string::npos; };
	REQUIRE(std::count_if(result.Warnings.begin(), result.Warnings.end(), mentions) == 1);
	const std::string& warning = *std::find_if(result.Warnings.begin(), result.Warnings.end(), mentions);
	CHECK(warning.find("5 node(s)") != std::string::npos);
	CHECK(warning.find("node 1;") != std::string::npos); // The first of them.
}

TEST_CASE("MeshImporter: node extras survive the retry without malformed lights")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfCastShadowBadLights");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	Paralax::Testing::AddCamerasAndLights(document);
	document["extensions"]["KHR_lights_punctual"]["lights"][1] = {{"type", "laser"}, {"intensity", 1.0}};
	document["nodes"][0]["extras"] = {{"cast_shadow", false}};
	Paralax::Testing::ScopedLogCapture capture;
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	CHECK(std::any_of(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("lights") != std::string::npos; }));
	CHECK_FALSE(std::any_of(result.Nodes.begin(), result.Nodes.end(), [](const ImportedNode& node) { return node.Light.has_value(); }));
	REQUIRE(result.Nodes.size() == 7);
	CHECK(result.Nodes[0].CastShadows == std::optional<bool>(false));
	CHECK_FALSE(result.Nodes[1].CastShadows.has_value());
}

TEST_CASE("MeshImporter: animations become clips, each meant for the deepest node above what it animates")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfAnimations");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	Paralax::Testing::AddCamerasAndLights(document);
	Paralax::Testing::AddAnimations(document);
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	CHECK(result.Warnings.empty());
	REQUIRE(result.Animations.size() == 3);

	// LampBob: one node, so the lamp plays it itself.
	const ImportedAnimation& bob = result.Animations[0];
	CHECK(bob.Clip->Name == "LampBob");
	CHECK(bob.Host == std::optional<size_t>(5));
	CHECK(bob.Clip->Duration == 2.0f);
	REQUIRE(bob.Clip->Channels.size() == 1);
	CHECK(bob.Clip->Channels[0].Target.empty());
	CHECK(bob.Clip->Channels[0].Property == AnimationProperty::Translation);
	CHECK(AnimationClipAsset::Sample(bob.Clip->Channels[0], 1.0f).y == 4.0f);

	// CameraMove: the camera and its child; the camera plays it, the child is named by path.
	const ImportedAnimation& move = result.Animations[1];
	CHECK(move.Host == std::optional<size_t>(2));
	REQUIRE(move.Clip->Channels.size() == 2);
	CHECK(move.Clip->Channels[0].Target.empty());
	CHECK(move.Clip->Channels[1].Target == std::vector<std::string>{"TopView"});
	CHECK(move.Clip->Channels[1].Interpolation == AnimationInterpolation::Step);
	CHECK(move.Clip->Channels[1].Property == AnimationProperty::Rotation);

	// Mixed: nodes in different subtrees, so the model's root plays it; paths start below the root.
	const ImportedAnimation& mixed = result.Animations[2];
	CHECK_FALSE(mixed.Host.has_value());
	REQUIRE(mixed.Clip->Channels.size() == 2);
	CHECK(mixed.Clip->Channels[0].Target == std::vector<std::string>{"Node0"});
	CHECK(mixed.Clip->Channels[1].Target == std::vector<std::string>{"Sun", "Lamp"});
	CHECK(mixed.Clip->Channels[0].Interpolation == AnimationInterpolation::CubicSpline);
	CHECK(AnimationClipAsset::Sample(mixed.Clip->Channels[0], 0.5f).x == doctest::Approx(1.5));

	// Handles follow the clips' names.
	CHECK(bob.Clip->Handle != move.Clip->Handle);
	json renamed = document;
	std::swap(renamed["animations"][0], renamed["animations"][1]);
	MeshImportResult again;
	REQUIRE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, renamed), UUID(1000), again, error));
	CHECK(again.Animations[1].Clip->Handle == bob.Clip->Handle);
}

TEST_CASE("MeshImporter: animation data Paralax cannot play is skipped with warnings, and invalid data rejected")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfBadAnimations");
	auto import = [&](const std::function<void(json&)>& spoil, MeshImportResult& result) {
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		Paralax::Testing::AddCamerasAndLights(document);
		Paralax::Testing::AddAnimations(document);
		spoil(document);
		std::string error;
		Paralax::Testing::ScopedLogCapture capture;
		return MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error);
	};
	auto hasWarning = [](const MeshImportResult& result, const std::string& text) {
		return std::any_of(result.Warnings.begin(), result.Warnings.end(), [&](const std::string& w) { return w.find(text) != std::string::npos; });
	};

	// Valid glTF, not supported: morph target weights (the clip has nothing else, so it is dropped)...
	MeshImportResult weights;
	REQUIRE(import([](json& d) { d["animations"][0]["channels"][0]["target"]["path"] = "weights"; }, weights));
	CHECK(weights.Animations.size() == 2);
	CHECK(hasWarning(weights, "morph target weights"));
	CHECK(hasWarning(weights, "nothing Paralax can play"));

	// ...rotation keys stored as normalised integers (the clip keeps its other channel)...
	MeshImportResult integers;
	REQUIRE(import(
		[](json& d) {
			const size_t accessor = d["animations"][1]["samplers"][1]["output"];
			d["accessors"][accessor]["componentType"] = 5122; // SHORT, 8 bytes a key: still inside its view.
			d["accessors"][accessor]["normalized"] = true;
		},
		integers));
	REQUIRE(integers.Animations.size() == 3);
	CHECK(integers.Animations[1].Clip->Channels.size() == 1);
	CHECK(hasWarning(integers, "only float keyframes"));

	// ...and a node that is not in the scene.
	MeshImportResult orphan;
	REQUIRE(import(
		[](json& d) {
			d["nodes"].push_back({{"name", "Orphan"}});
			d["animations"][0]["channels"][0]["target"]["node"] = d["nodes"].size() - 1;
		},
		orphan));
	CHECK(orphan.Animations.size() == 2);
	CHECK(hasWarning(orphan, "outside the scene"));

	// Translation keys that are not floats are invalid glTF, but fastgltf lets them through: the importer skips the
	// channel itself.
	MeshImportResult integerTranslations;
	REQUIRE(import([](json& d) { d["accessors"][d["animations"][0]["samplers"][0]["output"].get<size_t>()]["componentType"] = 5125; }, integerTranslations));
	CHECK(integerTranslations.Animations.size() == 2);
	CHECK(hasWarning(integerTranslations, "only float keyframes"));

	// Invalid glTF is rejected as a whole: values not matching the key times, a sampler or accessor that does not
	// exist (fastgltf read past the end for these; patched, see cmake/patches).
	for (const std::function<void(json&)>& spoil : std::vector<std::function<void(json&)>>{
			 [](json& d) { d["accessors"][d["animations"][1]["samplers"][0]["output"].get<size_t>()]["count"] = 1; },
			 [](json& d) { d["animations"][1]["channels"][1]["sampler"] = 7; },
			 [](json& d) { d["animations"][0]["samplers"][0]["input"] = 999; },
			 [](json& d) { d["animations"][0]["samplers"][0]["output"] = 999; }})
	{
		MeshImportResult result;
		CHECK_FALSE(import(spoil, result));
	}
}

TEST_CASE("MeshImporter: out-of-range indices that fastgltf read unchecked are rejected")
{
	// fastgltf v0.9.1 used these indices from the file without range checks (patched, see cmake/patches); found
	// under ASan while testing animation import.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfUncheckedIndices");
	for (const std::function<void(json&)>& spoil : std::vector<std::function<void(json&)>>{
			 // A primitive without indices (GenerateMeshIndices makes them) whose POSITION accessor does not exist.
			 [](json& d) {
				 d["meshes"][0]["primitives"][0].erase("indices");
				 d["meshes"][0]["primitives"][0]["attributes"]["POSITION"] = 999;
			 },
			 // A strip without indices and with no vertices: the index count wrapped around (and was allocated).
			 [](json& d) {
				 d["meshes"][0]["primitives"][0].erase("indices");
				 d["meshes"][0]["primitives"][0]["mode"] = 5;
				 d["accessors"][0]["count"] = 0;
			 },
			 // A primitive without indices claiming far more vertices than it has: that many indices were allocated
			 // before validation (std::bad_alloc, or the memory taken).
			 [](json& d) {
				 d["meshes"][0]["primitives"][0].erase("indices");
				 d["accessors"][0]["count"] = 4000000000000;
			 },
			 // A sparse accessor naming buffer views that do not exist.
			 [](json& d) {
				 d["accessors"][1]["sparse"] = {{"count", 1}, {"indices", {{"bufferView", 77}, {"componentType", 5123}}}, {"values", {{"bufferView", 78}}}};
			 }})
	{
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		spoil(document);
		MeshImportResult result;
		std::string error;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error));
	}
}

TEST_CASE("MeshImporter: an animation target sharing its name with a sibling is warned about")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfAmbiguousTargets");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	Paralax::Testing::AddCamerasAndLights(document);
	Paralax::Testing::AddAnimations(document);
	document["nodes"][6]["name"] = "Lamp"; // Spot, beside Lamp under Sun, now shares its name.
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
	// Mixed reaches Lamp by name from the root: Sun/Lamp could be either.
	CHECK(std::count_if(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("'Mixed'") != std::string::npos && w.find("sibling") != std::string::npos; }) == 1);
	// LampBob plays on the lamp itself (an empty path): nothing to confuse.
	CHECK(std::none_of(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("'LampBob'") != std::string::npos; }));
}

TEST_CASE("MeshImporter: computes normals when missing and warns about missing images")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfNoNormals");
	const std::filesystem::path path = WriteTriangleGltf(directory, false);
	std::filesystem::remove(directory / "albedo.png");

	Paralax::Testing::ScopedLogCapture capture;
	MeshImportResult result;
	std::string error;
	REQUIRE_MESSAGE(MeshImporter::Import(path, UUID(5), result, error), error);
	CHECK(result.Mesh->Vertices[0].Normal == glm::vec3(0.0f, 0.0f, 1.0f));
	CHECK(result.Materials[0]->BaseColorTexture == AssetHandle(0));
	CHECK_FALSE(result.Warnings.empty());
}

TEST_CASE("MeshImporter: rejects missing, malformed and empty files")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfBad");
	MeshImportResult result;
	std::string error;

	CHECK_FALSE(MeshImporter::Import(directory / "missing.gltf", UUID(1), result, error));
	CHECK_FALSE(error.empty());

	std::ofstream(directory / "garbage.gltf") << "{ this is not gltf";
	CHECK_FALSE(MeshImporter::Import(directory / "garbage.gltf", UUID(1), result, error));

	std::ofstream(directory / "empty.gltf") << R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[]}],"scene":0})";
	CHECK_FALSE(MeshImporter::Import(directory / "empty.gltf", UUID(1), result, error));
	CAPTURE(error);
	CHECK(error.find("no triangle meshes") != std::string::npos);
	CHECK(result.Mesh == nullptr);
}

TEST_CASE("MeshImporter: hostile files are rejected without reading out of bounds")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfHostile");
	MeshImportResult result;
	std::string error;

	SUBCASE("accessor count larger than its buffer view")
	{
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["accessors"][0]["count"] = 1000000;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("attribute referencing a missing accessor")
	{
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["meshes"][0]["primitives"][0]["attributes"]["POSITION"] = 99;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("buffer declaring more bytes than its data holds")
	{
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["buffers"][0]["byteLength"] = 100000;
		document["bufferViews"][0]["byteLength"] = 50000;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("default scene index out of range")
	{
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["scene"] = 5;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("huge zero-filled accessor")
	{
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["accessors"].push_back({{"componentType", 5126}, {"count", 4000000000000ull}, {"type", "VEC2"}});
		document["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"] = 4;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("accessor byte offset near 2^64 that wraps the end-of-data sum")
	{
		// The positions accessor spans 36 bytes of a 36-byte view: offset + 36 wraps to 0 (or 32) for the first two
		// values, which passes "end <= byteLength" in fastgltf's validation and, before the fix, the importer's own
		// check, after which the reader started 2^64 - 36 bytes into the buffer. The third is large but does not wrap,
		// so fastgltf rejects it before the importer looks.
		struct Case
		{
			uint64_t Offset;
			const char* Expected; // In the importer's error, or nullptr when fastgltf rejects the file first.
		};
		for (const Case& c : {Case{18446744073709551580ull, "starts 18446744073709551580 bytes into a buffer view of 36 bytes"}, Case{18446744073709551612ull, "starts 18446744073709551612 bytes"}, Case{18446744073709551000ull, nullptr}})
		{
			CAPTURE(c.Offset);
			json document = Paralax::Testing::MakeTriangleDocument(directory);
			document["accessors"][0]["byteOffset"] = c.Offset;
			CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
			CAPTURE(error);
			CHECK_FALSE(error.empty());
			if (c.Expected)
				CHECK(error.find(c.Expected) != std::string::npos);
			CHECK(result.Mesh == nullptr);
		}
	}
	SUBCASE("buffer view stride larger than the view")
	{
		// Three positions 252 bytes apart (fastgltf's largest stride) reach 516 bytes of a 36-byte view. fastgltf's
		// validation rejects that sum; with a wrapping offset (2^64 - 516: the sum comes out as exactly 0) fastgltf is
		// blind and the importer's offset check decides. The importer's stride check behind it is a backstop that no
		// document reaches today, kept so the end-of-data arithmetic never depends on fastgltf's.
		struct Case
		{
			uint64_t Offset;
			const char* Expected;
		};
		for (const Case& c : {Case{0, "is not a valid glTF file"}, Case{18446744073709551100ull, "starts 18446744073709551100 bytes into a buffer view of 36 bytes"}})
		{
			CAPTURE(c.Offset);
			json document = Paralax::Testing::MakeTriangleDocument(directory);
			document["bufferViews"][0]["byteStride"] = 252;
			if (c.Offset)
				document["accessors"][0]["byteOffset"] = c.Offset;
			CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
			CAPTURE(error);
			CHECK(error.find(c.Expected) != std::string::npos);
			CHECK(result.Mesh == nullptr);
		}
	}
	SUBCASE("a percent-encoded image URI that is not UTF-8 is skipped, never thrown")
	{
		// "%FF" decodes to a byte no path encoding accepts on Windows; the importer must treat it as an unusable image.
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["images"] = json::array({json{{"uri", "%FF%FE.png"}}});
		document["textures"] = json::array({json{{"source", 0}}});
		document["materials"] = json::array({json{{"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}}}});
		document["meshes"][0]["primitives"][0]["material"] = 0;
		bool imported = false;
		CHECK_NOTHROW(imported = MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
		CAPTURE(error);
		CHECK(imported);
		CHECK(result.Textures.empty());
	}
	SUBCASE("a model file above MaxBufferBytes is refused before it is read")
	{
		std::ofstream(directory / "huge.glb", std::ios::binary).put('g');
		std::filesystem::resize_file(directory / "huge.glb", MeshImporter::MaxBufferBytes + 1);
		CHECK_FALSE(MeshImporter::Import(directory / "huge.glb", UUID(1), result, error));
		CAPTURE(error);
		CHECK(error.find("too large") != std::string::npos);
		CHECK(result.Mesh == nullptr);
	}
	SUBCASE("a real non-zero accessor offset still imports")
	{
		// Normals read from the positions' view, 36 bytes in (the two blocks are adjacent in the buffer).
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["bufferViews"][0]["byteLength"] = 72;
		document["accessors"][1]["bufferView"] = 0;
		document["accessors"][1]["byteOffset"] = 36;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error), error);
		CHECK(result.Mesh->Vertices[0].Normal == glm::vec3(0.0f, 0.0f, 1.0f));
	}
}

TEST_CASE("MeshImporter: external buffers stay inside the model's directory and under the size cap")
{
	// fastgltf would follow any buffer URI and read any size; the importer reads buffers itself, like images.
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("GltfExternalBuffers");
	const std::filesystem::path directory = root / "model";
	std::filesystem::create_directories(directory);
	const std::vector<uint8_t> triangle = Paralax::Testing::MakeTriangleBuffer();
	auto writeBytes = [](const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	};
	// The triangle with its buffer in a file named by uri instead of embedded.
	auto externalDocument = [&](const std::string& uri) {
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["buffers"][0]["uri"] = uri;
		return document;
	};
	MeshImportResult result;
	std::string error;

	SUBCASE("a .bin beside the model, in a subfolder or with a percent-encoded name imports")
	{
		std::filesystem::create_directories(directory / "data");
		writeBytes(directory / "triangle.bin", triangle);
		writeBytes(directory / "data" / "triangle.bin", triangle);
		writeBytes(directory / "tri angle.bin", triangle);
		for (const char* uri : {"triangle.bin", "data/triangle.bin", "tri%20angle.bin"})
		{
			CAPTURE(std::string(uri));
			REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, externalDocument(uri)), UUID(1), result, error), error);
			CHECK(result.Warnings.empty());
			CHECK(result.Mesh->Vertices.size() == 6);
			CHECK(result.Mesh->Vertices[1].Position == glm::vec3(1.0f, 0.0f, 0.0f));
			CHECK(result.Mesh->Indices.size() == 6);
		}
	}
	SUBCASE("a path leading out of the directory is refused and its bytes are never used")
	{
		// Valid data: were it read, the import would succeed.
		writeBytes(root / "escape.bin", triangle);
		for (const char* uri : {"../escape.bin", "data/../../escape.bin", "./../escape.bin"})
		{
			CAPTURE(std::string(uri));
			CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, externalDocument(uri)), UUID(1), result, error));
			CAPTURE(error);
			CHECK(error.find("buffer 0") != std::string::npos);
			CHECK(error.find("leaves the model's directory") != std::string::npos);
			CHECK(result.Mesh == nullptr);
		}
	}
	SUBCASE("an absolute path is refused, as a plain path and as a file URL")
	{
		const std::filesystem::path outside = root / "absolute.bin";
		writeBytes(outside, triangle);
		for (const std::string& uri : {ToUtf8(outside), "file://" + ToUtf8(outside)})
		{
			CAPTURE(uri);
			CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, externalDocument(uri)), UUID(1), result, error));
			CAPTURE(error);
			CHECK(error.find("buffer 0") != std::string::npos);
			CHECK(result.Mesh == nullptr);
		}
	}
	SUBCASE("a URI that is not a local file is refused")
	{
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, externalDocument("https://example.com/triangle.bin")), UUID(1), result, error));
		CAPTURE(error);
		CHECK(error.find("buffer 0") != std::string::npos);
		CHECK(error.find("not a local file") != std::string::npos);
	}
	SUBCASE("a missing file names the buffer")
	{
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, externalDocument("missing.bin")), UUID(1), result, error));
		CAPTURE(error);
		CHECK(error.find("buffer 0") != std::string::npos);
		CHECK(error.find("'missing.bin' not found") != std::string::npos);
	}
	SUBCASE("a file above MaxBufferBytes is refused before it is read")
	{
		// A sparse file, so this costs nothing; the importer checks the size first and never reads it.
		writeBytes(directory / "huge.bin", triangle);
		std::filesystem::resize_file(directory / "huge.bin", MeshImporter::MaxBufferBytes + 1);
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, externalDocument("huge.bin")), UUID(1), result, error));
		CAPTURE(error);
		CHECK(error.find("buffer 0") != std::string::npos);
		CHECK(error.find("too large") != std::string::npos);
		CHECK(result.Mesh == nullptr);
	}
	SUBCASE("the cap covers all of a model's buffers together")
	{
		// Two files just over half the cap each: the first is read (sparse zeros), the second would go over it.
		const size_t half = MeshImporter::MaxBufferBytes / 2 + 1;
		writeBytes(directory / "first.bin", triangle);
		std::filesystem::resize_file(directory / "first.bin", half);
		writeBytes(directory / "second.bin", triangle);
		std::filesystem::resize_file(directory / "second.bin", half);
		json document = externalDocument("first.bin");
		document["buffers"][0]["byteLength"] = half;
		document["buffers"].push_back({{"byteLength", half}, {"uri", "second.bin"}});
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
		CAPTURE(error);
		CHECK(error.find("buffer 1") != std::string::npos);
		CHECK(error.find("too large") != std::string::npos);
	}
	SUBCASE("embedded data and GLB chunks are untouched")
	{
		// A document with no external file at all imports with no file beside it.
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, Paralax::Testing::MakeTriangleDocument(directory)), UUID(1), result, error), error);
		CHECK(result.Mesh->Vertices.size() == 6);
		const std::vector<uint8_t> glb = Paralax::Testing::MakeTriangleGlb(directory);
		writeBytes(directory / "triangle.glb", glb);
		REQUIRE_MESSAGE(MeshImporter::Import(directory / "triangle.glb", UUID(1), result, error), error);
		CHECK(result.Mesh->Vertices.size() == 6);
	}
}

TEST_CASE("MeshImporter: malformed accessor types are rejected without reading past them")
{
	// fastgltf v0.9.1 read characters [0] and [3] of the type without checking its length (patched, see
	// cmake/patches); found by the loader fuzz tests.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MeshImporterAccessorTypes");
	for (const char* type : {"", "V", "MA", "VEC", "VEC9", "MAT", "SCALARS", "vec3"})
	{
		CAPTURE(type);
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["accessors"][0]["type"] = type;
		MeshImportResult result;
		std::string error;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}
	// The original matched these by their first and fourth characters, so it accepted them as the right types.
	for (const auto& [accessor, type] : {std::pair{0, "VEC3X"}, std::pair{3, "SCALARX"}, std::pair{3, "S"}})
	{
		CAPTURE(accessor);
		CAPTURE(std::string(type));
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["accessors"][accessor]["type"] = type;
		MeshImportResult result;
		std::string error;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}
}

TEST_CASE("MeshImporter: accessors of the wrong element type for their use are rejected")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MeshImporterWrongTypes");

	// fastgltf validates vertex attribute types itself; the importer checks them too, in case it ever stops.
	for (const auto& [accessor, type] : {std::pair{0, "VEC2"}, std::pair{1, "VEC2"}, std::pair{2, "SCALAR"}})
	{
		CAPTURE(accessor);
		CAPTURE(std::string(type));
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["accessors"][accessor]["type"] = type;
		MeshImportResult result;
		std::string error;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}

	// It does not validate the indices, and its typed readers only assert the type, so a mismatch would write past
	// the destination; found by the loader fuzz tests. One element of each type fits in the 6-byte index view, so
	// only the type is wrong.
	for (const char* type : {"VEC2", "VEC3"})
	{
		CAPTURE(std::string(type));
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["accessors"][3]["type"] = type;
		document["accessors"][3]["count"] = 1;
		MeshImportResult result;
		std::string error;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
		CAPTURE(error);
		CHECK(error.find("has type") != std::string::npos);
	}

	// Indices must be unsigned integers.
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["accessors"][3]["componentType"] = 5122; // SHORT: the same size as the valid UNSIGNED_SHORT.
	MeshImportResult result;
	std::string error;
	CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	CAPTURE(error);
	CHECK(error.find("unsigned integers") != std::string::npos);
}

TEST_CASE("MeshImporter: embedded data of a malformed base64 length is rejected without reading past it")
{
	// fastgltf v0.9.1 assumed whole groups of four characters from the base64 alphabet (patched, see cmake/patches):
	// shorter data read past its end, and other bytes past the decoding table. Found by the fuzz tests and their review.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MeshImporterBase64");
	for (const char* data : {"", "A", "AB", "ABC", "ABCDE", "AAAAAA==", "A\xC3\xA9=", "AAA*", "AB=C", "A===", "=AAA", "AAAA AAA"})
	{
		CAPTURE(std::string(data));
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["buffers"][0]["uri"] = std::string("data:application/octet-stream;base64,") + data;
		MeshImportResult result;
		std::string error;
		CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	}

	// Padded data is still accepted: the triangle's buffer is 104 bytes, which needs "=", and two more bytes need "==".
	for (const auto& [extra, padding] : {std::pair{size_t(0), "="}, std::pair{size_t(2), "=="}})
	{
		CAPTURE(extra);
		json document = Paralax::Testing::MakeTriangleDocument(directory, true, extra);
		REQUIRE(document["buffers"][0]["uri"].get<std::string>().ends_with(padding));
		MeshImportResult result;
		std::string error;
		CHECK_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error), error);
	}
}

TEST_CASE("MeshImporter: percent escapes in image URIs decode only when whole")
{
	// fastgltf v0.9.1 read two characters past any '%', even at the end of the URI (patched, see cmake/patches).
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MeshImporterPercent");
	struct Case
	{
		const char* Uri;
		bool Found;
	};
	for (const Case& c : {Case{"albedo%2Epng", true}, Case{"albedo%2epng", true}, Case{"albedo.png%", false}, Case{"albedo.png%A", false},
						  Case{"albedo.png%00", false}, Case{"albedo.png%zz", false}, Case{"%", false}})
	{
		CAPTURE(std::string(c.Uri));
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		document["images"][0]["uri"] = c.Uri;
		Paralax::Testing::ScopedLogCapture capture;
		MeshImportResult result;
		std::string error;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error), error);
		CHECK(result.Textures.size() == (c.Found ? 1u : 0u));
	}
}

TEST_CASE("MeshImporter: node cycles and shared nodes are skipped instead of recursing forever")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfCycle");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["nodes"][1]["children"] = {0}; // 0 -> 1 -> 0
	MeshImportResult result;
	std::string error;
	const bool imported = MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error);
	// fastgltf's validator may reject the cycle outright; either way nothing crashes and nothing loops.
	if (imported)
		CHECK(result.Mesh->Submeshes.size() == 2);
	else
		CHECK_FALSE(error.empty());
}

TEST_CASE("MeshImporter: image URIs cannot escape the model's directory")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("GltfTraversal");
	std::filesystem::create_directories(root / "model");
	Image(2, 2).SavePNG(root / "secret.png");
	json document = Paralax::Testing::MakeTriangleDocument(root / "model");
	document["images"][0]["uri"] = "../secret.png";

	MeshImportResult result;
	std::string error;
	REQUIRE(MeshImporter::Import(Paralax::Testing::WriteGltf(root / "model", document), UUID(1), result, error));
	CHECK(result.Textures.empty());
	CHECK(result.Materials[0]->BaseColorTexture == AssetHandle(0));
	bool warned = false;
	for (const std::string& warning : result.Warnings)
		warned |= warning.find("leaves the model's directory") != std::string::npos;
	CHECK(warned);
}

TEST_CASE("MeshImporter: primitives whose index count is not a multiple of 3 are skipped")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfPartial");
	json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["accessors"][3]["count"] = 2;
	MeshImportResult result;
	std::string error;
	CHECK_FALSE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1), result, error));
	CHECK(error.find("no triangle meshes") != std::string::npos);
}

TEST_CASE("MeshImporter: derived sub-asset handles never fall in the reserved built-in range")
{
	for (uint64_t parent = 0; parent < 2000; parent++)
		for (uint64_t index = 0; index < 4; index++)
			REQUIRE(static_cast<uint64_t>(DeriveSubAssetHandle(UUID(parent), SubAssetKind::Material, index)) >= AssetManager::FirstUserHandle);
}

TEST_CASE("MeshImporter: warnings name the file's meshes, materials and paths briefly, however long they are")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfLongNames");
	const std::string huge(16 * 1024 * 1024, 'n');
	const std::string hugeSize = fmt::format("...' ({} bytes)", huge.size());
	// Each import gets one 16 MiB name or path, where a warning would echo it.
	auto import = [&](const std::function<void(json&)>& edit) {
		json document = Paralax::Testing::MakeTriangleDocument(directory);
		edit(document);
		Paralax::Testing::ScopedLogCapture log;
		MeshImportResult result;
		std::string error;
		REQUIRE_MESSAGE(MeshImporter::Import(Paralax::Testing::WriteGltf(directory, document), UUID(1000), result, error), error);
		for (const std::string& warning : result.Warnings)
			CHECK(warning.size() < 300);
		for (const Paralax::Testing::ScopedLogCapture::Entry& entry : log.GetEntries())
			CHECK(entry.Message.size() < 300);
		return result.Warnings;
	};
	auto mentions = [](const std::vector<std::string>& warnings, const std::string& text) {
		return std::any_of(warnings.begin(), warnings.end(), [&](const std::string& warning) { return warning.find(text) != std::string::npos; });
	};

	// A mesh with a primitive that is not a triangle list, beside one that is.
	const std::vector<std::string> mesh = import([&](json& document) {
		document["meshes"][0]["name"] = huge;
		json points = document["meshes"][0]["primitives"][0];
		points["mode"] = 0;
		document["meshes"][0]["primitives"].push_back(points);
	});
	CHECK(mentions(mesh, "mesh 'nnn"));
	CHECK(mentions(mesh, hugeSize + " primitive 1: only triangle lists are supported"));

	// A material asking for parallax without its parameters.
	CHECK(mentions(import([&](json& document) {
					   document["materials"][0]["name"] = huge;
					   document["materials"][0]["extras"] = {{"pom", true}};
				   }),
				   hugeSize + ": parallax needs height_map"));
	// Height map names longer than 1024 bytes are ignored; one that long is named briefly too.
	const std::string longHeightMap(1024, 'h');
	const std::vector<std::string> heightMap = import([&](json& document) { document["materials"][0]["extras"] = {{"pom", true}, {"height_map", longHeightMap}, {"height_depth_m", 0.02}, {"texture_repeat_m", 1.0}}; });
	CHECK(mentions(heightMap, "'hhh"));
	CHECK(mentions(heightMap, "...' (1024 bytes)"));

	// An image whose file is missing, named too long to quote whole, and one named too long for any file system.
	const std::string missing = std::string(200, 'm') + ".png";
	CHECK(mentions(import([&](json& document) { document["images"][0]["uri"] = missing; }), fmt::format("file '{}...' (204 bytes) not found", std::string(MaxPreviewBytes, 'm'))));
	CHECK(mentions(import([&](json& document) { document["images"][0]["uri"] = huge + ".png"; }), fmt::format("...' ({} bytes)", huge.size() + 4)));

	// A name with a line break and a terminal escape cannot break or restyle a log line.
	CHECK(mentions(import([&](json& document) {
					   document["materials"][0]["name"] = "Two\nLines\x1B[31m";
					   document["materials"][0]["extras"] = {{"pom", true}};
				   }),
				   "material 'Two\\nLines\\u001b[31m': parallax needs"));
}
