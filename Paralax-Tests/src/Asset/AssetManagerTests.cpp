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
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <algorithm>
#include <fstream>
#include <limits>

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

TEST_CASE("AssetManager: non-ASCII file names round-trip through import, the registry, reload and lookup")
{
	// "Brick-é-日本.png" and "Niveau-é-日本.pxscene", as UTF-8 bytes. On Windows the narrow conversions of
	// std::filesystem::path would use the ANSI code page here (and throw for 日本), so this pins the UTF-8 ones.
	const std::string textureName = "Textures/Brick-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.png";
	const std::string sceneName = "Scenes/Niveau-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.pxscene";
	const std::string entityName = "Ziegel-\xC3\xA9";
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AssetNonAscii");
	Image image(4, 4);
	image.SetPixel(1, 1, {1, 2, 3, 255});
	REQUIRE(image.SavePNG(directory / FromUtf8(textureName)));
	Scene scene("Niveau");
	scene.CreateEntity(entityName);
	REQUIRE(SceneSerializer::SaveToFile(scene, directory / FromUtf8(sceneName)));

	AssetHandle texture, level;
	{
		Paralax::Testing::ScopedLogCapture log;
		AssetManager assets(directory);
		texture = assets.ImportAsset(directory / FromUtf8(textureName));
		level = assets.ImportAsset(FromUtf8(sceneName));
		REQUIRE(texture.IsValid());
		REQUIRE(level.IsValid());
		CHECK(ToUtf8(assets.GetMetadata(texture)->FilePath) == textureName);
		CHECK(assets.GetHandleForPath(FromUtf8(textureName)) == texture);
		CHECK(assets.ImportAsset(FromUtf8(textureName)) == texture);
		// Log messages carry the name as UTF-8 too.
		CHECK(log.Contains(spdlog::level::info, "'" + textureName + "'"));
	}

	// The registry stores the names as UTF-8, unescaped, so it reads the same on every platform.
	const std::filesystem::path registryFile = directory / AssetManager::RegistryFileName;
	const std::string registry = Paralax::Testing::ReadFileText(registryFile);
	CHECK(registry.find("\"Path\": \"" + textureName + "\"") != std::string::npos);
	CHECK(registry.find("\"Path\": \"" + sceneName + "\"") != std::string::npos);

	AssetManager reopened(directory);
	CHECK(reopened.GetHandleForPath(FromUtf8(textureName)) == texture);
	CHECK(reopened.GetHandleForPath(directory / FromUtf8(sceneName)) == level);
	REQUIRE(reopened.GetMetadata(level));
	CHECK(ToUtf8(reopened.GetMetadata(level)->FilePath) == sceneName);
	Ref<TextureAsset> loadedTexture = reopened.GetAsset<TextureAsset>(texture);
	REQUIRE(loadedTexture);
	CHECK(loadedTexture->Mips[0][(1 * 4 + 1) * 4] == 1);
	CHECK(reopened.ReloadAsset(texture));

	Ref<Scene> loadedScene = SceneSerializer::LoadFromFile(reopened.ToAbsolutePath(reopened.GetMetadata(level)->FilePath));
	REQUIRE(loadedScene);
	CHECK(loadedScene->GetName() == "Niveau");
	REQUIRE(loadedScene->GetRootEntities().size() == 1);
	CHECK(loadedScene->GetEntityByUUID(loadedScene->GetRootEntities().front()).GetName() == entityName);

	// Saving again writes the registry unchanged.
	REQUIRE(reopened.SaveRegistry());
	CHECK(Paralax::Testing::ReadFileText(registryFile) == registry);
}

TEST_CASE("AssetManager: invalid UTF-8 anywhere in the registry makes it malformed, refused without throwing")
{
	// JSON text must be UTF-8, so the parser rejects the whole file before any path is converted.
	const std::filesystem::path directory = MakeProject("AssetRegistryInvalidUtf8");
	std::ofstream(directory / AssetManager::RegistryFileName, std::ios::binary)
		<< R"({"Paralax": {"Type": "AssetRegistry", "Version": 1}, "Assets": [{"Handle": "123456", "Type": "Texture", "Path": "Textures/Brick)"
		<< "\xFF"
		<< R"(.png"}]})";
	Paralax::Testing::ScopedLogCapture log;
	std::optional<AssetManager> assets;
	CHECK_NOTHROW(assets.emplace(directory));
	REQUIRE(assets);
	CHECK(assets->GetRegisteredAssets().empty());
	CHECK(log.Contains(spdlog::level::err, "malformed"));
}

TEST_CASE("AssetManager: a registry path with JSON-escaped non-ASCII characters names the UTF-8 file")
{
	// "画像" is "画像": the escapes decode to UTF-8 text, which becomes the path through FromUtf8.
	const std::string name = "Textures/\xE7\x94\xBB\xE5\x83\x8F.png";
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AssetRegistryEscaped");
	REQUIRE(Image(2, 2).SavePNG(directory / FromUtf8(name)));
	std::ofstream(directory / AssetManager::RegistryFileName, std::ios::binary)
		<< R"({"Paralax": {"Type": "AssetRegistry", "Version": 1}, "Assets": [{"Handle": "123456", "Type": "Texture", "Path": "Textures/画像.png"}]})";

	AssetManager assets(directory);
	const AssetHandle handle(123456);
	REQUIRE(assets.GetMetadata(handle));
	CHECK(ToUtf8(assets.GetMetadata(handle)->FilePath) == name);
	CHECK(assets.GetHandleForPath(FromUtf8(name)) == handle);
	CHECK(assets.GetAsset<TextureAsset>(handle) != nullptr);
	// Saved back as UTF-8, not escaped.
	REQUIRE(assets.SaveRegistry());
	CHECK(Paralax::Testing::ReadFileText(directory / AssetManager::RegistryFileName).find("\"Path\": \"" + name + "\"") != std::string::npos);
}

TEST_CASE("AssetManager: a file name that is not UTF-8 is refused at import, and the registry is unchanged")
{
	if constexpr (sizeof(std::filesystem::path::value_type) != 1)
	{
		// Windows names are UTF-16, so the only names without a UTF-8 form hold unpaired surrogates, which the same
		// check refuses but which this test cannot create portably.
		MESSAGE("Only POSIX file names can be bytes that are not UTF-8; nothing to check here");
	}
	else
	{
		// The registry stores UTF-8, so the name would be written with U+FFFD and point at no file after a reload.
		const std::filesystem::path directory = MakeProject("AssetNotUtf8");
		AssetManager assets(directory);
		REQUIRE(assets.ImportAsset("Brick.pxmat").IsValid());
		const std::filesystem::path registryFile = directory / AssetManager::RegistryFileName;
		const std::string registry = Paralax::Testing::ReadFileText(registryFile);
		const std::string name = "Textures/Brick\xFF.png";
		// Some file systems refuse such names outright (APFS, on macOS, requires UTF-8). The name is refused before the
		// file is looked at, so everything below holds either way; where the file can exist, it is there to show that
		// a real file under such a name is refused too.
		if (!Image(2, 2).SavePNG(directory / FromUtf8(name)))
			MESSAGE("This file system refuses names that are not UTF-8; the name is checked without a file behind it");

		Paralax::Testing::ScopedLogCapture log;
		AssetHandle handle = 0;
		CHECK_NOTHROW(handle = assets.ImportAsset(directory / FromUtf8(name)));
		CHECK_FALSE(handle.IsValid());
		CHECK_FALSE(assets.ImportAsset(FromUtf8(name)).IsValid());
		// The message shows U+FFFD where the name is not UTF-8.
		CHECK(log.Contains(spdlog::level::err, "'Textures/Brick\xEF\xBF\xBD.png' is not a valid UTF-8 file name; rename it"));

		std::string reason;
		CHECK_FALSE(assets.ToRegistryPath(FromUtf8(name), &reason));
		CHECK(reason.find("UTF-8") != std::string::npos);
		reason.clear();
		CHECK_FALSE(assets.ToRegistryPath("../Outside.png", &reason));
		CHECK(reason.empty()); // Outside the directory is not a naming problem.

		CHECK(assets.GetRegisteredAssets().size() == 1);
		CHECK(Paralax::Testing::ReadFileText(registryFile) == registry);
	}
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

TEST_CASE("AssetManager: a path the registry lists under several handles resolves to the lowest, in any order")
{
	const std::filesystem::path directory = MakeProject("AssetRegistryDuplicatePaths");
	const std::filesystem::path registry = directory / AssetManager::RegistryFileName;
	// Three entries for one file ("./Brick.pxmat" names it too), in every order: which is listed first, and the order
	// of the hash map they go into, must not matter. A sub-asset of the file is not a second entry for it.
	const std::string entries[] = {
		R"({"Handle":"9000","Type":"Material","Path":"Brick.pxmat"})",
		R"({"Handle":"5000","Type":"Material","Path":"Brick.pxmat"})",
		R"({"Handle":"7000","Type":"Material","Path":"./Brick.pxmat"})",
		R"({"Handle":"6000","Type":"Texture","Path":"Textures/Brick.png"})",
	};
	const std::string subAsset = R"({"Handle":"8000","Type":"Material","Path":"Brick.pxmat","Parent":"9000"})";
	std::vector<size_t> order = {0, 1, 2, 3};
	do
	{
		std::string listed;
		for (size_t index : order)
			listed += entries[index] + ",";
		REQUIRE(WriteFileAtomically(registry, R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[)" + listed + subAsset + "]}"));
		CAPTURE(listed);
		Paralax::Testing::ScopedLogCapture log;
		AssetManager assets(directory);
		CHECK(assets.GetHandleForPath("Brick.pxmat") == AssetHandle(5000));
		CHECK(assets.GetHandleForPath(directory / "Brick.pxmat") == AssetHandle(5000));
		CHECK(assets.ImportAsset("Brick.pxmat") == AssetHandle(5000));
		CHECK(assets.GetHandleForPath("Textures/Brick.png") == AssetHandle(6000));
		// Every entry stays registered, since scenes may use any of the handles, and each loads the file.
		for (uint64_t handle : {5000u, 7000u, 9000u})
		{
			CAPTURE(handle);
			CHECK(assets.GetAsset<MaterialAsset>(AssetHandle(handle)) != nullptr);
		}
		CHECK(assets.IsRegistered(AssetHandle(8000)));
		// Reported once, for the one path listed more than once.
		CHECK(log.Count(spdlog::level::warn, "lists 'Brick.pxmat' 3 times; looking it up by path gives the lowest handle, 5000, not 7000, 9000") == 1);
		CHECK(log.Count(spdlog::level::warn, " times; looking it up by path") == 1);
	} while (std::next_permutation(order.begin(), order.end()));

	// Saved back as they were: nothing is dropped.
	{
		AssetManager assets(directory);
		REQUIRE(assets.SaveRegistry());
	}
	AssetManager reopened(directory);
	CHECK(reopened.GetRegisteredAssets().size() == 5);
	CHECK(reopened.GetHandleForPath("Brick.pxmat") == AssetHandle(5000));

	// However many entries name the file, the warning lists a few and counts the rest.
	std::string many;
	for (int handle = 5007; handle >= 5001; handle--)
		many += fmt::format(R"({}{{"Handle":"{}","Type":"Material","Path":"Brick.pxmat"}})", handle == 5007 ? "" : ",", handle);
	REQUIRE(WriteFileAtomically(registry, R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[)" + many + "]}"));
	Paralax::Testing::ScopedLogCapture log;
	AssetManager crowded(directory);
	CHECK(crowded.GetHandleForPath("Brick.pxmat") == AssetHandle(5001));
	CHECK(log.Count(spdlog::level::warn, "lists 'Brick.pxmat' 7 times; looking it up by path gives the lowest handle, 5001, not 5002, 5003, 5004, 5005 and 2 more") == 1);
}

TEST_CASE("AssetManager: an oversized registry is refused before reading, and registers nothing")
{
	// A valid registry, padded with whitespace past the cap: read in full, it would register its entry.
	const std::filesystem::path directory = MakeProject("AssetRegistryLarge");
	const std::string valid = R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[{"Handle":"5003","Type":"Material","Path":"Brick.pxmat"}]})";
	REQUIRE(WriteFileAtomically(directory / AssetManager::RegistryFileName, valid));
	REQUIRE(AssetManager(directory).IsRegistered(AssetHandle(5003)));
	// A hostile file, so written past the limit the engine's own writers keep to.
	REQUIRE(WriteFileAtomically(directory / AssetManager::RegistryFileName, valid + std::string(AssetManager::MaxRegistryBytes + 1 - valid.size(), ' '), std::numeric_limits<size_t>::max()));
	Paralax::Testing::ScopedLogCapture log;
	AssetManager assets(directory);
	CHECK(log.Contains(spdlog::level::err, "too large"));
	CHECK_FALSE(assets.IsRegistered(AssetHandle(5003)));
	std::filesystem::remove(directory / AssetManager::RegistryFileName);
}

TEST_CASE("AssetManager: a registry that cannot be read is never overwritten, and imports fail until it loads")
{
	// A merge left conflict markers in the file: it lists the project's assets, but cannot be parsed. Saving the
	// empty in-memory registry over it (as the next import would) would lose every handle the scenes use.
	const std::filesystem::path directory = MakeProject("AssetRegistryUnreadable");
	const std::filesystem::path registry = directory / AssetManager::RegistryFileName;
	const std::string entry = R"({"Handle":"5003","Type":"Material","Path":"Brick.pxmat"})";
	const std::string conflicted = "<<<<<<< HEAD\n" + std::string(R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[)") + entry + "]}\n=======\n" +
								   std::string(R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[]})") + "\n>>>>>>> other\n";
	REQUIRE(WriteFileAtomically(registry, conflicted));
	Paralax::Testing::ScopedLogCapture log;
	AssetManager assets(directory);
	CHECK(log.Contains(spdlog::level::err, "is malformed"));
	CHECK(assets.GetRegisteredAssets().empty());

	CHECK_FALSE(assets.ImportAsset("Textures/Brick.png").IsValid());
	CHECK(log.Contains(spdlog::level::err, "could not be read"));
	CHECK(assets.GetRegisteredAssets().empty());
	CHECK_FALSE(assets.SaveRegistry());
	CHECK(Paralax::Testing::ReadFileText(registry) == conflicted);

	// Too large to read (here at the seam's small limit) is protected the same way. The valid file is padded well past
	// the registry the import would save, and the limit lies between the two sizes, so the save is not refused for its
	// size: only the guard can stop it.
	{
		const std::string padded = R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[)" + entry + "]}" + std::string(4096, ' ');
		REQUIRE(WriteFileAtomically(registry, padded));
		AssetManager limited(directory, 1024);
		Paralax::Testing::ScopedLogCapture guard;
		CHECK_FALSE(limited.ImportAsset("Textures/Brick.png").IsValid());
		CHECK(guard.Contains(spdlog::level::err, "could not be read"));
		CHECK_FALSE(guard.Contains(spdlog::level::err, "the document is too large"));
		CHECK(Paralax::Testing::ReadFileText(registry) == padded);
		REQUIRE(WriteFileAtomically(registry, conflicted));
	}

	// Repaired, and loaded again: the old entry is back, and imports are recorded beside it.
	REQUIRE(WriteFileAtomically(registry, R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[)" + entry + "]}"));
	CHECK(assets.LoadRegistry());
	CHECK(assets.IsRegistered(AssetHandle(5003)));
	const AssetHandle texture = assets.ImportAsset("Textures/Brick.png");
	REQUIRE(texture.IsValid());
	AssetManager reopened(directory);
	CHECK(reopened.IsRegistered(AssetHandle(5003)));
	CHECK(reopened.IsRegistered(texture));

	// Deleting an unreadable registry gives up what it listed: saves then create a new one, and keep it up to date
	// (the file is then one the manager wrote, so later saves may replace it).
	REQUIRE(WriteFileAtomically(registry, conflicted));
	AssetManager discarded(directory);
	std::filesystem::remove(registry);
	const AssetHandle material = discarded.ImportAsset("Brick.pxmat");
	CHECK(material.IsValid());
	CHECK(std::filesystem::is_regular_file(registry));
	const AssetHandle brickTexture = discarded.ImportAsset("Textures/Brick.png");
	CHECK(brickTexture.IsValid());
	AssetManager recreated(directory);
	CHECK(recreated.IsRegistered(material));
	CHECK(recreated.IsRegistered(brickTexture));
}

TEST_CASE("AssetManager: a registry changed on disk since it was loaded or saved is not saved over until it is reloaded")
{
	// A version-control checkout or merge rewrites the file under a running editor. Saving the registry in memory
	// over it would drop the entries it brought and bring back those it removed.
	const std::filesystem::path directory = MakeProject("AssetRegistryChangedOnDisk");
	const std::filesystem::path registry = directory / AssetManager::RegistryFileName;
	AssetManager assets(directory);
	const AssetHandle material = assets.ImportAsset("Brick.pxmat");
	REQUIRE(material.IsValid());
	const std::string checkedOut = R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[{"Handle":"5003","Type":"Material","Path":"Brick.pxmat"},)"
								   R"({"Handle":"5004","Type":"Texture","Path":"Textures/Brick.png"}]})";
	REQUIRE(WriteFileAtomically(registry, checkedOut));

	{
		Paralax::Testing::ScopedLogCapture log;
		CHECK_FALSE(assets.SaveRegistry());
		CHECK(log.Contains(spdlog::level::err, "'" + ToUtf8(assets.GetAssetDirectory() / AssetManager::RegistryFileName) + "' has changed on disk since it was loaded or last saved"));
		CHECK(log.Contains(spdlog::level::err, "asset.reload_registry"));
		// Whatever would save fails, and is undone: the import, the removal.
		CHECK_FALSE(assets.ImportAsset("Textures/Brick.png").IsValid());
		CHECK_FALSE(assets.GetHandleForPath("Textures/Brick.png").IsValid());
		CHECK_FALSE(assets.RemoveAsset(material));
		CHECK(assets.IsRegistered(material));
		CHECK(Paralax::Testing::ReadFileText(registry) == checkedOut);
	}

	// Reloaded, the registry is the checkout's, and saves go through again.
	REQUIRE(assets.LoadRegistry());
	CHECK_FALSE(assets.IsRegistered(material));
	CHECK(assets.GetHandleForPath("Textures/Brick.png") == AssetHandle(5004));
	CHECK(assets.SaveRegistry());

	// A rewrite of the same size differs in its bytes: the hash tells it apart.
	const std::string saved = Paralax::Testing::ReadFileText(registry);
	std::string sameSize = saved;
	REQUIRE(sameSize.find("\"5004\"") != std::string::npos);
	sameSize.replace(sameSize.find("\"5004\""), 6, "\"5005\"");
	REQUIRE(sameSize.size() == saved.size());
	REQUIRE(WriteFileAtomically(registry, sameSize));
	{
		Paralax::Testing::ScopedLogCapture log;
		CHECK_FALSE(assets.RemoveAsset(AssetHandle(5003)));
		CHECK(log.Contains(spdlog::level::err, "has changed on disk"));
		CHECK(Paralax::Testing::ReadFileText(registry) == sameSize);
	}
	REQUIRE(assets.LoadRegistry());
	CHECK(assets.RemoveAsset(AssetHandle(5003)));
	{
		AssetManager reopened(directory);
		CHECK_FALSE(reopened.IsRegistered(AssetHandle(5003)));
		CHECK(reopened.IsRegistered(AssetHandle(5005)));
	}

	// A file that appears where there was none is someone else's too; one removed since loses nothing when saved.
	std::filesystem::remove(registry);
	AssetManager fresh(directory);
	REQUIRE(WriteFileAtomically(registry, checkedOut));
	{
		Paralax::Testing::ScopedLogCapture log;
		CHECK_FALSE(fresh.ImportAsset("Brick.pxmat").IsValid());
		CHECK(log.Contains(spdlog::level::err, "has changed on disk"));
		CHECK(Paralax::Testing::ReadFileText(registry) == checkedOut);
	}
	std::filesystem::remove(registry);
	const AssetHandle recreated = fresh.ImportAsset("Brick.pxmat");
	CHECK(recreated.IsValid());
	CHECK(AssetManager(directory).IsRegistered(recreated));

	// Two managers on one project: what the first saves is not lost to the second, which must reload first.
	AssetManager first(directory);
	AssetManager second(directory);
	const AssetHandle texture = first.ImportAsset("Textures/Brick.png");
	REQUIRE(texture.IsValid());
	{
		Paralax::Testing::ScopedLogCapture log;
		CHECK_FALSE(second.RemoveAsset(recreated));
	}
	REQUIRE(second.LoadRegistry());
	CHECK(second.GetHandleForPath("Textures/Brick.png") == texture);
	CHECK(second.RemoveAsset(recreated));
	AssetManager last(directory);
	CHECK(last.IsRegistered(texture));
	CHECK_FALSE(last.IsRegistered(recreated));
}

TEST_CASE("AssetManager: one registry byte limit governs loading and saving, and an import past it is undone")
{
	// At a small limit (the constructor's seam), the cases a 64 MiB registry would take minutes to reach.
	const std::filesystem::path directory = MakeProject("AssetRegistryLimit");
	const std::filesystem::path registry = directory / AssetManager::RegistryFileName;
	{
		AssetManager assets(directory);
		REQUIRE(assets.ImportAsset("Brick.pxmat").IsValid());
	}
	const std::string before = Paralax::Testing::ReadFileText(registry);
	REQUIRE_FALSE(before.empty());

	// Just large enough for what is registered: it loads and saves, but nothing more fits.
	Paralax::Testing::WriteGltf(directory, Paralax::Testing::MakeTriangleDocument(directory), "Model.gltf");
	AssetManager assets(directory, before.size());
	REQUIRE(assets.GetHandleForPath("Brick.pxmat").IsValid());
	CHECK(assets.SaveRegistry());
	const size_t registered = assets.GetRegisteredAssets().size();
	Paralax::Testing::ScopedLogCapture log;
	// A model brings sub-assets (material, texture, node meshes) with it; the import and all of them are undone.
	CHECK_FALSE(assets.ImportAsset("Model.gltf").IsValid());
	CHECK(log.Contains(spdlog::level::err, "too large"));
	CHECK(log.Contains(spdlog::level::err, "registry could not be saved"));
	CHECK(assets.GetRegisteredAssets().size() == registered);
	CHECK_FALSE(assets.GetHandleForPath("Model.gltf").IsValid());
	for (const AssetMetadata& metadata : assets.GetRegisteredAssets())
		CHECK(metadata.FilePath != std::filesystem::path("Model.gltf"));
	CHECK(Paralax::Testing::ReadFileText(registry) == before);

	// One byte less, and the same registry no longer loads.
	AssetManager smaller(directory, before.size() - 1);
	CHECK_FALSE(smaller.GetHandleForPath("Brick.pxmat").IsValid());
	CHECK(Paralax::Testing::ReadFileText(registry) == before);
}

TEST_CASE("MaterialSerializer: oversized material files are refused before reading")
{
	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("MaterialLarge") / "Huge.pxmat";
	Paralax::Testing::CreateSparseFile(path, MaxTextFileBytes + 1);
	Paralax::Testing::ScopedLogCapture log;
	CHECK_FALSE(MaterialSerializer::LoadFromFile(path));
	CHECK(log.Contains(spdlog::level::err, "too large"));
	std::filesystem::remove(path);
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

TEST_CASE("MaterialSerializer: a Version that would wrap into range, a negative one and a float are refused as written")
{
	// 4294967297 and -4294967295 read as 1 through get<int>(), which keeps only the low 32 bits.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MaterialHostileVersions");
	Paralax::Testing::ScopedLogCapture log;
	for (const char* version : {"4294967297", "-4294967295", "-1", "1.5"})
	{
		const std::string shownVersion = version; // doctest shows a const char* as its address.
		CAPTURE(shownVersion);
		const std::string text = fmt::format(R"({{"Paralax": {{"Type": "Material", "Version": {}}}, "Roughness": 0.25}})", version);
		const std::string expected = fmt::format("Unsupported material version: expected 1-{}, got {} (number)", MaterialSerializer::CurrentVersion, version);
		MaterialAsset material;
		std::string error;
		CHECK_NOTHROW(CHECK_FALSE(MaterialSerializer::Deserialize(material, nlohmann::json::parse(text), error)));
		CHECK(error == expected);
		CHECK(material.RoughnessFactor == MaterialAsset().RoughnessFactor);
		REQUIRE(WriteFileAtomically(directory / "Hostile.pxmat", text));
		CHECK_FALSE(MaterialSerializer::LoadFromFile(directory / "Hostile.pxmat"));
		CHECK(log.Contains(spdlog::level::err, expected));
	}
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
	Testing::ScopedEnvironmentVariable overrideDirectory("PARALAX_RESOURCE_DIR", ToUtf8(missing));
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
		Testing::ScopedEnvironmentVariable restored("PARALAX_RESOURCE_DIR", ToUtf8(resourceDirectory));
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

TEST_CASE("AssetManager: an import whose registry cannot be saved is undone")
{
	const std::filesystem::path directory = MakeProject("AssetRegistryUnwritable");
	// A directory where the registry file belongs: nothing can be written or renamed over it, on any platform.
	std::filesystem::create_directories(directory / AssetManager::RegistryFileName);
	Paralax::Testing::ScopedLogCapture capture;
	AssetManager assets(directory);

	CHECK_FALSE(assets.ImportAsset("Brick.pxmat").IsValid());
	CHECK_FALSE(assets.ImportAsset("Textures/Brick.png").IsValid());
	// A model registers its sub-assets (material, texture, node meshes) during the import; they go back out too.
	Paralax::Testing::WriteGltf(directory, Paralax::Testing::MakeTriangleDocument(directory), "Model.gltf");
	CHECK_FALSE(assets.ImportAsset("Model.gltf").IsValid());
	CHECK(assets.GetRegisteredAssets().empty());
	CHECK_FALSE(assets.GetHandleForPath("Brick.pxmat").IsValid());
	CHECK_FALSE(assets.GetHandleForPath("Model.gltf").IsValid());
	CHECK(capture.Contains(spdlog::level::err, "registry could not be saved"));

	// Writable again: the same files import.
	std::filesystem::remove_all(directory / AssetManager::RegistryFileName);
	CHECK(assets.ImportAsset("Brick.pxmat").IsValid());
	CHECK(std::filesystem::is_regular_file(directory / AssetManager::RegistryFileName));
}

TEST_CASE("AssetManager: removing and reloading fail without losing the registry when it cannot be saved")
{
	const std::filesystem::path directory = MakeProject("AssetRegistryLocked");
	nlohmann::json document = Paralax::Testing::MakeTriangleDocument(directory);
	document["nodes"] = {{{"name", "Node"}, {"mesh", 0}}};
	document["scenes"][0]["nodes"] = {0};
	Paralax::Testing::WriteGltf(directory, document, "Model.gltf");
	AssetManager assets(directory);
	const AssetHandle material = assets.ImportAsset("Brick.pxmat");
	const AssetHandle model = assets.ImportAsset("Model.gltf");
	REQUIRE(material.IsValid());
	REQUIRE(model.IsValid());
	const size_t registered = assets.GetRegisteredAssets().size();
	REQUIRE(registered >= 3); // The material, the model and its node mesh at least.
	// The loaded model and one of its sub-assets, which a refused removal must put back as they were.
	const Ref<MeshAsset> mesh = assets.GetAsset<MeshAsset>(model);
	REQUIRE(mesh);
	AssetHandle subAsset = 0;
	for (const AssetMetadata& metadata : assets.GetRegisteredAssets())
	{
		if (metadata.Parent == model)
			subAsset = metadata.Handle;
	}
	REQUIRE(subAsset.IsValid());
	const Ref<Asset> loadedSubAsset = assets.GetAsset(subAsset);
	REQUIRE(loadedSubAsset);

	// Lock the registry by putting a directory in its place.
	std::filesystem::remove(directory / AssetManager::RegistryFileName);
	std::filesystem::create_directories(directory / AssetManager::RegistryFileName);
	Paralax::Testing::ScopedLogCapture capture;
	CHECK_FALSE(assets.RemoveAsset(material));
	CHECK(assets.IsRegistered(material));
	CHECK_FALSE(assets.RemoveAsset(model));
	CHECK(assets.IsRegistered(model));
	CHECK(assets.GetRegisteredAssets().size() == registered);
	CHECK(assets.IsLoaded(model));
	CHECK(assets.GetAsset<MeshAsset>(model) == mesh);
	const AssetMetadata* subMetadata = assets.GetMetadata(subAsset);
	REQUIRE(subMetadata);
	CHECK(subMetadata->Parent == model);
	CHECK(assets.IsLoaded(subAsset));
	CHECK(assets.GetAsset(subAsset) == loadedSubAsset);
	// Reloads that change nothing in the registry need no save; a material's never does.
	CHECK(assets.ReloadAsset(model));
	CHECK(assets.ReloadAsset(material));
	CHECK(capture.Contains(spdlog::level::err, "registry could not be saved"));

	// Re-exported with another object: the new mesh is registered in memory, but cannot be recorded.
	document["meshes"].push_back(document["meshes"][0]);
	document["meshes"][1]["name"] = "Second";
	document["nodes"].push_back({{"name", "Other"}, {"mesh", 1}});
	document["scenes"][0]["nodes"] = {0, 1};
	Paralax::Testing::WriteGltf(directory, document, "Model.gltf");
	CHECK_FALSE(assets.ReloadAsset(model));
	CHECK(assets.GetRegisteredAssets().size() == registered + 1);
	CHECK(capture.Contains(spdlog::level::err, "recorded only in memory"));

	// Unlocked: removal goes through, and is on disk.
	std::filesystem::remove_all(directory / AssetManager::RegistryFileName);
	CHECK(assets.RemoveAsset(material));
	CHECK_FALSE(assets.IsRegistered(material));
	AssetManager reopened(directory);
	CHECK_FALSE(reopened.IsRegistered(material));
	CHECK(reopened.IsRegistered(model));
}

TEST_CASE("AssetManager: a model under a non-ASCII name reloads, and a registry it cannot save is reported by that name")
{
	// "Modelle-é-日本/Model.gltf": the reload compares and reports registry paths, which must not go through the
	// platform's narrow encoding (on Windows that would throw for 日本).
	const std::string folder = "Modelle-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC";
	const std::string modelName = folder + "/Model.gltf";
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AssetReloadNonAscii");
	const std::filesystem::path modelDirectory = directory / FromUtf8(folder);
	nlohmann::json document = Paralax::Testing::MakeTriangleDocument(modelDirectory);
	document["nodes"] = {{{"name", "Node"}, {"mesh", 0}}};
	document["scenes"][0]["nodes"] = {0};
	Paralax::Testing::WriteGltf(modelDirectory, document, "Model.gltf");
	AssetManager assets(directory);
	const AssetHandle model = assets.ImportAsset(FromUtf8(modelName));
	REQUIRE(model.IsValid());
	CHECK(ToUtf8(assets.GetMetadata(model)->FilePath) == modelName);
	const std::filesystem::path registryFile = directory / AssetManager::RegistryFileName;
	const std::string registry = Paralax::Testing::ReadFileText(registryFile);

	// Unchanged: it reloads and the registry is left as it was.
	CHECK(assets.ReloadAsset(model));
	CHECK(Paralax::Testing::ReadFileText(registryFile) == registry);

	// Re-exported with another object while the registry cannot be written: the error names the model in UTF-8.
	std::filesystem::remove(registryFile);
	std::filesystem::create_directories(registryFile);
	document["meshes"].push_back(document["meshes"][0]);
	document["meshes"][1]["name"] = "Second";
	document["nodes"].push_back({{"name", "Other"}, {"mesh", 1}});
	document["scenes"][0]["nodes"] = {0, 1};
	Paralax::Testing::WriteGltf(modelDirectory, document, "Model.gltf");
	Paralax::Testing::ScopedLogCapture capture;
	CHECK_FALSE(assets.ReloadAsset(model));
	CHECK(capture.Contains(spdlog::level::err, "Reloaded '" + modelName + "', but the asset registry could not be saved"));
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
