#include <doctest/doctest.h>

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

	std::string Base64(const std::vector<uint8_t>& bytes)
	{
		static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		for (size_t i = 0; i < bytes.size(); i += 3)
		{
			const uint32_t a = bytes[i];
			const uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
			const uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
			const uint32_t triple = (a << 16) | (b << 8) | c;
			out += table[(triple >> 18) & 63];
			out += table[(triple >> 12) & 63];
			out += i + 1 < bytes.size() ? table[(triple >> 6) & 63] : '=';
			out += i + 2 < bytes.size() ? table[triple & 63] : '=';
		}
		return out;
	}

	template<typename T>
	void Append(std::vector<uint8_t>& buffer, const T& value)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
	}

	// Writes a glTF with one triangle (positions, normals, UVs, 16-bit indices) used by two nodes: a root translated
	// by (10, 0, 0) and its child translated by (0, 5, 0). The material has a base color factor and an external
	// base color texture.
	json MakeTriangleDocument(const std::filesystem::path& directory, bool withNormals = true)
	{
		Image texture(2, 2);
		texture.SetPixel(0, 0, {255, 0, 0, 255});
		REQUIRE(texture.SavePNG(directory / "albedo.png"));

		std::vector<uint8_t> buffer;
		const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
		const float normals[] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
		const float uvs[] = {0, 0, 1, 0, 0, 1};
		const uint16_t indices[] = {0, 1, 2, 0};
		for (float v : positions)
			Append(buffer, v);
		for (float v : normals)
			Append(buffer, v);
		for (float v : uvs)
			Append(buffer, v);
		for (uint16_t v : indices)
			Append(buffer, v);

		json attributes = {{"POSITION", 0}, {"TEXCOORD_0", 2}};
		if (withNormals)
			attributes["NORMAL"] = 1;

		json document = {
			{"asset", {{"version", "2.0"}}},
			{"scene", 0},
			{"scenes", {{{"nodes", {0}}}}},
			{"nodes", {{{"mesh", 0}, {"translation", {10, 0, 0}}, {"children", {1}}}, {{"mesh", 0}, {"translation", {0, 5, 0}}}}},
			{"meshes", {{{"name", "Tri"}, {"primitives", {{{"attributes", attributes}, {"indices", 3}, {"material", 0}}}}}}},
			{"materials", {{{"pbrMetallicRoughness", {{"baseColorFactor", {0.5, 0.25, 1.0, 1.0}}, {"metallicFactor", 0.75}, {"roughnessFactor", 0.2}, {"baseColorTexture", {{"index", 0}}}}}, {"emissiveFactor", {1, 0, 0}}, {"alphaMode", "MASK"}, {"alphaCutoff", 0.3}, {"doubleSided", true}}}},
			{"textures", {{{"source", 0}}}},
			{"images", {{{"uri", "albedo.png"}}}},
			{"buffers", {{{"byteLength", buffer.size()}, {"uri", "data:application/octet-stream;base64," + Base64(buffer)}}}},
			{"bufferViews", {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}}, {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}}, {{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 24}}, {{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 6}}}},
			{"accessors",
			 {{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
			  {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
			  {{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
			  {{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}}},
		};

		return document;
	}

	std::filesystem::path WriteGltf(const std::filesystem::path& directory, const json& document, const char* name = "triangle.gltf")
	{
		const std::filesystem::path path = directory / name;
		std::ofstream(path) << document.dump();
		return path;
	}

	std::filesystem::path WriteTriangleGltf(const std::filesystem::path& directory, bool withNormals = true)
	{
		return WriteGltf(directory, MakeTriangleDocument(directory, withNormals));
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
		json document = MakeTriangleDocument(directory);
		document["accessors"][0]["count"] = 1000000;
		CHECK_FALSE(MeshImporter::Import(WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("attribute referencing a missing accessor")
	{
		json document = MakeTriangleDocument(directory);
		document["meshes"][0]["primitives"][0]["attributes"]["POSITION"] = 99;
		CHECK_FALSE(MeshImporter::Import(WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("buffer declaring more bytes than its data holds")
	{
		json document = MakeTriangleDocument(directory);
		document["buffers"][0]["byteLength"] = 100000;
		document["bufferViews"][0]["byteLength"] = 50000;
		CHECK_FALSE(MeshImporter::Import(WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("default scene index out of range")
	{
		json document = MakeTriangleDocument(directory);
		document["scene"] = 5;
		CHECK_FALSE(MeshImporter::Import(WriteGltf(directory, document), UUID(1), result, error));
	}
	SUBCASE("huge zero-filled accessor")
	{
		json document = MakeTriangleDocument(directory);
		document["accessors"].push_back({{"componentType", 5126}, {"count", 4000000000000ull}, {"type", "VEC2"}});
		document["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"] = 4;
		CHECK_FALSE(MeshImporter::Import(WriteGltf(directory, document), UUID(1), result, error));
	}
}

TEST_CASE("MeshImporter: node cycles and shared nodes are skipped instead of recursing forever")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("GltfCycle");
	json document = MakeTriangleDocument(directory);
	document["nodes"][1]["children"] = {0}; // 0 -> 1 -> 0
	MeshImportResult result;
	std::string error;
	const bool imported = MeshImporter::Import(WriteGltf(directory, document), UUID(1), result, error);
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
	json document = MakeTriangleDocument(root / "model");
	document["images"][0]["uri"] = "../secret.png";

	MeshImportResult result;
	std::string error;
	REQUIRE(MeshImporter::Import(WriteGltf(root / "model", document), UUID(1), result, error));
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
	json document = MakeTriangleDocument(directory);
	document["accessors"][3]["count"] = 2;
	MeshImportResult result;
	std::string error;
	CHECK_FALSE(MeshImporter::Import(WriteGltf(directory, document), UUID(1), result, error));
	CHECK(error.find("no triangle meshes") != std::string::npos);
}

TEST_CASE("MeshImporter: derived sub-asset handles never fall in the reserved built-in range")
{
	for (uint64_t parent = 0; parent < 2000; parent++)
		for (uint64_t index = 0; index < 4; index++)
			REQUIRE(static_cast<uint64_t>(DeriveSubAssetHandle(UUID(parent), SubAssetKind::Material, index)) >= AssetManager::FirstUserHandle);
}
