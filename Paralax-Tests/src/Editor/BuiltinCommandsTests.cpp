#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Audio/AudioSystem.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scripting/ScriptAsset.h>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	std::string CreateEntity(EditorContext& context, json params = json::object())
	{
		CommandResult result = context.Execute("entity.create", params);
		REQUIRE_MESSAGE(result.Success, result.Error);
		return result.Result["id"].get<std::string>();
	}

}

TEST_CASE("BuiltinCommands: parameters are validated before commands run")
{
	EditorContext context;
	CHECK_FALSE(context.Execute("no.such.command").Success);
	CHECK(context.Execute("no.such.command").Error.find("help") != std::string::npos);
	CHECK_FALSE(context.Execute("entity.rename", {{"id", "1"}}).Success);                      // missing name
	CHECK_FALSE(context.Execute("entity.rename", {{"id", "1"}, {"name", 5}}).Success);         // wrong type
	CHECK_FALSE(context.Execute("entity.create", {{"name", "A"}, {"colour", "red"}}).Success); // unknown parameter
	CHECK_FALSE(context.Execute("scene.info", json::array()).Success);                         // not an object
	CHECK(context.Execute("scene.info", json()).Success);                                      // null = no parameters

	const json help = context.Execute("help").Result["commands"];
	CHECK(help.size() >= 20);
	bool hasCreate = false;
	for (const json& command : help)
		hasCreate |= command["name"] == "entity.create";
	CHECK(hasCreate);
}

TEST_CASE("BuiltinCommands: entities and components are created, edited and inspected")
{
	EditorContext context;
	const std::string parent = CreateEntity(context, {{"name", "Parent"}});
	const std::string child = CreateEntity(context, {{"name", "Lamp"}, {"parent", parent}, {"components", {{"PointLight", {{"Intensity", 25}, {"Color", {1, 0.5, 0}}}}, {"Transform", {{"Translation", {0, 3, 0}}}}}}});

	json entity = context.Execute("entity.get", {{"id", child}}).Result;
	CHECK(entity["name"] == "Lamp");
	CHECK(entity["parent"] == parent);
	CHECK(entity["components"]["PointLight"]["Intensity"] == 25.0);
	CHECK(entity["components"]["Transform"]["Translation"] == json::array({0.0, 3.0, 0.0}));
	CHECK(context.Execute("selection.get").Result["ids"] == json::array({child}));

	CHECK(context.Execute("component.set", {{"id", child}, {"components", {{"PointLight", {{"Range", 7}}}}}}).Success);
	entity = context.Execute("entity.get", {{"id", child}}).Result;
	CHECK(entity["components"]["PointLight"]["Range"] == 7.0);
	CHECK(entity["components"]["PointLight"]["Intensity"] == 25.0);

	CommandResult invalid = context.Execute("component.set", {{"id", child}, {"components", {{"PointLight", {{"Range", "far"}}}}}});
	CHECK_FALSE(invalid.Success);
	CHECK(context.Execute("entity.get", {{"id", child}}).Result["components"]["PointLight"]["Range"] == 7.0);

	CHECK_FALSE(context.Execute("component.remove", {{"id", child}, {"component", "Transform"}}).Success);
	CHECK(context.Execute("component.remove", {{"id", child}, {"component", "PointLight"}}).Success);
	CHECK_FALSE(context.Execute("component.remove", {{"id", child}, {"component", "PointLight"}}).Success);

	CHECK(context.Execute("entity.rename", {{"id", child}, {"name", "Bulb"}}).Success);
	CHECK(context.Execute("scene.find", {{"name", "Bulb"}}).Result["ids"] == json::array({child}));
	CHECK_FALSE(context.Execute("entity.get", {{"id", "12345"}}).Success);
	CHECK_FALSE(context.Execute("entity.get", {{"id", "not-a-number"}}).Success);
}

TEST_CASE("BuiltinCommands: entities can be reparented, duplicated and destroyed")
{
	EditorContext context;
	const std::string a = CreateEntity(context, {{"name", "A"}});
	const std::string b = CreateEntity(context, {{"name", "B"}});
	CHECK(context.Execute("entity.set_parent", {{"id", b}, {"parent", a}}).Success);
	CHECK_FALSE(context.Execute("entity.set_parent", {{"id", a}, {"parent", b}}).Success);

	const std::string copy = context.Execute("entity.duplicate", {{"id", a}}).Result["id"].get<std::string>();
	CHECK(context.Execute("scene.info").Result["entities"] == 4);
	CHECK(context.Execute("entity.set_parent", {{"id", b}, {"parent", nullptr}}).Success);

	CHECK(context.Execute("entity.destroy", {{"id", copy}}).Success);
	CHECK(context.Execute("scene.info").Result["entities"] == 2);
	CHECK(context.Execute("scene.list").Result["entities"].size() == 2);
}

TEST_CASE("BuiltinCommands: undo and redo restore scene state and selection")
{
	EditorContext context;
	CHECK_FALSE(context.Execute("edit.undo").Success);

	const std::string id = CreateEntity(context, {{"name", "Box"}});
	CHECK(context.Execute("component.set", {{"id", id}, {"components", {{"Transform", {{"Translation", {5, 0, 0}}}}}}}).Success);
	CHECK(context.IsSceneDirty());

	CHECK(context.Execute("edit.undo").Result["undone"] == "component.set");
	CHECK(context.Execute("entity.get", {{"id", id}}).Result["components"]["Transform"]["Translation"] == json::array({0.0, 0.0, 0.0}));
	CHECK(context.Execute("edit.undo").Success);
	CHECK(context.Execute("scene.info").Result["entities"] == 0);
	CHECK(context.GetSelection().empty());

	CHECK(context.Execute("edit.redo").Success);
	CHECK(context.Execute("edit.redo").Success);
	CHECK(context.Execute("entity.get", {{"id", id}}).Result["components"]["Transform"]["Translation"] == json::array({5.0, 0.0, 0.0}));
	CHECK_FALSE(context.Execute("edit.redo").Success);

	// A new change clears the redo stack; a no-op change records nothing.
	CHECK(context.Execute("edit.undo").Success);
	CHECK(context.Execute("entity.rename", {{"id", id}, {"name", "Crate"}}).Success);
	CHECK_FALSE(context.CanRedo());
	const std::string before = context.GetUndoDescription();
	CHECK(context.Execute("component.set", {{"id", id}, {"components", {{"Transform", {{"Translation", {0, 0, 0}}}}}}}).Success);
	CHECK(context.GetUndoDescription() == before);
}

TEST_CASE("BuiltinCommands: failed commands roll back partial changes")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "A"}});
	const size_t undoBefore = context.CanUndo() ? 1 : 0;
	// One valid and one invalid component: the command fails and nothing is left behind.
	CommandResult result = context.Execute("component.set", {{"id", id}, {"components", {{"PointLight", {{"Range", 3}}}, {"Bogus", json::object()}}}});
	CHECK_FALSE(result.Success);
	CHECK_FALSE(context.Execute("entity.get", {{"id", id}}).Result["components"].contains("PointLight"));
	CHECK((context.CanUndo() ? 1 : 0) == undoBefore);
}

TEST_CASE("BuiltinCommands: play mode runs on a copy and discards changes")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Original"}});
	CHECK_FALSE(context.Execute("play.stop").Success);
	CHECK(context.Execute("play.start").Success);
	CHECK_FALSE(context.Execute("play.start").Success);
	CHECK(context.Execute("scene.info").Result["playing"] == true);

	CHECK(context.Execute("entity.rename", {{"id", id}, {"name", "Changed"}}).Success);
	CHECK(context.Execute("play.step", {{"frames", 10}}).Success);
	CHECK_FALSE(context.Execute("play.step", {{"frames", 0}}).Success);
	CHECK_FALSE(context.Execute("scene.save", {{"path", "x.pxscene"}}).Success);
	CHECK_FALSE((context.CanUndo() && context.GetUndoDescription() == "entity.rename"));

	CHECK(context.Execute("play.stop").Success);
	CHECK(context.Execute("entity.get", {{"id", id}}).Result["name"] == "Original");
	CHECK_FALSE(context.Execute("play.step").Success);
}

TEST_CASE("BuiltinCommands: scenes and materials are saved to and loaded from the project")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("EditorProject") / "Game";
	Ref<Project> project = Project::Create(directory, "Game");
	REQUIRE(project);
	CHECK(std::filesystem::exists(directory / "Game.pxproj"));
	CHECK(Project::Create(directory, "Game") == nullptr);
	CHECK(Project::Create(directory / "Other", "Bad/Name") == nullptr);

	EditorContext context(project);
	CHECK_FALSE(context.Execute("scene.save").Success); // no path yet
	const std::string material = context.Execute("material.create", {{"path", "Materials/Red"}, {"fields", {{"BaseColor", {1, 0, 0, 1}}, {"Roughness", 0.3}}}}).Result["handle"].get<std::string>();
	CHECK_FALSE(material.empty());
	CHECK(std::filesystem::exists(directory / "Assets" / "Materials" / "Red.pxmat"));
	CHECK(context.Execute("material.set", {{"handle", material}, {"fields", {{"Metallic", 1.0}}}}).Success);
	CHECK_FALSE(context.Execute("material.set", {{"handle", "16"}, {"fields", {{"Metallic", 1.0}}}}).Success);
	CHECK_FALSE(context.Execute("material.create", {{"path", "../Escape.pxmat"}}).Success);

	CreateEntity(context, {{"name", "Ball"}, {"components", {{"Mesh", {{"Mesh", "2"}, {"Materials", {material}}}}}}});
	REQUIRE(context.Execute("scene.save", {{"path", "Scenes/Level1"}}).Success);
	CHECK_FALSE(context.IsSceneDirty());
	CHECK(std::filesystem::exists(directory / "Assets" / "Scenes" / "Level1.pxscene"));

	// Reload everything from disk.
	Ref<Project> reloaded = Project::Load(directory);
	REQUIRE(reloaded);
	EditorContext second(reloaded);
	REQUIRE(second.Execute("scene.open", {{"path", "Scenes/Level1.pxscene"}}).Success);
	const json found = second.Execute("scene.find", {{"name", "Ball"}}).Result["ids"];
	REQUIRE(found.size() == 1);
	CHECK(second.Execute("entity.get", {{"id", found[0]}}).Result["components"]["Mesh"]["Materials"] == json::array({material}));
	Ref<MaterialAsset> loadedMaterial = second.GetAssets().GetAsset<MaterialAsset>(AssetHandle(std::stoull(material)));
	REQUIRE(loadedMaterial);
	CHECK(loadedMaterial->MetallicFactor == 1.0f);
	CHECK(loadedMaterial->RoughnessFactor == 0.3f);
	CHECK_FALSE(second.Execute("scene.open", {{"path", "Scenes/Missing.pxscene"}}).Success);
}

TEST_CASE("BuiltinCommands: component type descriptions expose fields, ranges and enums")
{
	EditorContext context;
	const json types = context.Execute("component.types").Result["components"];
	bool foundCamera = false;
	for (const json& type : types)
	{
		if (type["name"] != "Camera")
			continue;
		foundCamera = true;
		for (const json& field : type["fields"])
		{
			if (field["name"] == "Projection")
				CHECK(field["values"] == json::array({"Perspective", "Orthographic"}));
			if (field["name"] == "FieldOfView")
				CHECK(field["units"] == "radians");
		}
	}
	CHECK(foundCamera);
	CHECK_FALSE(context.Execute("render.capture", {{"path", "x.png"}}).Success); // no renderer in tests
}

TEST_CASE("BuiltinCommands: material.set with an invalid field changes nothing in memory or on disk")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MaterialSetInvalid") / "Game";
	Ref<Project> project = Project::Create(directory, "Game");
	REQUIRE(project);
	EditorContext context(project);
	const std::string handle = context.Execute("material.create", {{"path", "Materials/M"}, {"fields", {{"Metallic", 0.25}}}}).Result["handle"].get<std::string>();
	const std::filesystem::path file = directory / "Assets" / "Materials" / "M.pxmat";
	const std::string before = Paralax::Testing::ReadFileText(file);
	Ref<MaterialAsset> material = context.GetAssets().GetAsset<MaterialAsset>(AssetHandle(std::stoull(handle)));
	REQUIRE(material);
	const uint32_t version = material->Version;

	CHECK_FALSE(context.Execute("material.set", {{"handle", handle}, {"fields", {{"Metallic", 1.0}, {"Bogus", 3}}}}).Success);
	CHECK_FALSE(context.Execute("material.set", {{"handle", handle}, {"fields", {{"Roughness", "rough"}}}}).Success);
	CHECK(material->MetallicFactor == 0.25f);
	CHECK(material->Version == version);
	CHECK(Paralax::Testing::ReadFileText(file) == before);

	REQUIRE(context.Execute("material.set", {{"handle", handle}, {"fields", {{"Metallic", 1.0}}}}).Success);
	CHECK(material->MetallicFactor == 1.0f);
	CHECK(material->Version == version + 1);
	CHECK(Paralax::Testing::ReadFileText(file) != before);
}

TEST_CASE("BuiltinCommands: material.create does not overwrite an existing material")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MaterialCreateExisting") / "Game";
	Ref<Project> project = Project::Create(directory, "Game");
	REQUIRE(project);
	EditorContext context(project);
	REQUIRE(context.Execute("material.create", {{"path", "M.pxmat"}, {"fields", {{"Roughness", 0.2}}}}).Success);
	const std::string before = Paralax::Testing::ReadFileText(directory / "Assets" / "M.pxmat");
	const CommandResult again = context.Execute("material.create", {{"path", "M.pxmat"}, {"fields", {{"Roughness", 0.9}}}});
	CHECK_FALSE(again.Success);
	CHECK(again.Error.find("already exists") != std::string::npos);
	CHECK(Paralax::Testing::ReadFileText(directory / "Assets" / "M.pxmat") == before);
	CHECK_FALSE(EditorContext().Execute("material.create", {{"path", "M.pxmat"}}).Success); // No project.
}

TEST_CASE("BuiltinCommands: play.step validates its frame count and time step")
{
	EditorContext context;
	REQUIRE(context.Execute("play.start").Success);
	CHECK(context.Execute("play.step", {{"frames", 3}}).Result["frames"] == 3);
	CHECK_FALSE(context.Execute("play.step", {{"frames", 0}}).Success);
	CHECK_FALSE(context.Execute("play.step", {{"frames", 10001}}).Success);
	CHECK_FALSE(context.Execute("play.step", {{"dt", 0.0}}).Success);
	CHECK_FALSE(context.Execute("play.step", {{"dt", -1.0}}).Success);
	CHECK_FALSE(context.Execute("play.step", {{"dt", 1.5}}).Success);
	CHECK(context.Execute("play.step", {{"dt", 1.0}}).Success);
	CHECK_FALSE(context.Execute("play.start").Success);
}

TEST_CASE("BuiltinCommands: edits while playing leave the editor scene and undo history untouched")
{
	EditorContext context;
	const std::string parent = CreateEntity(context, {{"name", "Parent"}});
	const std::string child = CreateEntity(context, {{"name", "Child"}});
	const std::string editorScene = context.Execute("scene.list").Result.dump();
	const std::string undo = context.GetUndoDescription();

	REQUIRE(context.Execute("play.start").Success);
	CHECK(context.Execute("entity.set_parent", {{"id", child}, {"parent", parent}}).Success);
	CHECK(context.Execute("component.set", {{"id", parent}, {"components", {{"Transform", {{"Translation", {5, 5, 5}}}}}}}).Success);
	CHECK(context.Execute("entity.destroy", {{"id", parent}}).Success);
	CHECK(context.GetActiveScene().GetEntityCount() == 0);
	REQUIRE(context.Execute("play.stop").Success);

	CHECK(context.Execute("scene.list").Result.dump() == editorScene);
	CHECK(context.GetUndoDescription() == undo);
}

TEST_CASE("BuiltinCommands: component.set cannot give an entity an empty name")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Named"}});
	CHECK_FALSE(context.Execute("component.set", {{"id", id}, {"components", {{"Tag", {{"Name", ""}}}}}}).Success);
	CHECK(context.Execute("entity.get", {{"id", id}}).Result["name"] == "Named");
	CHECK(context.Execute("component.set", {{"id", id}, {"components", {{"Tag", {{"Tag", "Enemy"}}}}}}).Success);
	// Numeric ids are accepted for parents, like every other id parameter.
	const CommandResult child = context.Execute("entity.create", {{"name", "Child"}, {"parent", std::stoull(id)}});
	REQUIRE(child.Success);
	CHECK(context.Execute("entity.get", {{"id", child.Result["id"]}}).Result["parent"] == id);
}

TEST_CASE("BuiltinCommands: asset list exposes the complete built-in catalog without a project")
{
	EditorContext context;
	const auto result = context.Execute("asset.list");
	REQUIRE(result.Success);
	const auto& builtins = result.Result["builtins"];
	REQUIRE(builtins.size() == AssetManager::GetBuiltinAssets().size());
	size_t index = 0;
	for (const BuiltinAssetInfo& info : AssetManager::GetBuiltinAssets())
	{
		CHECK(builtins[index]["handle"].get<std::string>() == std::to_string(info.Handle));
		CHECK(builtins[index]["type"].get<std::string>() == std::string(AssetTypeToString(info.Type)));
		CHECK(builtins[index]["name"].get<std::string>() == std::string(info.Name));
		index++;
	}
}

TEST_CASE("BuiltinCommands: a paused game advances only through play.step")
{
	EditorContext context;
	CHECK_FALSE(context.Execute("play.pause").Success); // Not playing.
	CHECK_FALSE(context.Execute("play.resume").Success);
	REQUIRE(context.Execute("play.start", {{"paused", true}}).Success);
	CHECK(context.IsPaused());

	// The editor's frame loop calls TickRuntime; a paused scene ignores it, but play.step still advances.
	context.TickRuntime(Timestep(1.0f));
	CHECK(context.Execute("play.step", {{"frames", 2}}).Success);
	CHECK(context.Execute("play.resume").Success);
	CHECK_FALSE(context.IsPaused());
	CHECK(context.Execute("play.pause").Success);
	CHECK(context.IsPaused());

	REQUIRE(context.Execute("play.stop").Success);
	CHECK_FALSE(context.IsPaused()); // Stopping clears the pause.
	context.SetPaused(true);
	CHECK_FALSE(context.IsPaused()); // Pausing outside play mode is ignored.
	REQUIRE(context.Execute("play.start").Success);
	CHECK_FALSE(context.IsPaused());
	context.Execute("play.stop");
}

TEST_CASE("BuiltinCommands: TickRuntime runs scripts unless paused")
{
	EditorContext context;
	Ref<ScriptAsset> script = ScriptAsset::Create("Counter.lua", "local S = {} function S:OnUpdate() Ticks = (Ticks or 0) + 1 end return S");
	script->Handle = AssetHandle(9001);
	context.GetAssets().AddMemoryAsset(script);
	CreateEntity(context, {{"components", {{"Script", {{"Script", "9001"}}}}}});

	REQUIRE(context.Execute("play.start", {{"paused", true}}).Success);
	context.TickRuntime(Timestep(0.1f));
	context.TickRuntime(Timestep(0.1f));
	CHECK(context.Execute("script.eval", {{"code", "Ticks"}}).Result["result"] == "nil");
	context.Execute("play.step");
	context.Execute("play.resume");
	context.TickRuntime(Timestep(0.1f));
	CHECK(context.Execute("script.eval", {{"code", "Ticks"}}).Result["result"] == "2");
	context.Execute("play.stop");
}

TEST_CASE("BuiltinCommands: audio output is chosen and inspected through commands, and pausing play pauses sound")
{
	EditorContext context;
	CHECK(context.GetAudioOutput() == AudioOutput::Offline); // Headless and agent sessions are silent by default.
	CHECK_FALSE(context.Execute("audio.set_output", {{"output", "Speakers"}}).Success);
	json status = context.Execute("audio.status").Result;
	CHECK(status["playing"] == false);
	CHECK(status["chosenOutput"] == "Offline");

	REQUIRE(context.Execute("play.start", {{"paused", true}}).Success);
	REQUIRE(context.GetAudioSystem());
	CHECK(context.GetAudioSystem()->IsPaused());
	status = context.Execute("audio.status").Result;
	CHECK(status["playing"] == true);
	CHECK(status["output"] == "Offline");
	CHECK(status["paused"] == true);
	CHECK(status["voices"] == 0);
	CHECK(status["masterVolume"] == 1.0);
	REQUIRE(context.Execute("play.resume").Success);
	CHECK_FALSE(context.GetAudioSystem()->IsPaused());
	REQUIRE(context.Execute("play.pause").Success);
	CHECK(context.GetAudioSystem()->IsPaused());
	REQUIRE(context.Execute("play.stop").Success);
	CHECK(context.GetAudioSystem() == nullptr);

	// The choice applies from the next play session; without a device it falls back to offline mixing.
	CHECK(context.Execute("audio.set_output", {{"output", "Device"}}).Result["output"] == "Device");
	CHECK(context.GetAudioOutput() == AudioOutput::Device);
	REQUIRE(context.Execute("play.start").Success);
	status = context.Execute("audio.status").Result;
	CHECK(status["chosenOutput"] == "Device");
	CHECK((status["output"] == "Device" || status["output"] == "Offline"));
	REQUIRE(context.Execute("play.stop").Success);
}
