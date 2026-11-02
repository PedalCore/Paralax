#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Prefab.h>
#include <Paralax/Scene/Scene.h>

#include <fstream>

using namespace Paralax;
using json = nlohmann::json;

namespace {

	// A small prefab: Tower (with a point light) > Top > Flag.
	Ref<PrefabAsset> MakeTower()
	{
		Scene source;
		Entity tower = source.CreateEntity("Tower");
		tower.AddComponent<PointLightComponent>().Intensity = 7.0f;
		tower.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
		Entity top = source.CreateEntity("Top");
		source.SetParent(top, tower);
		Entity flag = source.CreateEntity("Flag");
		source.SetParent(flag, top);
		source.CreateEntity("Unrelated");
		return PrefabAsset::CreateFromEntity(source, tower);
	}

}

TEST_CASE("Prefab: CreateFromEntity captures exactly the subtree")
{
	Ref<PrefabAsset> prefab = MakeTower();
	REQUIRE(prefab);
	CHECK(prefab->GetEntityCount() == 3);
	CHECK(prefab->GetRootName() == "Tower");
}

TEST_CASE("Prefab: instances get new IDs, keep hierarchy and components, and are marked")
{
	Ref<PrefabAsset> prefab = MakeTower();
	prefab->Handle = AssetHandle(7777);
	Scene scene;
	Entity a = prefab->Instantiate(scene);
	Entity b = prefab->Instantiate(scene, std::nullopt, glm::vec3(0.0f, 2.0f, 0.0f));
	CHECK(scene.GetEntityCount() == 6);
	CHECK(a.GetUUID() != b.GetUUID());
	CHECK(a.GetComponent<PointLightComponent>().Intensity == 7.0f);
	CHECK(a.GetComponent<TransformComponent>().Translation == glm::vec3(5.0f, 0.0f, 0.0f));
	CHECK(b.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 2.0f, 0.0f));
	CHECK(a.GetComponent<PrefabInstanceComponent>().Prefab == AssetHandle(7777));
	REQUIRE(a.GetComponent<RelationshipComponent>().Children.size() == 1);
	Entity top = scene.GetEntityByUUID(a.GetComponent<RelationshipComponent>().Children[0]);
	CHECK(top.GetName() == "Top");
	CHECK_FALSE(top.HasComponent<PrefabInstanceComponent>());
	CHECK(top.GetComponent<RelationshipComponent>().Children.size() == 1);
	CHECK(scene.GetRootEntities() == std::vector<UUID>{a.GetUUID(), b.GetUUID()});

	Entity holder = scene.CreateEntity("Holder");
	Entity child = prefab->Instantiate(scene, holder);
	CHECK(scene.GetParent(child) == holder);
}

TEST_CASE("Prefab: a prefab made from an instance does not keep the old instance marker")
{
	Ref<PrefabAsset> prefab = MakeTower();
	prefab->Handle = AssetHandle(1);
	Scene scene;
	Entity instance = prefab->Instantiate(scene);
	Ref<PrefabAsset> copy = PrefabAsset::CreateFromEntity(scene, instance);
	copy->Handle = AssetHandle(2);
	Scene other;
	CHECK(copy->Instantiate(other).GetComponent<PrefabInstanceComponent>().Prefab == AssetHandle(2));
}

TEST_CASE("Prefab: JSON and file round trips")
{
	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("PrefabFiles") / "Tower.pxprefab";
	REQUIRE(MakeTower()->SaveToFile(path));
	Ref<PrefabAsset> loaded = PrefabAsset::LoadFromFile(path);
	REQUIRE(loaded);
	CHECK(loaded->GetEntityCount() == 3);
	CHECK(loaded->ToJson()["Paralax"]["Type"] == "Prefab");
	Scene scene;
	CHECK(loaded->Instantiate(scene).GetComponent<PointLightComponent>().Intensity == 7.0f);
}

TEST_CASE("Prefab: hostile and malformed prefab files are rejected")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("PrefabHostile");
	Paralax::Testing::ScopedLogCapture log;
	auto load = [&](const std::string& name, const std::string& text) {
		std::ofstream(directory / name, std::ios::binary) << text;
		return PrefabAsset::LoadFromFile(directory / name);
	};
	const json valid = MakeTower()->ToJson();
	json scene = valid;
	scene["Paralax"]["Type"] = "Scene";
	json twoRoots = valid;
	twoRoots["Entities"].push_back({{"ID", "999"}, {"Parent", "0"}, {"Components", json::object()}});
	json noRoots = valid;
	noRoots["Entities"] = json::array();

	CHECK_FALSE(load("Empty.pxprefab", ""));
	CHECK_FALSE(load("Garbage.pxprefab", "{{{"));
	CHECK_FALSE(load("Scene.pxprefab", scene.dump()));
	CHECK_FALSE(load("TwoRoots.pxprefab", twoRoots.dump()));
	CHECK_FALSE(load("NoRoots.pxprefab", noRoots.dump()));
	CHECK_FALSE(PrefabAsset::LoadFromFile(directory / "Missing.pxprefab"));
	// A field nested 20,000 levels deep is refused before parsing (copying or dumping it would overflow the stack).
	CHECK_FALSE(load("Deep.pxprefab", R"({"Paralax": {"Type": "Prefab", "Version": 1}, "Entities": [{"ID": "1", "Parent": "0", "Components": {"Tag": {"Tag": )" +
										  Paralax::Testing::DeeplyNestedJson(20000) + "}}}]}"));
	CHECK(log.Contains(spdlog::level::err, "nests too deeply"));
	CHECK(load("Valid.pxprefab", valid.dump()));

	std::string error;
	CHECK_FALSE(PrefabAsset::FromJson(json::array(), error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("Prefab: oversized files are refused before reading")
{
	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("PrefabLarge") / "Huge.pxprefab";
	{
		std::ofstream file(path, std::ios::binary);
		file.seekp(static_cast<std::streamoff>(PrefabAsset::MaxFileBytes));
		file.put(' ');
	}
	Paralax::Testing::ScopedLogCapture log;
	CHECK_FALSE(PrefabAsset::LoadFromFile(path));
	CHECK(log.Contains(spdlog::level::err, "too large"));
}

TEST_CASE("Prefab: a prefab too large to load again is not written, and the existing file is kept")
{
	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("PrefabTooLargeToSave") / "Tower.pxprefab";
	Ref<PrefabAsset> small = MakeTower();
	REQUIRE(small->SaveToFile(path));
	const std::string before = Paralax::Testing::ReadFileText(path);

	// Values set in code are not capped, so one text this long stands in for a prefab with very many entities.
	Scene scene;
	Entity root = scene.CreateEntity("Huge");
	root.AddComponent<TextComponent>().Text = std::string(PrefabAsset::MaxFileBytes, 'x');
	Ref<PrefabAsset> huge = PrefabAsset::CreateFromEntity(scene, root);
	Paralax::Testing::ScopedLogCapture log;
	CHECK_FALSE(huge->SaveToFile(path));
	CHECK(log.Contains(spdlog::level::err, "too large"));
	CHECK(Paralax::Testing::ReadFileText(path) == before);
	Ref<PrefabAsset> reloaded = PrefabAsset::LoadFromFile(path);
	REQUIRE(reloaded);
	CHECK(reloaded->GetRootName() == "Tower");
}

TEST_CASE("Prefab: creating from an invalid entity fails verification")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	Scene scene;
	Entity entity = scene.CreateEntity();
	scene.DestroyEntity(entity);
	CHECK_THROWS_AS(PrefabAsset::CreateFromEntity(scene, entity), Paralax::Testing::AssertionFailedError);
}

TEST_CASE("Prefab: names that are not valid UTF-8 save without throwing and the file loads")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("PrefabInvalidUtf8");
	Scene source;
	Entity root = source.CreateEntity(std::string("Tower\xFF"));
	Ref<PrefabAsset> prefab = PrefabAsset::CreateFromEntity(source, root);
	bool saved = false;
	CHECK_NOTHROW(saved = prefab->SaveToFile(directory / "Tower.pxprefab"));
	REQUIRE(saved);
	Ref<PrefabAsset> loaded = PrefabAsset::LoadFromFile(directory / "Tower.pxprefab");
	REQUIRE(loaded);
	CHECK(loaded->GetRootName() == "Tower\xEF\xBF\xBD");
}
