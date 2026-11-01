#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Renderer/Image.h>

#include <glm/gtc/epsilon.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
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
	for (size_t i : {size_t(0), size_t(2)})
	{
		CAPTURE(i);
		CHECK(result.Nodes[i].Translation == glm::vec3(0.0f));
		CHECK(result.Nodes[i].Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		CHECK(result.Nodes[i].Scale == glm::vec3(1.0f));
	}
	CHECK(std::count_if(result.Warnings.begin(), result.Warnings.end(), [](const std::string& w) { return w.find("unusable transform") != std::string::npos; }) == 2);
	// The collapsed node stays collapsed, where the combined mesh shows it (nowhere).
	CHECK(result.Nodes[6].Scale == glm::vec3(0.0f));
	CHECK(result.Nodes[5].Name == std::string(255, 'x'));
	CHECK(result.Nodes[2].Camera->FieldOfView == doctest::Approx(glm::radians(179.0)));
	CHECK(result.Nodes[3].Camera->Far > result.Nodes[3].Camera->Near);
	CHECK(result.Nodes[3].Camera->OrthographicSize == doctest::Approx(6.0));
	CHECK(result.Nodes[4].Light->Intensity == doctest::Approx(100000.0));
	CHECK(result.Nodes[5].Light->Color == glm::vec3(1.0f, 0.0f, 0.5f));
	CHECK(result.Nodes[6].Light->OuterConeAngle == doctest::Approx(glm::radians(89.0)));
	CHECK(result.Nodes[6].Light->InnerConeAngle <= result.Nodes[6].Light->OuterConeAngle);
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

TEST_CASE("MeshImporter: malformed lights are skipped with a warning; the meshes still import")
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
