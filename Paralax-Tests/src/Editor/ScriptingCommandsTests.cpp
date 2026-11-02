#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Core/Input.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scripting/ScriptAsset.h>

#include <algorithm>
#include <fstream>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	struct ProjectFixture
	{
		explicit ProjectFixture(const std::string& name)
			: Directory(Paralax::Testing::CreateTestDirectory(name) / "Game")
			, Context(Project::Create(Directory, "Game"))
		{
			REQUIRE(Context.GetProject());
		}

		json Run(const std::string& command, const json& params = json::object())
		{
			CommandResult result = Context.Execute(command, params);
			REQUIRE_MESSAGE(result.Success, command, ": ", result.Error);
			return result.Result;
		}

		std::filesystem::path Directory;
		EditorContext Context;
	};

}

TEST_CASE("ScriptingCommands: script.create writes a runnable template or the given source")
{
	ProjectFixture fixture("ScriptCreate");
	const json created = fixture.Run("script.create", {{"path", "Scripts/player-controller"}});
	CHECK(created["path"] == "Scripts/player-controller.lua");
	const std::string text = Paralax::Testing::ReadFileText(fixture.Directory / "Assets" / "Scripts" / "player-controller.lua");
	CHECK(text.find("local PlayerController = {}") != std::string::npos);
	CHECK(text.find("return PlayerController") != std::string::npos);

	// The template runs: attach it, play, and check that no script errors were recorded.
	const std::string id = fixture.Run("entity.create", {{"name", "Player"}, {"components", {{"Script", {{"Script", created["handle"]}}}}}})["id"];
	fixture.Run("play.start");
	fixture.Run("play.step", {{"frames", 2}});
	CHECK(fixture.Run("script.errors")["errors"].empty());
	fixture.Run("play.stop");

	CHECK_FALSE(fixture.Context.Execute("script.create", {{"path", "Scripts/player-controller.lua"}}).Success); // Exists.
	CHECK_FALSE(fixture.Context.Execute("script.create", {{"path", "../Outside"}}).Success);
	CHECK_FALSE(EditorContext().Execute("script.create", {{"path", "Scripts/X"}}).Success); // No project.
	CHECK(fixture.Run("script.create", {{"path", "Scripts/Custom.lua"}, {"source", "return {}"}})["path"] == "Scripts/Custom.lua");
}

TEST_CASE("ScriptingCommands: a script or prefab whose import fails leaves no file behind, so it can be created again")
{
	ProjectFixture fixture("ScriptingCommandsRegistryLocked");
	const std::filesystem::path assets = fixture.Context.GetAssets().GetAssetDirectory();
	const std::filesystem::path registry = assets / AssetManager::RegistryFileName;
	const std::string crate = fixture.Run("entity.create", {{"name", "Crate"}})["id"].get<std::string>();

	// A directory where the registry file belongs: the registry cannot be saved, so imports fail.
	std::error_code error;
	std::filesystem::remove(registry, error);
	std::filesystem::create_directories(registry);
	Paralax::Testing::ScopedLogCapture capture;
	CHECK_FALSE(fixture.Context.Execute("script.create", {{"path", "Scripts/Mover.lua"}}).Success);
	CHECK_FALSE(std::filesystem::exists(assets / "Scripts" / "Mover.lua"));
	CHECK_FALSE(fixture.Context.Execute("prefab.create", {{"id", crate}, {"path", "Prefabs/Crate"}}).Success);
	CHECK_FALSE(std::filesystem::exists(assets / "Prefabs" / "Crate.pxprefab"));

	// Writable again: the same commands go through.
	std::filesystem::remove_all(registry);
	fixture.Run("script.create", {{"path", "Scripts/Mover.lua"}});
	fixture.Run("prefab.create", {{"id", crate}, {"path", "Prefabs/Crate"}});
	CHECK(std::filesystem::is_regular_file(assets / "Scripts" / "Mover.lua"));
	CHECK(std::filesystem::is_regular_file(assets / "Prefabs" / "Crate.pxprefab"));
}

TEST_CASE("ScriptingCommands: scripts run in play mode and script.eval reaches the running scene")
{
	ProjectFixture fixture("ScriptPlay");
	const std::string handle = fixture.Run("script.create", {{"path", "Scripts/Spinner.lua"}, {"source", R"(
		local Spinner = {}
		function Spinner:OnUpdate(dt)
			local t = self.entity:GetComponent("Transform")
			t.Translation = t.Translation + vec3(dt, 0, 0)
		end
		return Spinner
	)"}})["handle"];
	const std::string id = fixture.Run("entity.create", {{"name", "Mover"}, {"components", {{"Script", {{"Script", handle}}}}}})["id"];

	CHECK_FALSE(fixture.Context.Execute("script.eval", {{"code", "1"}}).Success); // Not playing.
	fixture.Run("play.start");
	fixture.Run("play.step", {{"frames", 30}, {"dt", 0.1}});
	CHECK(fixture.Run("script.eval", {{"code", "math.abs(Scene.FindEntityByName('Mover'):GetComponent('Transform').Translation.x - 3) < 1e-4"}})["result"] == "true");
	CHECK_FALSE(fixture.Context.Execute("script.eval", {{"code", "error('nope')"}}).Success);
	fixture.Run("play.stop");
	// Play mode ran on a copy: the editor scene is untouched.
	CHECK(fixture.Run("entity.get", {{"id", id}})["components"]["Transform"]["Translation"] == json::array({0.0, 0.0, 0.0}));
}

TEST_CASE("ScriptingCommands: script.errors reports failing scripts and can be cleared")
{
	ProjectFixture fixture("ScriptErrors");
	const std::string handle = fixture.Run("script.create", {{"path", "Scripts/Broken.lua"}, {"source", "local S = {} function S:OnCreate() error('kaput') end return S"}})["handle"];
	fixture.Run("entity.create", {{"components", {{"Script", {{"Script", handle}}}}}});
	CHECK(fixture.Run("script.errors") == json{{"errors", json::array()}, {"playing", false}});
	fixture.Run("play.start");
	fixture.Run("play.step");
	const json errors = fixture.Run("script.errors", {{"clear", true}})["errors"];
	REQUIRE(errors.size() == 1);
	CHECK(errors[0].get<std::string>().find("kaput") != std::string::npos);
	CHECK(fixture.Run("script.errors")["errors"].empty());
	fixture.Run("play.stop");
}

TEST_CASE("ScriptingCommands: prefabs are created from entities and instantiated undoably")
{
	ProjectFixture fixture("PrefabCommands");
	const std::string root = fixture.Run("entity.create", {{"name", "Tree"}, {"components", {{"Mesh", {{"Mesh", "4"}}}}}})["id"];
	fixture.Run("entity.create", {{"name", "Leaves"}, {"parent", root}, {"components", {{"Mesh", {{"Mesh", "2"}}}}}});
	const json prefab = fixture.Run("prefab.create", {{"id", root}, {"path", "Prefabs/Tree"}});
	CHECK(prefab["entities"] == 2);
	CHECK(std::filesystem::exists(fixture.Directory / "Assets" / "Prefabs" / "Tree.pxprefab"));
	CHECK_FALSE(fixture.Context.Execute("prefab.create", {{"id", root}, {"path", "Prefabs/Tree"}}).Success); // Exists.
	CHECK_FALSE(fixture.Context.Execute("prefab.create", {{"id", "1"}, {"path", "Prefabs/Other"}}).Success); // No entity.

	const size_t before = fixture.Context.GetEditorScene().GetEntityCount();
	const std::string byHandle = fixture.Run("prefab.instantiate", {{"prefab", prefab["handle"]}, {"position", {3, 0, 0}}})["id"];
	const std::string byPath = fixture.Run("prefab.instantiate", {{"prefab", "Prefabs/Tree.pxprefab"}, {"parent", root}})["id"];
	CHECK(fixture.Context.GetEditorScene().GetEntityCount() == before + 4);
	CHECK(fixture.Run("entity.get", {{"id", byHandle}})["components"]["Transform"]["Translation"] == json::array({3.0, 0.0, 0.0}));
	CHECK(fixture.Run("entity.get", {{"id", byPath}})["parent"] == root);
	CHECK(fixture.Run("entity.get", {{"id", byHandle}})["components"]["PrefabInstance"]["Prefab"] == prefab["handle"]);

	REQUIRE(fixture.Context.Undo());
	REQUIRE(fixture.Context.Undo());
	CHECK(fixture.Context.GetEditorScene().GetEntityCount() == before);

	CHECK_FALSE(fixture.Context.Execute("prefab.instantiate", {{"prefab", "Prefabs/Missing.pxprefab"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("prefab.instantiate", {{"prefab", prefab["handle"]}, {"position", {1, 2}}}).Success);
	CHECK_FALSE(fixture.Context.Execute("prefab.instantiate", {{"prefab", prefab["handle"]}, {"parent", "12345"}}).Success);
	CHECK(fixture.Context.GetEditorScene().GetEntityCount() == before);
}

TEST_CASE("ScriptingCommands: model.create_prefab turns a glTF scene into entities")
{
	ProjectFixture fixture("ModelPrefabCommand");
	const std::filesystem::path models = fixture.Directory / "Assets" / "Models";
	std::filesystem::create_directories(models);
	json document = Paralax::Testing::MakeTriangleDocument(models);
	Paralax::Testing::AddCamerasAndLights(document);
	Paralax::Testing::WriteGltf(models, document, "Bay.gltf");

	// The model is imported on the way, by its path.
	const json created = fixture.Run("model.create_prefab", {{"model", "Models/Bay.gltf"}});
	CHECK(created["entities"] == 8);
	CHECK(created["meshes"] == 2);
	CHECK(created["cameras"] == 2);
	CHECK(created["lights"] == 3);
	CHECK(created["warnings"].empty());
	CHECK(std::filesystem::exists(fixture.Directory / "Assets" / "Prefabs" / "Bay.pxprefab"));

	// Instantiated, it shows the model piece by piece.
	const std::string root = fixture.Run("prefab.instantiate", {{"prefab", created["handle"]}})["id"];
	const json entity = fixture.Run("entity.get", {{"id", root}});
	CHECK(entity["name"] == "Bay");
	Scene& scene = fixture.Context.GetActiveScene();
	std::optional<Entity> lamp = scene.FindEntityByName("Lamp");
	REQUIRE(lamp);
	CHECK(lamp->HasComponent<PointLightComponent>());
	std::optional<Entity> node1 = scene.FindEntityByName("Node1");
	REQUIRE(node1);
	const AssetHandle nodeMesh = node1->GetComponent<MeshComponent>().Mesh;
	CHECK(fixture.Context.GetAssets().GetAsset<MeshAsset>(nodeMesh));

	// The per-node meshes are in the registry: a fresh asset manager (the project reopened) finds them.
	{
		AssetManager reopened(fixture.Directory / "Assets");
		REQUIRE(reopened.LoadRegistry());
		Ref<MeshAsset> mesh = reopened.GetAsset<MeshAsset>(nodeMesh);
		REQUIRE(mesh);
		CHECK(mesh->Submeshes.size() == 1);
	}

	// By handle, with a chosen path and root name; existing destinations and non-models are refused.
	const json again = fixture.Run("model.create_prefab", {{"model", created["model"]}, {"path", "Prefabs/BayCopy"}, {"name", "Copy"}});
	CHECK(again["entities"] == 8);
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", "Models/Bay.gltf"}}).Success); // Prefabs/Bay.pxprefab exists.
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", "Models/Missing.glb"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", "../Outside.gltf"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", created["handle"]}}).Success); // A prefab, not a model.
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", "1"}}).Success);               // The built-in cube.
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", 5}}).Success);
	CHECK_FALSE(EditorContext().Execute("model.create_prefab", {{"model", "Models/Bay.gltf"}}).Success); // No project.
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", "Models/Bay.gltf"}, {"path", 3}}).Success);
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", "Models/Bay.gltf"}, {"name", false}}).Success);

	// Refused while playing: reloading the model would change assets under the running game.
	fixture.Run("play.start", {{"paused", true}});
	CHECK_FALSE(fixture.Context.Execute("model.create_prefab", {{"model", "Models/Bay.gltf"}, {"path", "Prefabs/WhilePlaying"}}).Success);
	fixture.Run("play.stop");
	CHECK_FALSE(std::filesystem::exists(fixture.Directory / "Assets" / "Prefabs" / "WhilePlaying.pxprefab"));
}

TEST_CASE("ScriptingCommands: model.create_prefab brings animations that play in play mode")
{
	ProjectFixture fixture("ModelPrefabAnimations");
	const std::filesystem::path models = fixture.Directory / "Assets" / "Models";
	std::filesystem::create_directories(models);
	json document = Paralax::Testing::MakeTriangleDocument(models);
	Paralax::Testing::AddCamerasAndLights(document);
	Paralax::Testing::AddAnimations(document);
	Paralax::Testing::WriteGltf(models, document, "Rig.gltf");

	const json created = fixture.Run("model.create_prefab", {{"model", "Models/Rig.gltf"}});
	CHECK(created["animations"] == 3);
	CHECK(created["warnings"].empty());
	fixture.Run("prefab.instantiate", {{"prefab", created["handle"]}});

	// LampBob lifts the lamp from y 3 to 4 over the first second, relative to the sun it hangs from.
	fixture.Run("play.start", {{"paused", true}});
	fixture.Run("play.step", {{"frames", 60}, {"dt", 1.0 / 60.0}});
	const std::string y = fixture.Run("script.eval", {{"code", "return Scene.FindEntityByName('Lamp'):GetComponent('Transform').Translation.y"}})["result"];
	CHECK(std::stod(y) == doctest::Approx(4.0).epsilon(1e-3));
	// Scripts and agents control playback through the component's fields.
	fixture.Run("script.eval", {{"code", "local a = Scene.FindEntityByName('Lamp'):GetComponent('Animation') a.Playing = false a.Time = 0.5"}});
	fixture.Run("play.step", {{"frames", 30}});
	const std::string paused = fixture.Run("script.eval", {{"code", "return Scene.FindEntityByName('Lamp'):GetComponent('Transform').Translation.y"}})["result"];
	CHECK(std::stod(paused) == doctest::Approx(3.5).epsilon(1e-3));
	CHECK(fixture.Run("script.errors")["errors"].empty());
	fixture.Run("play.stop");
}

TEST_CASE("ScriptingCommands: model.create_prefab registers per-node meshes for models imported before them")
{
	ProjectFixture fixture("ModelPrefabOldRegistry");
	const std::filesystem::path models = fixture.Directory / "Assets" / "Models";
	std::filesystem::create_directories(models);
	json document = Paralax::Testing::MakeTriangleDocument(models);
	Paralax::Testing::WriteGltf(models, document, "Old.GLTF"); // Extensions match in any case.
	const std::string model = fixture.Run("asset.import", {{"path", "Models/Old.GLTF"}})["handle"];

	// The registry as an older engine wrote it: the model's materials and textures, but no per-node meshes.
	const std::filesystem::path registryFile = fixture.Directory / "Assets" / AssetManager::RegistryFileName;
	json registry = json::parse(Paralax::Testing::ReadFileText(registryFile));
	json& entries = registry["Assets"];
	const size_t before = entries.size();
	entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const json& entry) { return entry.value("Parent", "") == model && entry["Type"] == "Mesh"; }), entries.end());
	REQUIRE(entries.size() < before);
	std::ofstream(registryFile) << registry.dump();

	EditorContext reopened(Project::Load(fixture.Directory / "Game.pxproj"));
	REQUIRE(reopened.GetProject());
	const CommandResult created = reopened.Execute("model.create_prefab", {{"model", model}});
	REQUIRE_MESSAGE(created.Success, created.Error);
	CHECK(created.Result["meshes"] == 2);
	REQUIRE(reopened.Execute("prefab.instantiate", {{"prefab", created.Result["handle"]}}).Success);
	std::optional<Entity> node = reopened.GetActiveScene().FindEntityByName("Node1");
	REQUIRE(node);
	CHECK(reopened.GetAssets().IsRegistered(node->GetComponent<MeshComponent>().Mesh));
	CHECK(reopened.GetAssets().GetAsset<MeshAsset>(node->GetComponent<MeshComponent>().Mesh));
}

TEST_CASE("ScriptingCommands: script.api lists the scripting API with or without play mode")
{
	EditorContext context;
	const CommandResult stopped = context.Execute("script.api");
	REQUIRE(stopped.Success);
	CHECK(stopped.Result["api"].size() > 60);
	REQUIRE(context.Execute("play.start").Success);
	const CommandResult playing = context.Execute("script.api");
	REQUIRE(playing.Success);
	CHECK(playing.Result["api"] == stopped.Result["api"]);
	context.Execute("play.stop");
}

TEST_CASE("ScriptingCommands: script.get and script.update read and replace a script's source")
{
	ProjectFixture fixture("ScriptUpdate");
	const json created = fixture.Run("script.create", {{"path", "Scripts/Counter.lua"}, {"source", "return { value = 1 }"}});
	CHECK(fixture.Run("script.get", {{"path", "Scripts/Counter.lua"}})["source"] == "return { value = 1 }");
	CHECK(fixture.Run("script.get", {{"path", created["handle"]}})["path"] == "Scripts/Counter.lua"); // By handle.
	CHECK(fixture.Run("script.get", {{"path", "Scripts/Counter"}})["handle"] == created["handle"]);   // Extension optional.

	fixture.Run("script.update", {{"path", "Scripts/Counter.lua"}, {"source", "return { value = 2 }"}});
	CHECK(Paralax::Testing::ReadFileText(fixture.Directory / "Assets" / "Scripts" / "Counter.lua") == "return { value = 2 }");
	CHECK(fixture.Run("script.get", {{"path", "Scripts/Counter.lua"}})["source"] == "return { value = 2 }");

	CHECK_FALSE(fixture.Context.Execute("script.get", {{"path", "Scripts/Missing.lua"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("script.update", {{"path", "Scripts/Missing.lua"}, {"source", "return {}"}}).Success); // Never creates.
	CHECK_FALSE(fixture.Context.Execute("script.update", {{"path", "../Outside.lua"}, {"source", "return {}"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("script.update", {{"path", "Scripts/Counter.lua"}, {"source", std::string(ScriptAsset::MaxSourceBytes + 1, ' ')}}).Success);
	CHECK_FALSE(fixture.Context.Execute("script.get", {{"path", "2"}}).Success);                      // A built-in mesh, not a script.
	CHECK_FALSE(EditorContext().Execute("script.get", {{"path", "Scripts/Counter.lua"}}).Success);    // No project.
	CHECK_FALSE(fixture.Context.Execute("script.update", {{"path", "Scripts/Counter.lua"}}).Success); // No source.

	// While playing, the game keeps the code it started with; the update applies when play stops.
	const AssetHandle counter(std::stoull(created["handle"].get<std::string>()));
	fixture.Run("entity.create", {{"name", "Holder"}, {"components", {{"Script", {{"Script", created["handle"]}}}}}});
	fixture.Run("play.start", {{"paused", true}});
	CHECK(fixture.Run("script.update", {{"path", "Scripts/Counter.lua"}, {"source", "return { value = 3 }"}})["appliesNow"] == false);
	CHECK(fixture.Run("script.get", {{"path", "Scripts/Counter.lua"}})["source"] == "return { value = 3 }"); // The file.
	CHECK(fixture.Context.GetAssets().GetAsset<ScriptAsset>(counter)->Source == "return { value = 2 }");
	fixture.Run("play.stop");
	CHECK(fixture.Context.GetAssets().GetAsset<ScriptAsset>(counter)->Source == "return { value = 3 }");
	// A registered script whose file is gone is never recreated.
	std::filesystem::remove(fixture.Directory / "Assets" / "Scripts" / "Counter.lua");
	CHECK(fixture.Context.Execute("script.update", {{"path", "Scripts/Counter.lua"}, {"source", "return {}"}}).Error.find("missing") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(fixture.Directory / "Assets" / "Scripts" / "Counter.lua"));
}

TEST_CASE("ScriptingCommands: injected input reaches the game one step at a time")
{
	ProjectFixture fixture("InputInjection");
	const json script = fixture.Run("script.create", {{"path", "Scripts/Recorder.lua"}, {"source", R"(
		local R = {}
		Pressed, Released, Held, Clicks, Scroll = 0, 0, 0, 0, 0
		function R:OnUpdate()
			if Input.WasKeyPressed(Key.Space) then Pressed = Pressed + 1 end
			if Input.WasKeyReleased(Key.Space) then Released = Released + 1 end
			if Input.IsKeyDown(Key.Space) then Held = Held + 1 end
			if Input.WasMouseButtonPressed(Mouse.Left) then Clicks = Clicks + 1 end
			Scroll = Scroll + Input.GetMouseScrollDelta().y
			Cursor = Input.GetMousePosition()
		end
		return R
	)"}});
	fixture.Run("entity.create", {{"name", "Recorder"}, {"components", {{"Script", {{"Script", script["handle"]}}}}}});
	fixture.Run("play.start", {{"paused", true}});

	fixture.Run("input.key", {{"key", "Space"}, {"action", "press"}});
	fixture.Run("input.mouse_button", {{"button", "Left"}, {"action", "press"}});
	fixture.Run("input.mouse_move", {{"x", 120.5}, {"y", 64}});
	CHECK(fixture.Run("input.scroll", {{"x", 0}, {"y", 2}})["queued"] == 4);
	// Queued until the game's next update, so nothing is lost however many editor frames pass between requests.
	Input::EndFrame();
	CHECK(fixture.Run("input.state")["keys"].empty());
	fixture.Run("play.step", {{"frames", 3}});
	CHECK(fixture.Run("input.state")["keys"] == json::array({"Space"}));
	CHECK(fixture.Run("input.state")["buttons"] == json::array({"Left"}));
	CHECK(fixture.Run("input.state")["queued"] == 0);
	// Pressed (and scrolled) in the first step only; held in all three.
	CHECK(fixture.Run("script.eval", {{"code", "Pressed, Held, Clicks, Scroll, Cursor.x, Cursor.y"}})["result"] == "1\t3\t1\t2.0\t120.5\t64.0");

	fixture.Run("input.key", {{"key", "Space"}, {"action", "release"}});
	fixture.Run("play.step", {{"frames", 2}});
	CHECK(fixture.Run("script.eval", {{"code", "Released, Held"}})["result"] == "1\t3");

	// release_all releases at the next update, which sees the releases; the cursor stays where it was.
	fixture.Run("input.key", {{"key", "Space"}, {"action", "press"}});
	fixture.Run("play.step");
	fixture.Run("input.release_all");
	fixture.Run("play.step");
	CHECK(fixture.Run("script.eval", {{"code", "Released, Cursor.x"}})["result"] == "2\t120.5");
	CHECK(fixture.Run("input.state")["buttons"].empty());

	// Stopping play releases anything still held, so it cannot steer the editor afterwards; injection then fails.
	fixture.Run("input.key", {{"key", "Left"}, {"action", "press"}});
	fixture.Run("play.step");
	fixture.Run("play.stop");
	CHECK(fixture.Run("input.state")["keys"].empty());
	CHECK_FALSE(fixture.Context.Execute("input.key", {{"key", "A"}, {"action", "press"}}).Success);
	CHECK(fixture.Run("input.release_all")["queued"] == 0);

	fixture.Run("play.start", {{"paused", true}});
	CHECK(fixture.Context.Execute("input.key", {{"key", "Spacebar"}, {"action", "press"}}).Error.find("Space") != std::string::npos); // Lists names.
	CHECK_FALSE(fixture.Context.Execute("input.key", {{"key", "Space"}, {"action", "hold"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("input.mouse_button", {{"button", "Fourth"}, {"action", "press"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("input.mouse_move", {{"x", "far"}, {"y", 0}}).Success);
	CHECK_FALSE(fixture.Context.Execute("input.mouse_move", {{"x", 1e300}, {"y", 0}}).Success); // Beyond a float.
	CHECK_FALSE(fixture.Context.Execute("input.scroll", {{"x", 0}, {"y", -1e7}}).Success);
	CHECK(fixture.Run("input.state")["queued"] == 0);
}

TEST_CASE("ScriptingCommands: scripts switch scenes in play mode, and stopping returns to the edited scene")
{
	ProjectFixture fixture("PlaySceneSwitch");
	fixture.Run("entity.create", {{"name", "LevelTwoMarker"}});
	fixture.Run("scene.save", {{"path", "Scenes/LevelTwo"}});
	fixture.Run("scene.new", {{"name", "LevelOne"}});
	fixture.Run("entity.create", {{"name", "LevelOneMarker"}});

	fixture.Run("play.start", {{"paused", true}});
	const std::string marker = fixture.Run("scene.find", {{"name", "LevelOneMarker"}})["ids"][0];
	fixture.Run("selection.set", {{"ids", {marker}}});
	fixture.Run("script.eval", {{"code", "Game.Set('Coins', 7) Scene.Load('Scenes/LevelTwo.pxscene')"}});
	fixture.Run("play.step");
	CHECK(fixture.Run("selection.get")["ids"].empty()); // Its entities are gone with the old scene.
	CHECK(fixture.Run("scene.find", {{"name", "LevelTwoMarker"}})["ids"].size() == 1);
	CHECK(fixture.Run("scene.find", {{"name", "LevelOneMarker"}})["ids"].empty());
	CHECK(fixture.Run("script.eval", {{"code", "Game.Get('Coins')"}})["result"] == "7"); // Kept across the switch.
	fixture.Run("script.eval", {{"code", "Scene.Reload()"}});
	fixture.Run("play.step");
	CHECK(fixture.Run("scene.find", {{"name", "LevelTwoMarker"}})["ids"].size() == 1); // Reloads the current level.
	CHECK(fixture.Context.GetGame()->GetSceneSwitches() == 2);

	fixture.Run("play.stop");
	CHECK(fixture.Run("scene.find", {{"name", "LevelOneMarker"}})["ids"].size() == 1);
	CHECK(fixture.Run("scene.find", {{"name", "LevelTwoMarker"}})["ids"].empty());
	CHECK(fixture.Run("script.errors")["errors"].empty());
}
