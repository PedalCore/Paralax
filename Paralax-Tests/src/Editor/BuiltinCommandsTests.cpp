#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Audio/AudioSystem.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>
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

TEST_CASE("BuiltinCommands: errors show refused ids, names and keys briefly, whatever their size")
{
	EditorContext context;
	const std::string huge(16u << 20, 'x');
	constexpr size_t ShortErrorBytes = 1024;
	auto fails = [&](std::string_view command, const json& params) {
		const CommandResult result = context.Execute(command, params);
		CHECK_FALSE(result.Success);
		REQUIRE_MESSAGE(result.Error.size() < ShortErrorBytes, result.Error.substr(0, ShortErrorBytes));
		return result.Error;
	};

	// An id that names no entity.
	std::string error = fails("entity.get", {{"id", huge}});
	CHECK_MESSAGE(error.starts_with("No entity with id \"xxxx"), error);
	CHECK_MESSAGE(error.ends_with("... (string of 16777216 bytes)"), error);
	CHECK(fails("entity.destroy", {{"id", "42"}}) == "No entity with id \"42\" (string)");
	CHECK(fails("entity.rename", {{"id", json::array({1, 2})}, {"name", "A"}}) == "No entity with id [1,2] (array)");

	// Unknown parameters and commands.
	error = fails("entity.create", {{"name", "A"}, {huge, 1}});
	CHECK_MESSAGE(error.starts_with("'entity.create': unknown parameter 'xxxx"), error);
	CHECK_MESSAGE(error.ends_with("...' (16777216 bytes)"), error);
	CHECK(fails("entity.create", {{"colour", "red"}}) == "'entity.create': unknown parameter 'colour'");
	error = fails(huge, json::object());
	CHECK_MESSAGE(error.starts_with("Unknown command 'xxxx"), error);
	CHECK(error.find("...' (16777216 bytes)") != std::string::npos);
	error = fails("input.key", {{"key", huge}, {"action", "press"}});
	CHECK_MESSAGE(error.starts_with("Unknown key 'xxxx"), error);
	CHECK(error.find("...' (16777216 bytes); keys are: ") != std::string::npos);
	CHECK(fails("input.key", {{"key", "Nope"}, {"action", "press"}}).starts_with("Unknown key 'Nope'; keys are: "));

	// Many unknown components, one of them huge: the first few are named and the rest counted.
	json components = json::object();
	for (int i = 0; i < 10000; i++)
		components["Bogus" + std::to_string(10000 + i)] = json::object();
	components[huge] = json::object();
	error = fails("entity.create", {{"name", "A"}, {"components", components}});
	CHECK_MESSAGE(error.starts_with("unknown component 'Bogus10000'; unknown component 'Bogus10001'"), error);
	CHECK_MESSAGE(error.ends_with("; and 9993 more"), error);
	// A component's own field errors are capped the same way.
	json fields = json::object();
	for (int i = 0; i < 10000; i++)
		fields["Field" + std::to_string(10000 + i)] = huge.substr(0, 1000);
	error = fails("entity.create", {{"name", "A"}, {"components", {{"PointLight", fields}}}});
	CHECK_MESSAGE(error.starts_with("Unknown field 'PointLight.Field10000'"), error);
	CHECK_MESSAGE(error.ends_with("; and 9992 more"), error);
	// Nothing was created by the refused commands.
	CHECK(context.GetEditorScene().GetEntityCount() == 0);
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

	// An explicit null parent means root for entity.create too (generic clients send nulls for omitted fields).
	const std::string root = CreateEntity(context, {{"name", "Root"}, {"parent", nullptr}});
	CHECK(context.Execute("entity.get", {{"id", root}}).Result["parent"] == "0");
	CHECK_FALSE(context.Execute("entity.create", {{"name", "Orphan"}, {"parent", "12345"}}).Success);
	CHECK(context.Execute("entity.destroy", {{"id", root}}).Success);

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

TEST_CASE("BuiltinCommands: scene.save warns when the saved scene cannot be registered")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("SceneSaveRegistryLocked") / "Game";
	EditorContext context(Project::Create(directory, "Game"));
	REQUIRE(context.GetProject());
	CreateEntity(context, {{"name", "Player"}});
	const CommandResult registered = context.Execute("scene.save", {{"path", "Scenes/Main"}});
	REQUIRE(registered.Success);
	CHECK_FALSE(registered.Result.contains("warnings"));

	// A directory where the registry file belongs: the file is saved, but cannot be registered.
	const std::filesystem::path registry = directory / "Assets" / AssetManager::RegistryFileName;
	std::error_code error;
	std::filesystem::remove(registry, error);
	std::filesystem::create_directories(registry);
	Paralax::Testing::ScopedLogCapture capture;
	const CommandResult locked = context.Execute("scene.save", {{"path", "Scenes/Other"}});
	REQUIRE(locked.Success);
	CHECK(std::filesystem::is_regular_file(directory / "Assets" / "Scenes" / "Other.pxscene"));
	REQUIRE(locked.Result.contains("warnings"));
	CHECK(locked.Result["warnings"][0].get<std::string>().find("not registered") != std::string::npos);
	CHECK_FALSE(context.Execute("project.set_start_scene", {{"path", "Scenes/Other.pxscene"}}).Success);

	// A scene that was registered before needs no new entry, so it saves without a warning.
	REQUIRE(context.OpenScene("Scenes/Main.pxscene"));
	const CommandResult again = context.Execute("scene.save");
	REQUIRE(again.Success);
	CHECK_FALSE(again.Result.contains("warnings"));
}

TEST_CASE("BuiltinCommands: material.create leaves no file behind when the import fails")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("MaterialCreateRegistryLocked") / "Game";
	EditorContext context(Project::Create(directory, "Game"));
	REQUIRE(context.GetProject());
	// A directory where the registry file belongs: the registry cannot be saved, so the import fails.
	const std::filesystem::path registry = directory / "Assets" / AssetManager::RegistryFileName;
	std::error_code error;
	std::filesystem::remove(registry, error);
	std::filesystem::create_directories(registry);
	Paralax::Testing::ScopedLogCapture capture;
	CHECK_FALSE(context.Execute("material.create", {{"path", "M.pxmat"}}).Success);
	CHECK_FALSE(std::filesystem::exists(directory / "Assets" / "M.pxmat"));

	// Writable again: the same command goes through.
	std::filesystem::remove_all(registry);
	CHECK(context.Execute("material.create", {{"path", "M.pxmat"}}).Success);
	CHECK(std::filesystem::is_regular_file(directory / "Assets" / "M.pxmat"));
}

TEST_CASE("BuiltinCommands: asset.reload_registry takes in a registry changed on disk, after which imports save again")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AssetReloadRegistry") / "Game";
	EditorContext context(Project::Create(directory, "Game"));
	REQUIRE(context.GetProject());
	REQUIRE(context.Execute("material.create", {{"path", "M.pxmat"}}).Success);

	// A checkout replaces the registry under the running editor.
	const std::filesystem::path registry = directory / "Assets" / AssetManager::RegistryFileName;
	const std::string checkedOut = R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[{"Handle":"5003","Type":"Material","Path":"Other.pxmat"}]})";
	REQUIRE(WriteFileAtomically(registry, checkedOut));
	{
		// asset.list tells beforehand that a save would be refused, and why.
		const CommandResult listed = context.Execute("asset.list");
		REQUIRE(listed.Success);
		CHECK(listed.Result["changedOnDisk"] == true);
		CHECK(listed.Result["readOnly"] == false);
	}
	{
		Paralax::Testing::ScopedLogCapture capture;
		const CommandResult refused = context.Execute("material.create", {{"path", "N.pxmat"}});
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("console.get") != std::string::npos);
		CHECK(capture.Contains(spdlog::level::err, "has changed on disk"));
		CHECK_FALSE(std::filesystem::exists(directory / "Assets" / "N.pxmat"));
		CHECK(Paralax::Testing::ReadFileText(registry) == checkedOut);
	}

	// Not while playing (the game reads the registry), and not without a project.
	REQUIRE(context.Execute("play.start", {{"paused", true}}).Success);
	const CommandResult playing = context.Execute("asset.reload_registry");
	CHECK_FALSE(playing.Success);
	CHECK(playing.Error.find("play.stop") != std::string::npos);
	REQUIRE(context.Execute("play.stop").Success);
	CHECK_FALSE(EditorContext().Execute("asset.reload_registry").Success);

	const CommandResult reloaded = context.Execute("asset.reload_registry");
	REQUIRE_MESSAGE(reloaded.Success, reloaded.Error);
	CHECK(reloaded.Result["assets"] == 1);
	CHECK(reloaded.Result["skipped"] == 0);
	CHECK(reloaded.Result["readOnly"] == false);
	CHECK_FALSE(reloaded.Result.contains("warnings"));
	CHECK(context.Execute("asset.list").Result["readOnly"] == false);
	CHECK(context.Execute("asset.list").Result["changedOnDisk"] == false);
	CHECK(context.GetAssets().IsRegistered(AssetHandle(5003)));
	REQUIRE(context.Execute("material.create", {{"path", "N.pxmat"}}).Success);
	{
		AssetManager reopened(directory / "Assets");
		CHECK(reopened.IsRegistered(AssetHandle(5003)));
		CHECK(reopened.GetHandleForPath("N.pxmat").IsValid());
		CHECK_FALSE(reopened.GetHandleForPath("M.pxmat").IsValid());
	}

	// Entries this build cannot read (here, a newer engine's asset type) leave the registry read-only, and say so.
	const std::string newer = R"({"Paralax":{"Type":"AssetRegistry","Version":1},"Assets":[{"Handle":"5003","Type":"Material","Path":"Other.pxmat"},)"
							  R"({"Handle":"5006","Type":"Hologram","Path":"H.pxholo"}]})";
	REQUIRE(WriteFileAtomically(registry, newer));
	{
		Paralax::Testing::ScopedLogCapture capture;
		const CommandResult partial = context.Execute("asset.reload_registry");
		REQUIRE_MESSAGE(partial.Success, partial.Error);
		CHECK(partial.Result["assets"] == 1);
		CHECK(partial.Result["skipped"] == 1);
		CHECK(partial.Result["readOnly"] == true);
		REQUIRE(partial.Result.contains("warnings"));
		CHECK(partial.Result["warnings"][0].get<std::string>().find("1 registry entry could not be read and was skipped") != std::string::npos);
		const CommandResult listed = context.Execute("asset.list");
		REQUIRE(listed.Success);
		CHECK(listed.Result["readOnly"] == true);
		CHECK(listed.Result["skipped"] == 1);
		CHECK_FALSE(context.Execute("material.create", {{"path", "Q.pxmat"}}).Success);
		CHECK(capture.Contains(spdlog::level::err, "saving would drop it"));
		CHECK(Paralax::Testing::ReadFileText(registry) == newer);
	}

	// A file that cannot be read fails the reload, and stays as it is.
	const std::string conflicted = "<<<<<<< HEAD\n" + checkedOut + "\n=======\n";
	REQUIRE(WriteFileAtomically(registry, conflicted));
	Paralax::Testing::ScopedLogCapture capture;
	const CommandResult broken = context.Execute("asset.reload_registry");
	CHECK_FALSE(broken.Success);
	CHECK(broken.Error.find("could not be read") != std::string::npos);
	CHECK(context.GetAssets().GetRegisteredAssets().empty());
	CHECK_FALSE(context.Execute("material.create", {{"path", "P.pxmat"}}).Success);
	CHECK(Paralax::Testing::ReadFileText(registry) == conflicted);
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

TEST_CASE("BuiltinCommands: undo and redo while playing say to stop play mode, and keep the history")
{
	EditorContext context;
	// Something to undo and something to redo: two created, the second undone.
	const std::string box = CreateEntity(context, {{"name", "Box"}});
	const std::string ball = CreateEntity(context, {{"name", "Ball"}});
	REQUIRE(context.Execute("edit.undo").Success);
	REQUIRE(context.CanUndo());
	REQUIRE(context.CanRedo());
	REQUIRE(context.Execute("play.start").Success);

	for (const char* command : {"edit.undo", "edit.redo"})
	{
		const CommandResult result = context.Execute(command);
		CHECK_FALSE(result.Success);
		CHECK(result.Error == "Stop play mode first (play.stop)");
	}
	const json info = context.Execute("scene.info").Result;
	CHECK(info["canUndo"] == true);
	CHECK(info["canRedo"] == true);

	// Both kinds of history still work once play stops.
	REQUIRE(context.Execute("play.stop").Success);
	CHECK(context.Execute("edit.redo").Result["redone"] == "entity.create");
	CHECK(context.Execute("entity.get", {{"id", ball}}).Success);
	CHECK(context.Execute("edit.undo").Success);
	CHECK(context.Execute("edit.undo").Success);
	CHECK(context.Execute("scene.info").Result["entities"] == 0);
	CHECK(context.Execute("edit.redo").Success);
	CHECK(context.Execute("entity.get", {{"id", box}}).Success);
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

TEST_CASE("BuiltinCommands: names longer than a scene can hold are refused, and the longest survives undo, redo and a reload")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("EditorLongNames") / "Game";
	Ref<Project> project = Project::Create(directory, "Game");
	REQUIRE(project);
	EditorContext context(project);
	const std::string longest(TagComponent::MaxLength, 'n');
	const std::string tooLong = longest + "n";

	// The registry's rule and message, as for scene files, scripts and component.set.
	const CommandResult created = context.Execute("entity.create", {{"name", tooLong}});
	CHECK_FALSE(created.Success);
	CHECK(created.Error.find("at most 256 bytes") != std::string::npos);
	CHECK(context.Execute("scene.info").Result["entities"] == 0); // Rolled back.
	const std::string id = CreateEntity(context, {{"name", "Short"}});
	const CommandResult renamed = context.Execute("entity.rename", {{"id", id}, {"name", tooLong}});
	CHECK_FALSE(renamed.Success);
	CHECK(renamed.Error.find("at most 256 bytes") != std::string::npos);
	auto nameOf = [](EditorContext& editor, const std::string& entity) { return editor.Execute("entity.get", {{"id", entity}}).Result["name"]; };
	CHECK(nameOf(context, id) == "Short");
	CHECK(context.Execute("entity.create").Result.contains("id")); // No name: the default.

	// Undo and redo restore the scene from snapshots, and saving writes it out: the longest name survives each.
	REQUIRE(context.Execute("entity.rename", {{"id", id}, {"name", longest}}).Success);
	const std::string other = CreateEntity(context, {{"name", longest}});
	REQUIRE(context.Execute("edit.undo").Success);
	CHECK(nameOf(context, id) == longest);
	REQUIRE(context.Execute("edit.redo").Success);
	CHECK(nameOf(context, other) == longest);
	REQUIRE(context.Execute("scene.save", {{"path", "Scenes/Long"}}).Success);
	EditorContext reopened(Project::Load(directory));
	REQUIRE(reopened.Execute("scene.open", {{"path", "Scenes/Long.pxscene"}}).Success);
	CHECK(nameOf(reopened, id) == longest);
	CHECK(nameOf(reopened, other) == longest);
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
