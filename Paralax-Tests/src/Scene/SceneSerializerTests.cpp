#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>

using namespace Paralax;
using json = nlohmann::json;

namespace {

	// A scene using every reflected component and a nested hierarchy.
	void BuildSampleScene(Scene& scene)
	{
		Entity sun = scene.CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>().Intensity = 4.5f;
		sun.GetTransform().Rotation = glm::angleAxis(glm::radians(-45.0f), glm::vec3(1.0f, 0.0f, 0.0f));

		Entity player = scene.CreateEntity("Player");
		player.GetComponent<TagComponent>().Tag = "Player";
		player.GetTransform().Translation = {1.0f, 2.0f, 3.0f};
		player.GetTransform().Scale = {1.0f, 2.0f, 1.0f};
		player.AddComponent<MeshComponent>().Mesh = UUID(1234567890123ull);
		player.GetComponent<MeshComponent>().Materials = {UUID(1), UUID(0), UUID(3)};

		Entity camera = scene.CreateEntity("Camera");
		auto& cameraComponent = camera.AddComponent<CameraComponent>();
		cameraComponent.Camera.Projection = SceneCamera::ProjectionType::Orthographic;
		cameraComponent.Camera.OrthographicSize = 25.0f;
		scene.SetParent(camera, player);

		Entity lamp = scene.CreateEntity("Lamp");
		lamp.AddComponent<PointLightComponent>().Color = {1.0f, 0.5f, 0.25f};
		lamp.AddComponent<SpotLightComponent>().OuterConeAngle = glm::radians(40.0f);
		scene.SetParent(lamp, camera);

		Entity sky = scene.CreateEntity("Sky");
		sky.AddComponent<SkyLightComponent>().Environment = UUID(77);
		sky.AddComponent<PrefabInstanceComponent>().Prefab = UUID(88);
	}

}

TEST_CASE("SceneSerializer: serialize -> deserialize -> serialize is lossless")
{
	Scene original("Sample");
	BuildSampleScene(original);
	const json first = SceneSerializer::Serialize(original);

	CHECK(first["Paralax"]["Type"] == "Scene");
	CHECK(first["Paralax"]["Version"] == SceneSerializer::CurrentVersion);
	CHECK(first["Entities"].size() == 5);

	Scene loaded;
	std::string error;
	std::vector<std::string> warnings;
	REQUIRE(SceneSerializer::Deserialize(loaded, first, error, &warnings));
	CHECK(warnings.empty());
	CHECK(loaded.GetName() == "Sample");
	CHECK(SceneSerializer::Serialize(loaded) == first);

	Entity lamp = *loaded.FindEntityByName("Lamp");
	CHECK(loaded.GetParent(lamp)->GetName() == "Camera");
	CHECK(lamp.GetComponent<PointLightComponent>().Color == glm::vec3(1.0f, 0.5f, 0.25f));
}

TEST_CASE("SceneSerializer: files are saved atomically and loaded back")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("SceneFiles");
	const std::filesystem::path path = directory / "Level.pxscene";

	Scene original("Level");
	BuildSampleScene(original);
	REQUIRE(SceneSerializer::SaveToFile(original, path));
	CHECK_FALSE(std::filesystem::exists(directory / "Level.pxscene.tmp"));

	Ref<Scene> loaded = SceneSerializer::LoadFromFile(path);
	REQUIRE(loaded);
	CHECK(SceneSerializer::Serialize(*loaded) == SceneSerializer::Serialize(original));

	// Saving again overwrites in place.
	original.SetName("Renamed");
	REQUIRE(SceneSerializer::SaveToFile(original, path));
	CHECK(SceneSerializer::LoadFromFile(path)->GetName() == "Renamed");
}

TEST_CASE("SceneSerializer: unknown components and invalid fields produce warnings, not failures")
{
	json document = {
		{"Paralax", {{"Type", "Scene"}, {"Version", 1}}},
		{"Name", "Tolerant"},
		{"Entities",
		 {
			 {{"ID", "100"}, {"Parent", "0"}, {"Components", {{"Tag", {{"Name", "A"}}}, {"FutureComponent", {{"X", 1}}}, {"PointLight", {{"Intensity", "bright"}, {"Range", 5}}}}}},
			 {{"ID", "200"}, {"Parent", "999"}, {"Components", {{"Tag", {{"Name", "Orphan"}}}}}},
			 {{"Components", {}}},
		 }},
	};

	Scene scene;
	std::string error;
	std::vector<std::string> warnings;
	REQUIRE(SceneSerializer::Deserialize(scene, document, error, &warnings));
	CHECK(scene.GetEntityCount() == 2);
	CHECK(warnings.size() == 4);

	Entity a = scene.GetEntityByUUID(UUID(100));
	CHECK(a.GetName() == "A");
	CHECK(a.GetComponent<PointLightComponent>().Range == 5.0f);
	CHECK_FALSE(scene.GetParent(scene.GetEntityByUUID(UUID(200))).has_value());
}

TEST_CASE("SceneSerializer: structurally invalid documents are rejected")
{
	auto tryLoad = [](const json& document) {
		Scene scene;
		std::string error;
		const bool ok = SceneSerializer::Deserialize(scene, document, error);
		CHECK(scene.GetEntityCount() == 0);
		return ok ? std::string() : error;
	};

	CHECK(tryLoad(json::array()).find("JSON object") != std::string::npos);
	CHECK(tryLoad(json{{"Entities", json::array()}}).find("Not a Paralax scene") != std::string::npos);
	CHECK(tryLoad(json{{"Paralax", {{"Type", "Scene"}, {"Version", 99}}}, {"Entities", json::array()}}).find("Unsupported scene version") != std::string::npos);
	CHECK(tryLoad(json{{"Paralax", {{"Type", "Scene"}, {"Version", 1}}}}).find("Entities") != std::string::npos);
	CHECK(tryLoad(json{{"Paralax", {{"Type", "Scene"}, {"Version", 1}}}, {"Entities", {{{"ID", "5"}}, {{"ID", "5"}}}}}).find("Duplicate") != std::string::npos);

	Scene nonEmpty;
	nonEmpty.CreateEntity();
	std::string error;
	CHECK_FALSE(SceneSerializer::Deserialize(nonEmpty, json::object(), error));
}

TEST_CASE("SceneSerializer: LoadFromFile reports missing files and malformed JSON")
{
	Paralax::Testing::ScopedLogCapture capture;
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("BadScenes");
	CHECK(SceneSerializer::LoadFromFile(directory / "missing.pxscene") == nullptr);

	REQUIRE(WriteFileAtomically(directory / "broken.pxscene", "{ not json"));
	CHECK(SceneSerializer::LoadFromFile(directory / "broken.pxscene") == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "not valid JSON"));

	// A hostile field value nested 20,000 levels deep is refused before parsing: reporting it as a wrong type would
	// dump it, which recurses once per level and overflows the stack.
	const std::string deep = R"({"Paralax": {"Type": "Scene", "Version": 1}, "Entities": [{"ID": "1", "Parent": "0", "Components": {"Tag": {"Tag": )" +
							 Paralax::Testing::DeeplyNestedJson(20000) + "}}}]}";
	REQUIRE(WriteFileAtomically(directory / "deep.pxscene", deep));
	CHECK(SceneSerializer::LoadFromFile(directory / "deep.pxscene") == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "nests too deeply"));
}

TEST_CASE("SceneSerializer: oversized scene files are refused before reading")
{
	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("SceneLarge") / "Huge.pxscene";
	Paralax::Testing::CreateSparseFile(path, MaxTextFileBytes + 1);
	Paralax::Testing::ScopedLogCapture log;
	CHECK(SceneSerializer::LoadFromFile(path) == nullptr);
	CHECK(log.Contains(spdlog::level::err, "too large"));

	// Exactly the limit is still read (and then rejected as JSON, not for its size).
	Paralax::Testing::CreateSparseFile(path, MaxTextFileBytes);
	CHECK(SceneSerializer::LoadFromFile(path) == nullptr);
	CHECK(log.Contains(spdlog::level::err, "not valid JSON"));
	std::filesystem::remove(path);
}

TEST_CASE("SceneSerializer: wrongly typed headers are rejected without throwing")
{
	for (const json& header : {json{{"Type", 5}, {"Version", 1}}, json{{"Type", "Scene"}, {"Version", "1"}}, json{{"Type", nullptr}}})
	{
		Scene scene;
		std::string error;
		CHECK_NOTHROW(CHECK_FALSE(SceneSerializer::Deserialize(scene, json{{"Paralax", header}, {"Entities", json::array()}}, error)));
	}
}

TEST_CASE("SceneSerializer: cycles, self-parenting and missing parents become roots in document order")
{
	const json document = {
		{"Paralax", {{"Type", "Scene"}, {"Version", 1}}},
		{"Entities",
		 {
			 {{"ID", "1"}, {"Parent", "0"}},
			 {{"ID", "2"}, {"Parent", "999"}},
			 {{"ID", "3"}, {"Parent", "3"}},
			 {{"ID", "4"}, {"Parent", "5"}},
			 {{"ID", "5"}, {"Parent", "4"}},
			 {{"ID", "6"}, {"Parent", "1"}, {"Components", {{"Tag", {{"Name", ""}}}}}},
		 }},
	};
	Scene scene;
	std::string error;
	std::vector<std::string> warnings;
	REQUIRE(SceneSerializer::Deserialize(scene, document, error, &warnings));
	CHECK(warnings.size() >= 3);

	std::vector<uint64_t> roots;
	for (UUID id : scene.GetRootEntities())
		roots.push_back(id);
	// Exactly one of 4/5 keeps its parent (the cycle is broken at one point).
	CHECK(roots.size() == 4);
	CHECK(roots[0] == 1);
	CHECK(roots[1] == 2);
	CHECK(roots[2] == 3);
	CHECK(scene.GetParent(scene.GetEntityByUUID(UUID(6)))->GetUUID() == UUID(1));
	CHECK(scene.GetEntityByUUID(UUID(6)).GetName() == "Entity");
}

TEST_CASE("SceneSerializer: very deep hierarchies load and save without recursion or quadratic work")
{
	// No wall-clock comparison (flaky on loaded machines): a quadratic loader would need ~10^10 steps for this depth
	// and hit the test timeout, a recursive one would overflow the stack; the linear one takes seconds even unoptimized.
	constexpr uint64_t depth = 100000;
	json entities = json::array();
	for (uint64_t i = 1; i <= depth; i++)
		entities.push_back({{"ID", std::to_string(i)}, {"Parent", std::to_string(i - 1)}});
	const json document = {{"Paralax", {{"Type", "Scene"}, {"Version", 1}}}, {"Entities", entities}};

	Scene scene;
	std::string error;
	REQUIRE(SceneSerializer::Deserialize(scene, document, error));
	CHECK(scene.GetEntityCount() == depth);
	CHECK(scene.GetRootEntities() == std::vector<UUID>{UUID(1)});
	CHECK(scene.GetParent(scene.GetEntityByUUID(UUID(depth)))->GetUUID() == UUID(depth - 1));
	const json saved = SceneSerializer::Serialize(scene);
	REQUIRE(saved["Entities"].size() == depth);
	CHECK(saved["Entities"][depth - 1]["ID"] == std::to_string(depth));
}

TEST_CASE("SceneSerializer: names that are not valid UTF-8 save without throwing and the file loads")
{
	// Names come from scripts and imported files with any bytes; saving must neither throw nor write a file that
	// cannot be read back. The invalid bytes become U+FFFD.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("SceneInvalidUtf8");
	Scene scene(std::string("Level\xFF"));
	Entity crate = scene.CreateEntity(std::string("Crate\xFF"));
	crate.GetComponent<TagComponent>().Tag = std::string("\xE6\x97");
	const std::filesystem::path file = directory / "Level.pxscene";

	bool saved = false;
	CHECK_NOTHROW(saved = SceneSerializer::SaveToFile(scene, file));
	REQUIRE(saved);
	Ref<Scene> loaded = SceneSerializer::LoadFromFile(file);
	REQUIRE(loaded);
	CHECK(loaded->GetName() == "Level\xEF\xBF\xBD");
	REQUIRE(loaded->GetRootEntities().size() == 1);
	Entity loadedCrate = loaded->GetEntityByUUID(loaded->GetRootEntities().front());
	CHECK(loadedCrate.GetName() == "Crate\xEF\xBF\xBD");
	CHECK(loadedCrate.GetComponent<TagComponent>().Tag == "\xEF\xBF\xBD");
}
