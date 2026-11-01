#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>

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
