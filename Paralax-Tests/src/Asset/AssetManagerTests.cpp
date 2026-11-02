#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MaterialSerializer.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/Image.h>
#include <Paralax/Core/EngineResources.h>

#include <algorithm>
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
		{"Handle":"5003","Type":"Material","Path":"Brick.pxmat"},
		{"Handle":"5004","Type":"Material","Path":"Brick.pxmat","Parent":"5004"},
		{"Handle":"5005","Type":"Material","Path":"Brick.pxmat","Parent":"5006"},
		{"Handle":"5006","Type":"Material","Path":"Brick.pxmat","Parent":"5005"},
		{"Handle":"5007","Type":"Material","Path":"Brick.pxmat","Parent":"5003"},
		{"Handle":"5008","Type":"Material","Path":"Brick.pxmat","Parent":"5007"},
		{"Handle":"5009","Type":"Material","Path":"Brick.pxmat","Parent":"4999"},
		{"Handle":"20","Type":"Material","Path":"Brick.pxmat"}]})";

	Paralax::Testing::ScopedLogCapture capture;
	AssetManager assets(directory);
	CHECK_FALSE(assets.IsRegistered(AssetHandle(5000)));
	CHECK_FALSE(assets.IsRegistered(AssetHandle(5001)));
	CHECK_FALSE(assets.IsRegistered(AssetHandle(5002)));
	CHECK(assets.IsRegistered(AssetHandle(5003)));
	// Parents must be registered file assets: a self-parent, a cycle, a chain and a missing parent are dropped, and
	// asking for them cannot recurse. A one-level sub-asset of a file that does not produce it fails cleanly.
	for (uint64_t handle : {5004u, 5005u, 5006u, 5008u, 5009u})
	{
		CAPTURE(handle);
		CHECK_FALSE(assets.IsRegistered(AssetHandle(handle)));
		CHECK(assets.GetAsset(AssetHandle(handle)) == nullptr);
	}
	CHECK(assets.IsRegistered(AssetHandle(5007)));
	CHECK(assets.GetAsset(AssetHandle(5007)) == nullptr);
	// The built-in range belongs to the built-ins, never to the registry.
	CHECK_FALSE(assets.IsRegistered(AssetHandle(20)));
	CHECK(assets.GetAssetType(AssetHandle(20)) == AssetType::None);

	// A registry nesting 20,000 levels deep is refused before parsing, so nothing from it is registered.
	const std::string deepRegistry = R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[{"Handle":"5003","Type":"Material","Path":"Brick.pxmat","Extra":)" +
									 Paralax::Testing::DeeplyNestedJson(20000) + "}]}";
	std::ofstream(directory / AssetManager::RegistryFileName) << deepRegistry;
	AssetManager deep(directory);
	CHECK_FALSE(deep.IsRegistered(AssetHandle(5003)));
	CHECK(capture.Contains(spdlog::level::err, "nests too deeply"));
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

TEST_CASE("MaterialSerializer: hostile and malformed material files are rejected")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MaterialHostile");
	Paralax::Testing::ScopedLogCapture log;
	auto load = [&](const char* name, const std::string& text) {
		std::ofstream(directory / name, std::ios::binary) << text;
		return MaterialSerializer::LoadFromFile(directory / name);
	};

	CHECK_FALSE(MaterialSerializer::LoadFromFile(directory / "Missing.pxmat"));
	CHECK_FALSE(load("Broken.pxmat", "{ not json"));
	CHECK(log.Contains(spdlog::level::err, "not valid JSON"));
	CHECK_FALSE(load("Scene.pxmat", R"({"Paralax": {"Type": "Scene", "Version": 1}})"));

	// Deserialize copies the document, which recurses once per level: a field nested 20,000 levels deep is refused
	// on the text, before parsing.
	CHECK_FALSE(load("Deep.pxmat", R"({"Paralax": {"Type": "Material", "Version": 1}, "Metallic": )" + Paralax::Testing::DeeplyNestedJson(20000) + "}"));
	CHECK(log.Contains(spdlog::level::err, "nests too deeply"));

	MaterialAsset material;
	material.MetallicFactor = 0.25f;
	REQUIRE(MaterialSerializer::SaveToFile(material, directory / "Valid.pxmat"));
	Ref<MaterialAsset> loaded = MaterialSerializer::LoadFromFile(directory / "Valid.pxmat");
	REQUIRE(loaded);
	CHECK(loaded->MetallicFactor == 0.25f);
}

TEST_CASE("AssetManager: the built-in catalog loads unique handles with matching types")
{
	AssetManager assets;
	std::vector<uint64_t> handles;
	for (const BuiltinAssetInfo& info : AssetManager::GetBuiltinAssets())
	{
		CAPTURE(info.Name);
		CHECK(info.Handle > 0);
		CHECK(info.Handle < AssetManager::FirstUserHandle);
		CHECK(std::find(handles.begin(), handles.end(), info.Handle) == handles.end());
		handles.push_back(info.Handle);
		CHECK(assets.IsBuiltin(AssetHandle(info.Handle)));
		CHECK(assets.GetAssetType(AssetHandle(info.Handle)) == info.Type);
		// File-backed built-ins are covered by the resource-specific tests below.
		if (info.Type == AssetType::EnvironmentMap)
			continue;
		auto asset = assets.GetAsset(AssetHandle(info.Handle));
		REQUIRE(asset);
		CHECK(asset->GetType() == info.Type);
		CHECK(static_cast<uint64_t>(asset->Handle) == info.Handle);
		CHECK(assets.GetAsset(asset->Handle) == asset);
	}
	for (uint64_t handle : {0u, 6u, 15u, 20u, 1023u})
	{
		CHECK_FALSE(assets.IsBuiltin(AssetHandle(handle)));
		CHECK(assets.GetAssetType(AssetHandle(handle)) == AssetType::None);
	}
}

TEST_CASE("AssetManager: ground material is distinct, cached and protected from replacement")
{
	AssetManager assets;
	const AssetHandle handle(AssetManager::BuiltinGroundMaterial);
	auto ground = assets.GetAsset<MaterialAsset>(handle);
	REQUIRE(ground);
	CHECK(ground->BaseColorFactor == glm::vec4(0.22f, 0.24f, 0.27f, 1.0f));
	CHECK(ground->RoughnessFactor == 0.4f);
	CHECK(ground->MetallicFactor == 0.0f);
	CHECK(assets.GetAsset<MaterialAsset>(handle) == ground);
	CHECK_FALSE(assets.GetAsset<MeshAsset>(handle));
	CHECK_FALSE(assets.RemoveAsset(handle));
	CHECK_FALSE(assets.ReloadAsset(handle));
	{
		Paralax::Testing::ScopedAssertionCapture capture;
		auto clash = CreateRef<MaterialAsset>();
		clash->Handle = handle;
		CHECK_THROWS_AS(assets.AddMemoryAsset(clash), Paralax::Testing::AssertionFailedError);
	}
	auto neutral = assets.GetAsset<MaterialAsset>(AssetHandle(AssetManager::BuiltinDefaultMaterial));
	REQUIRE(neutral);
	CHECK(neutral != ground);
	CHECK(neutral->BaseColorFactor.r == neutral->BaseColorFactor.g);
	CHECK(neutral->BaseColorFactor.g == neutral->BaseColorFactor.b);
}

TEST_CASE("AssetManager: unknown and broken assets are reported once, then retried after a re-import")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AssetFailures");
	AssetManager assets(directory);
	Paralax::Testing::ScopedLogCapture log;
	CHECK_FALSE(assets.GetAsset(AssetHandle(987654)));
	CHECK_FALSE(assets.GetAsset(AssetHandle(987654)));
	size_t reports = 0;
	for (const auto& entry : log.GetEntries())
		reports += entry.Message.find("Unknown asset 987654") != std::string::npos ? 1 : 0;
	CHECK(reports == 1);

	// A broken material is not re-read every time it is requested, but is once it has been re-imported.
	{
		std::ofstream(directory / "Broken.pxmat") << "{ not json";
	}
	const AssetHandle handle = assets.ImportAsset("Broken.pxmat");
	REQUIRE(handle.IsValid());
	CHECK_FALSE(assets.GetAsset(handle));
	{
		std::ofstream(directory / "Broken.pxmat") << R"({"Paralax": {"Type": "Material", "Version": 1}})";
	}
	CHECK_FALSE(assets.GetAsset(handle)); // Cached failure.
	CHECK(assets.ImportAsset("Broken.pxmat") == handle);
	CHECK(assets.GetAsset(handle));
}

TEST_CASE("AssetManager: packaged HDR environments load at reserved handles with full mip chains")
{
	AssetManager assets;
	for (uint64_t handle : {AssetManager::BuiltinSkyEnvironment, AssetManager::BuiltinStudioEnvironment})
	{
		CAPTURE(handle);
		auto texture = assets.GetAsset<TextureAsset>(AssetHandle(handle));
		REQUIRE(texture);
		CHECK(texture->GetType() == AssetType::EnvironmentMap);
		CHECK(assets.GetAssetType(AssetHandle(handle)) == AssetType::EnvironmentMap);
		CHECK(texture->Width == 2048);
		CHECK(texture->Height == 1024);
		CHECK(texture->Format == TextureFormat::RGBA32F);
		CHECK_FALSE(texture->SRGB);
		CHECK(texture->GetMipCount() == 12);
		CHECK(texture->Mips.back().size() == 16);
		CHECK(assets.GetAsset(AssetHandle(handle)) == texture);
		CHECK(assets.GetAssetType(AssetHandle(handle)) == AssetType::EnvironmentMap);
	}
}

TEST_CASE("AssetManager: missing built-in resources are reported once and retry after cache reset")
{
	const auto resourceDirectory = GetEngineResourceDirectory();
	const auto missing = Testing::CreateTestDirectory("MissingEnvironmentResources");
	Testing::ScopedEnvironmentVariable overrideDirectory("PARALAX_RESOURCE_DIR", missing.string());
	Testing::ScopedLogCapture log;
	AssetManager assets;
	const AssetHandle handle(AssetManager::BuiltinSkyEnvironment);
	for (int attempt = 0; attempt < 3; attempt++)
		CHECK_FALSE(assets.GetAsset(handle));
	CHECK_FALSE(assets.IsLoaded(handle));
	CHECK(assets.IsBuiltin(handle));
	CHECK(assets.GetAssetType(handle) == AssetType::EnvironmentMap);
	size_t errors = 0;
	for (const auto& entry : log.GetEntries())
		errors += entry.Level == spdlog::level::err ? 1 : 0;
	CHECK(errors == 1);
	{
		Testing::ScopedEnvironmentVariable restored("PARALAX_RESOURCE_DIR", resourceDirectory.string());
		CHECK_FALSE(assets.GetAsset(handle)); // A repaired path alone does not bypass the failure cache.
		REQUIRE(assets.LoadRegistry());
		REQUIRE(assets.GetAsset<TextureAsset>(handle));
		CHECK(assets.IsLoaded(handle));
	}
}

TEST_CASE("AssetManager: imported HDR retains its registered environment type")
{
	const auto directory = Testing::CreateTestDirectory("ImportedEnvironment");
	{
		std::ofstream file(directory / "sky.hdr", std::ios::binary);
		file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
		const char pixel[] = {64, 32, 16, static_cast<char>(130)};
		file.write(pixel, sizeof(pixel));
	}
	AssetManager assets(directory);
	const AssetHandle handle = assets.ImportAsset("sky.hdr");
	REQUIRE(handle.IsValid());
	auto texture = assets.GetAsset<TextureAsset>(handle);
	REQUIRE(texture);
	CHECK(texture->GetType() == AssetType::EnvironmentMap);
	CHECK(assets.GetAssetType(handle) == AssetType::EnvironmentMap);
	REQUIRE(assets.ReloadAsset(handle));
	CHECK(assets.GetAsset<TextureAsset>(handle)->GetType() == AssetType::EnvironmentMap);
}

TEST_CASE("AssetManager: a model's per-node meshes are registered, and follow re-exports that drop objects")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AssetManagerNodeMeshes");
	// Two named meshes, each on its own node.
	nlohmann::json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["meshes"].push_back(document["meshes"][0]);
	document["meshes"][0]["name"] = "Keep";
	document["meshes"][1]["name"] = "Drop";
	document["nodes"] = {{{"name", "Kept"}, {"mesh", 0}}, {{"name", "Dropped"}, {"mesh", 1}}};
	document["scenes"][0]["nodes"] = {0, 1};
	Paralax::Testing::WriteGltf(directory, document, "Model.gltf");

	AssetManager assets(directory);
	const AssetHandle model = assets.ImportAsset("Model.gltf");
	REQUIRE(model.IsValid());
	MeshImportResult imported;
	std::string error;
	REQUIRE(MeshImporter::Import(directory / "Model.gltf", model, imported, error));
	REQUIRE(imported.NodeMeshes.size() == 2);
	const AssetHandle kept = imported.Nodes[0].Mesh;
	const AssetHandle dropped = imported.Nodes[1].Mesh;
	for (AssetHandle handle : {kept, dropped})
	{
		REQUIRE(assets.IsRegistered(handle));
		CHECK(assets.GetMetadata(handle)->Parent == model);
		CHECK(assets.GetMetadata(handle)->Type == AssetType::Mesh);
		CHECK(assets.GetAsset<MeshAsset>(handle));
	}

	// Re-exported without the second object: reloading unregisters its mesh and keeps the other's handle.
	document["meshes"].erase(1);
	document["nodes"].erase(1);
	document["scenes"][0]["nodes"] = {0};
	Paralax::Testing::WriteGltf(directory, document, "Model.gltf");
	REQUIRE(assets.ReloadAsset(model));
	CHECK(assets.IsRegistered(kept));
	CHECK(assets.GetAsset<MeshAsset>(kept));
	CHECK_FALSE(assets.IsRegistered(dropped));
	Paralax::Testing::ScopedLogCapture capture;
	CHECK_FALSE(assets.GetAsset<MeshAsset>(dropped));
}
