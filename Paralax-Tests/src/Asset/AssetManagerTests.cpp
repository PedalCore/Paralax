#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MaterialSerializer.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/Image.h>

#include <fstream>

using namespace Paralax;

namespace {

	std::filesystem::path MakeProject(const char* name)
	{
		const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory(name);
		std::filesystem::create_directories(directory / "Textures");
		Image image(4, 4);
		image.SetPixel(1, 1, {1, 2, 3, 255});
		REQUIRE(image.SavePNG(directory / "Textures" / "Brick.png"));
		MaterialAsset material;
		material.BaseColorFactor = {0.1f, 0.2f, 0.3f, 1.0f};
		material.RoughnessFactor = 0.9f;
		REQUIRE(MaterialSerializer::SaveToFile(material, directory / "Brick.pxmat"));
		return directory;
	}

}

TEST_CASE("AssetManager: built-in assets are always available at fixed handles")
{
	AssetManager assets;
	Ref<MeshAsset> cube = assets.GetAsset<MeshAsset>(AssetHandle(AssetManager::BuiltinCube));
	REQUIRE(cube);
	CHECK(cube->Handle == AssetHandle(AssetManager::BuiltinCube));
	CHECK(assets.GetAsset<MeshAsset>(AssetHandle(AssetManager::BuiltinSphere)) != nullptr);
	CHECK(assets.GetAsset<MeshAsset>(AssetHandle(AssetManager::BuiltinCapsule)) != nullptr);
	CHECK(assets.GetAsset<MaterialAsset>(AssetHandle(AssetManager::BuiltinDefaultMaterial)) != nullptr);
	CHECK(assets.IsBuiltin(AssetHandle(AssetManager::BuiltinPlane)));
	CHECK(assets.GetAssetType(AssetHandle(AssetManager::BuiltinCube)) == AssetType::Mesh);
	// Same instance on repeated access; wrong type yields nullptr.
	CHECK(assets.GetAsset<MeshAsset>(AssetHandle(AssetManager::BuiltinCube)) == cube);
	CHECK(assets.GetAsset<MaterialAsset>(AssetHandle(AssetManager::BuiltinCube)) == nullptr);
}

TEST_CASE("AssetManager: importing registers files, persists the registry and loads on demand")
{
	const std::filesystem::path directory = MakeProject("AssetImport");
	AssetHandle texture, material;
	{
		AssetManager assets(directory);
		texture = assets.ImportAsset(directory / "Textures" / "Brick.png");
		material = assets.ImportAsset("Brick.pxmat");
		REQUIRE(texture.IsValid());
		REQUIRE(material.IsValid());
		CHECK(static_cast<uint64_t>(texture) >= AssetManager::FirstUserHandle);
		CHECK(assets.ImportAsset("Textures/Brick.png") == texture); // idempotent
		CHECK(assets.GetMetadata(texture)->FilePath == "Textures/Brick.png");
		CHECK(assets.GetHandleForPath("Textures/Brick.png") == texture);
		CHECK_FALSE(assets.IsLoaded(texture));
	}

	CHECK(std::filesystem::exists(directory / AssetManager::RegistryFileName));

	AssetManager reopened(directory);
	REQUIRE(reopened.IsRegistered(texture));
	CHECK(reopened.GetAssetType(material) == AssetType::Material);
	Ref<TextureAsset> loadedTexture = reopened.GetAsset<TextureAsset>(texture);
	REQUIRE(loadedTexture);
	CHECK(loadedTexture->Handle == texture);
	CHECK(loadedTexture->Mips[0][(1 * 4 + 1) * 4] == 1);
	Ref<MaterialAsset> loadedMaterial = reopened.GetAsset<MaterialAsset>(material);
	REQUIRE(loadedMaterial);
	CHECK(loadedMaterial->RoughnessFactor == 0.9f);
	CHECK(reopened.GetRegisteredAssets().size() == 2);
}

TEST_CASE("AssetManager: rejects missing, unsupported and out-of-project files")
{
	const std::filesystem::path directory = MakeProject("AssetReject");
	std::ofstream(directory / "notes.txt") << "hello";
	const std::filesystem::path outside = Paralax::Testing::CreateTestDirectory("AssetOutside") / "x.png";
	Image(2, 2).SavePNG(outside);

	Paralax::Testing::ScopedLogCapture capture;
	AssetManager assets(directory);
	CHECK_FALSE(assets.ImportAsset("missing.png").IsValid());
	CHECK_FALSE(assets.ImportAsset("notes.txt").IsValid());
	CHECK_FALSE(assets.ImportAsset(outside).IsValid());
	CHECK_FALSE(assets.ImportAsset("../AssetOutside/x.png").IsValid());
	CHECK(assets.GetRegisteredAssets().empty());
	CHECK(assets.GetAsset(AssetHandle(987654321)) == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "unsupported file type"));
	CHECK(capture.Contains(spdlog::level::err, "not inside the asset directory"));

	AssetManager noProject;
	CHECK_FALSE(noProject.ImportAsset(directory / "Brick.pxmat").IsValid());
}

TEST_CASE("AssetManager: memory assets, reloading and removal")
{
	const std::filesystem::path directory = MakeProject("AssetReload");
	AssetManager assets(directory);

	auto memory = CreateRef<MaterialAsset>();
	memory->Handle = AssetHandle(424242);
	assets.AddMemoryAsset(memory);
	CHECK(assets.GetAsset<MaterialAsset>(AssetHandle(424242)) == memory);
	{
		Paralax::Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(assets.AddMemoryAsset(memory), Paralax::Testing::AssertionFailedError);
		auto builtinClash = CreateRef<MaterialAsset>();
		builtinClash->Handle = AssetHandle(AssetManager::BuiltinCube);
		CHECK_THROWS_AS(assets.AddMemoryAsset(builtinClash), Paralax::Testing::AssertionFailedError);
	}

	const AssetHandle material = assets.ImportAsset("Brick.pxmat");
	Ref<MaterialAsset> first = assets.GetAsset<MaterialAsset>(material);
	REQUIRE(first);
	MaterialAsset changed = *first;
	changed.RoughnessFactor = 0.1f;
	REQUIRE(MaterialSerializer::SaveToFile(changed, directory / "Brick.pxmat"));
	REQUIRE(assets.ReloadAsset(material));
	Ref<MaterialAsset> second = assets.GetAsset<MaterialAsset>(material);
	CHECK(second->RoughnessFactor == 0.1f);
	CHECK(second->Version == first->Version + 1);

	CHECK(assets.RemoveAsset(material));
	CHECK_FALSE(assets.IsRegistered(material));
	CHECK_FALSE(assets.RemoveAsset(material));
}

TEST_CASE("MaterialSerializer: round-trips every field and validates documents")
{
	MaterialAsset material;
	material.BaseColorFactor = {0.5f, 0.6f, 0.7f, 0.8f};
	material.BaseColorTexture = AssetHandle(77);
	material.MetallicFactor = 1.0f;
	material.EmissiveFactor = {2.0f, 1.0f, 0.0f};
	material.HeightTexture = AssetHandle(78);
	material.HeightScale = 0.08f;
	material.Alpha = AlphaMode::Blend;
	material.DoubleSided = true;

	const nlohmann::json document = MaterialSerializer::Serialize(material);
	CHECK(document["AlphaMode"] == "Blend");
	MaterialAsset loaded;
	std::string error;
	REQUIRE(MaterialSerializer::Deserialize(loaded, document, error));
	CHECK(error.empty());
	CHECK(MaterialSerializer::Serialize(loaded) == document);

	CHECK_FALSE(MaterialSerializer::Deserialize(loaded, nlohmann::json::object(), error));
	nlohmann::json future = document;
	future["Paralax"]["Version"] = 99;
	CHECK_FALSE(MaterialSerializer::Deserialize(loaded, future, error));

	nlohmann::json partlyBad = document;
	partlyBad["Metallic"] = 5.0;
	partlyBad["Roughness"] = 0.33;
	partlyBad["Sparkle"] = true;
	REQUIRE(MaterialSerializer::Deserialize(loaded, partlyBad, error));
	CHECK(loaded.RoughnessFactor == 0.33f);
	CHECK(loaded.MetallicFactor == 1.0f);
	CHECK(error.find("Metallic") != std::string::npos);
	CHECK(error.find("Sparkle") != std::string::npos);
}

TEST_CASE("AssetManager: malformed and out-of-project registry entries are skipped safely")
{
	const std::filesystem::path directory = MakeProject("AssetRegistryHostile");
	Image(2, 2).SavePNG(directory.parent_path() / "outside.png");
	std::ofstream(directory / AssetManager::RegistryFileName) << R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[
		{"Handle":"5000","Type":5,"Path":"Brick.pxmat"},
		{"Handle":"5001","Type":"Texture","Path":"../outside.png"},
		{"Handle":"5002","Type":"Texture","Path":"/etc/passwd"},
		{"Handle":"5003","Type":"Material","Path":"Brick.pxmat"}]})";

	Paralax::Testing::ScopedLogCapture capture;
	AssetManager assets(directory);
	CHECK_FALSE(assets.IsRegistered(AssetHandle(5000)));
	CHECK_FALSE(assets.IsRegistered(AssetHandle(5001)));
	CHECK_FALSE(assets.IsRegistered(AssetHandle(5002)));
	CHECK(assets.IsRegistered(AssetHandle(5003)));
}

TEST_CASE("AssetManager: relative lookups are normalised")
{
	const std::filesystem::path directory = MakeProject("AssetNormalise");
	AssetManager assets(directory);
	const AssetHandle handle = assets.ImportAsset("Textures/Brick.png");
	CHECK(assets.GetHandleForPath("./Textures/Brick.png") == handle);
	CHECK(assets.GetHandleForPath("Textures/../Textures/Brick.png") == handle);
	CHECK(assets.GetHandleForPath(directory / "Textures" / "Brick.png") == handle);
}

TEST_CASE("MaterialSerializer: wrongly typed headers fail without throwing")
{
	MaterialAsset material;
	std::string error;
	CHECK_NOTHROW(CHECK_FALSE(MaterialSerializer::Deserialize(material, nlohmann::json{{"Paralax", {{"Type", "Material"}, {"Version", "x"}}}}, error)));
	CHECK_NOTHROW(CHECK_FALSE(MaterialSerializer::Deserialize(material, nlohmann::json{{"Paralax", {{"Type", 3}}}}, error)));
	CHECK_NOTHROW(CHECK_FALSE(MaterialSerializer::Deserialize(material, nlohmann::json{{"Paralax", "Material"}}, error)));
}
