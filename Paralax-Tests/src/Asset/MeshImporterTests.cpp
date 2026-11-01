#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Renderer/Image.h>

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>

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
