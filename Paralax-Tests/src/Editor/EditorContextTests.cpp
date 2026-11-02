#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Scene/Entity.h>

#include <filesystem>
#include <fstream>

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

	std::string NameOf(EditorContext& context, const std::string& id)
	{
		return context.Execute("entity.get", {{"id", id}}).Result["name"].get<std::string>();
	}

	// Edits an entity's name directly, the way a UI widget does between ApplyInteractiveEdit calls.
	void SetNameDirectly(EditorContext& context, const std::string& id, const std::string& name)
	{
		std::optional<Entity> entity = context.ResolveEntity(id);
		REQUIRE(entity);
		entity->GetComponent<TagComponent>().Name = name;
	}

}

TEST_CASE("EditorContext: a failed command that changed nothing keeps the same Scene object")
{
	EditorContext context;
	const std::string parent = CreateEntity(context, {{"name", "Parent"}});
	const std::string child = CreateEntity(context, {{"name", "Child"}, {"parent", parent}});
	const Scene* before = &context.GetActiveScene();

	// A cycle is rejected without touching the scene: UI code holding references across this call stays valid.
	CHECK_FALSE(context.Execute("entity.set_parent", {{"id", parent}, {"parent", child}}).Success);
	CHECK(&context.GetActiveScene() == before);
	CHECK_FALSE(context.Execute("entity.destroy", {{"id", "999"}}).Success);
	CHECK(&context.GetActiveScene() == before);
}

TEST_CASE("EditorContext: the demo scene is the starting point, clean and with nothing to undo")
{
	EditorContext context;
	CHECK(CreateDemoScene(context));
	CHECK(context.GetEditorScene().GetEntityCount() == 6);
	CHECK(context.GetEditorScene().GetPrimaryCameraEntity().has_value());
	const json info = context.Execute("scene.info").Result;
	CHECK(info["dirty"] == false);
	CHECK(info["canUndo"] == false);
	CHECK(info["canRedo"] == false);
	CHECK(context.GetSelection().empty());
	CHECK_FALSE(context.Execute("edit.undo").Success);

	// The first edit is undoable, back to the demo and no further.
	const std::string id = CreateEntity(context, {{"name", "Extra"}});
	CHECK(context.IsSceneDirty());
	CHECK(context.Execute("edit.undo").Success);
	CHECK_FALSE(context.ResolveEntity(id));
	CHECK(context.GetEditorScene().GetEntityCount() == 6);
	CHECK_FALSE(context.CanUndo());
}

TEST_CASE("EditorContext: the demo scene never fills a scene that was opened, even an empty one")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("EditorContextDemoScene");
	{
		EditorContext author(Project::Create(root / "Game", "Game"));
		REQUIRE(author.GetProject());
		REQUIRE(author.Execute("scene.save", {{"path", "Scenes/Empty"}}).Success);
		REQUIRE(author.Execute("project.set_start_scene", {{"path", "Scenes/Empty.pxscene"}}).Success);
	}

	// What the editor does at start: open the start scene, then offer the demo.
	EditorContext context(Project::Load(root / "Game"));
	REQUIRE(context.GetProject());
	REQUIRE(context.OpenScene("Scenes/Empty.pxscene"));
	CHECK_FALSE(CreateDemoScene(context));
	CHECK(context.GetEditorScene().GetEntityCount() == 0);
	const json info = context.Execute("scene.info").Result;
	CHECK(info["dirty"] == false);
	CHECK(info["canUndo"] == false);
	CHECK(info["path"].get<std::string>().find("Empty.pxscene") != std::string::npos);

	// Nor an unsaved scene with entities in it.
	EditorContext unsaved;
	CreateEntity(unsaved, {{"name", "Mine"}});
	CHECK_FALSE(CreateDemoScene(unsaved));
	CHECK(unsaved.GetEditorScene().GetEntityCount() == 1);
}

TEST_CASE("EditorContext: ResetHistory forgets undo and redo and marks the scene clean, but not during a change")
{
	EditorContext context;
	CreateEntity(context, {{"name", "A"}});
	CreateEntity(context, {{"name", "B"}});
	REQUIRE(context.Execute("edit.undo").Success);
	REQUIRE(context.CanUndo());
	REQUIRE(context.CanRedo());

	context.BeginUndoableChange("open");
	{
		Paralax::Testing::ScopedLogCapture capture;
		CHECK_FALSE(context.ResetHistory());
		CHECK(capture.Contains(spdlog::level::err, "in progress"));
	}
	context.EndUndoableChange(false);
	CHECK(context.CanUndo());

	CHECK(context.ResetHistory());
	CHECK_FALSE(context.CanUndo());
	CHECK_FALSE(context.CanRedo());
	CHECK_FALSE(context.IsSceneDirty());
	CHECK(context.GetEditorScene().GetEntityCount() == 1); // The scene itself is untouched.
}

TEST_CASE("EditorContext: a failed command that changed the scene is rolled back")
{
	EditorContext context;
	// The first component applies, the second is unknown: the whole command fails and the entity is not created.
	CHECK_FALSE(context.Execute("entity.create", {{"name", "Half"}, {"components", {{"Camera", json::object()}, {"Bogus", json::object()}}}}).Success);
	CHECK(context.GetEditorScene().GetEntityCount() == 0);
	CHECK_FALSE(context.CanUndo());
}

TEST_CASE("EditorContext: nested undo brackets merge into one step")
{
	EditorContext context;
	context.BeginUndoableChange("Outer");
	CreateEntity(context, {{"name", "A"}});
	CreateEntity(context, {{"name", "B"}});
	CHECK(context.IsChangeOpen());
	context.EndUndoableChange(true);
	CHECK_FALSE(context.IsChangeOpen());

	CHECK(context.GetUndoDescription() == "Outer");
	REQUIRE(context.Undo());
	CHECK(context.GetEditorScene().GetEntityCount() == 0);
	CHECK_FALSE(context.CanUndo());
}

TEST_CASE("EditorContext: the undo history keeps at most MaxUndoSteps steps")
{
	EditorContext context;
	for (size_t i = 0; i < EditorContext::MaxUndoSteps + 5; i++)
		CreateEntity(context, {{"name", "E"}});
	size_t undone = 0;
	while (context.Undo())
		undone++;
	CHECK(undone == EditorContext::MaxUndoSteps);
	CHECK(context.GetEditorScene().GetEntityCount() == 5);
}

TEST_CASE("EditorContext: a bracket opened before play mode starts still closes")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Before"}});

	// A drag starts, then play mode starts mid-drag (e.g. an agent request), then the drag ends.
	context.BeginUndoableChange("Drag");
	SetNameDirectly(context, id, "Dragged");
	REQUIRE(context.StartPlaying());
	context.EndUndoableChange(true);
	CHECK_FALSE(context.IsChangeOpen());
	REQUIRE(context.StopPlaying());

	// The edit made before play started is one undo step, and later changes are recorded normally.
	CHECK(context.GetUndoDescription() == "Drag");
	CreateEntity(context, {{"name", "After"}});
	CHECK(context.GetUndoDescription() == "entity.create");
	CHECK(context.IsSceneDirty());
}

TEST_CASE("EditorContext: brackets opened while playing record nothing and stay balanced")
{
	EditorContext context;
	REQUIRE(context.StartPlaying());
	context.BeginUndoableChange("Runtime");
	REQUIRE(context.StopPlaying());
	context.EndUndoableChange(true);
	CHECK_FALSE(context.IsChangeOpen());
	CHECK_FALSE(context.CanUndo());

	CreateEntity(context);
	CHECK(context.CanUndo());
}

TEST_CASE("EditorContext: undo, redo and replacing the scene are refused while a change is open")
{
	EditorContext context;
	CreateEntity(context, {{"name", "A"}});
	context.BeginUndoableChange("Open");
	CHECK_FALSE(context.Undo());
	CHECK_FALSE(context.NewScene());
	CHECK_FALSE(context.OpenScene("Anything.pxscene"));
	CHECK(context.Execute("edit.undo").Error.find("in progress") != std::string::npos);
	CHECK_FALSE(context.Execute("scene.new").Success);
	context.EndUndoableChange(true);

	CHECK(context.Undo());
	CHECK(context.Redo());
	CHECK(context.NewScene("Fresh"));
	CHECK(context.GetEditorScene().GetName() == "Fresh");
}

#if defined(PX_ENABLE_ASSERTS)
TEST_CASE("EditorContext: ending a change that was never begun is an asserted precondition")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	EditorContext context;
	CHECK_THROWS_AS(context.EndUndoableChange(true), Paralax::Testing::AssertionFailedError);
}
#else
TEST_CASE("EditorContext: in Dist, ending a change that was never begun is ignored")
{
	EditorContext context;
	context.EndUndoableChange(true);
	CHECK_FALSE(context.IsChangeOpen());
}
#endif

TEST_CASE("EditorContext: an interactive edit spanning many frames is one undo step")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Start"}});

	// Frame 1: the drag begins and changes the value.
	context.ApplyInteractiveEdit(1, "Drag");
	SetNameDirectly(context, id, "Step1");
	context.EndStaleInteractiveEdit();
	// Frame 2: still dragging without a change this frame.
	context.TouchInteractiveEdit(1);
	context.EndStaleInteractiveEdit();
	// Frame 3: another change.
	context.ApplyInteractiveEdit(1, "Drag");
	SetNameDirectly(context, id, "Step2");
	context.EndStaleInteractiveEdit();
	CHECK(context.IsChangeOpen());
	// Frame 4: released (no touch): the edit closes.
	context.EndStaleInteractiveEdit();
	CHECK_FALSE(context.IsChangeOpen());

	CHECK(context.GetUndoDescription() == "Drag");
	REQUIRE(context.Undo());
	CHECK(NameOf(context, id) == "Start");
	REQUIRE(context.Redo());
	CHECK(NameOf(context, id) == "Step2");
}

TEST_CASE("EditorContext: starting an interactive edit on another key commits the previous one")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Start"}});
	context.ApplyInteractiveEdit(1, "First");
	SetNameDirectly(context, id, "One");
	context.ApplyInteractiveEdit(2, "Second");
	SetNameDirectly(context, id, "Two");
	context.CommitInteractiveEdit();
	CHECK_FALSE(context.IsChangeOpen());

	CHECK(context.GetUndoDescription() == "Second");
	REQUIRE(context.Undo());
	CHECK(NameOf(context, id) == "One");
	CHECK(context.GetUndoDescription() == "First");

	// Touching an edit that is not open does nothing.
	context.TouchInteractiveEdit(7);
	CHECK_FALSE(context.IsChangeOpen());
}

TEST_CASE("EditorContext: undo commits an open interactive edit first")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Start"}});
	context.ApplyInteractiveEdit(1, "Typing");
	SetNameDirectly(context, id, "Typed");
	REQUIRE(context.Undo());
	CHECK(NameOf(context, id) == "Start");
	CHECK_FALSE(context.IsChangeOpen());
}

TEST_CASE("EditorContext: deferred commands run in order after the caller is done")
{
	EditorContext context;
	context.Defer("entity.create", {{"name", "First"}});
	context.Defer("no.such.command");
	context.Defer("entity.create", {{"name", "Second"}});
	CHECK(context.GetEditorScene().GetEntityCount() == 0);

	Paralax::Testing::ScopedLogCapture log;
	CHECK(context.ExecuteDeferred() == 1);
	CHECK(context.GetEditorScene().GetEntityCount() == 2);
	CHECK(context.Execute("scene.find", {{"name", "Second"}}).Result["ids"].size() == 1);
	CHECK(context.ExecuteDeferred() == 0);
}

TEST_CASE("EditorContext: selection ignores unknown entities and is restored by undo")
{
	EditorContext context;
	const std::string a = CreateEntity(context, {{"name", "A"}});
	const std::string b = CreateEntity(context, {{"name", "B"}});
	context.SetSelection({UUID(std::stoull(a)), UUID(12345)});
	REQUIRE(context.GetSelection().size() == 1);
	CHECK(context.IsSelected(UUID(std::stoull(a))));

	REQUIRE(context.Execute("entity.destroy", {{"id", a}}).Success);
	CHECK(context.GetSelection().empty());
	REQUIRE(context.Undo());
	CHECK(context.IsSelected(UUID(std::stoull(a))));
	CHECK_FALSE(context.IsSelected(UUID(std::stoull(b))));
}

TEST_CASE("EditorContext: scene paths must stay inside the project's asset directory")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("EditorContextPaths");
	Ref<Project> project = Project::Create(root / "Game", "Game");
	REQUIRE(project);
	EditorContext context(project);
	Paralax::Testing::ScopedLogCapture log;

	std::string error;
	CHECK(context.ResolveProjectPath("Scenes/A.pxscene", error) == project->GetAssetDirectory() / "Scenes" / "A.pxscene");
	CHECK_FALSE(context.ResolveProjectPath("../Outside.pxscene", error));
	CHECK(error.find("outside") != std::string::npos);
	CHECK_FALSE(context.ResolveProjectPath(root / "Elsewhere.pxscene", error));
	CHECK_FALSE(context.ResolveProjectPath("", error));

	CHECK_FALSE(context.Execute("scene.save", {{"path", "../../Escape"}}).Success);
	CHECK_FALSE(std::filesystem::exists(root / "Escape.pxscene"));
	CHECK_FALSE(context.Execute("scene.save", {{"path", ToUtf8(root / "Absolute")}}).Success);
	CHECK_FALSE(std::filesystem::exists(root / "Absolute.pxscene"));
	CHECK_FALSE(context.Execute("scene.open", {{"path", "../Game.pxproj"}}).Success);

	// A symlink inside Assets that points outside does not let writes escape.
	std::error_code linkError;
	std::filesystem::create_directory_symlink(root, project->GetAssetDirectory() / "Link", linkError);
	if (!linkError)
	{
		CHECK_FALSE(context.Execute("scene.save", {{"path", "Link/Escaped"}}).Success);
		CHECK_FALSE(std::filesystem::exists(root / "Escaped.pxscene"));
	}

	CHECK(context.Execute("scene.save", {{"path", "Scenes/Inside"}}).Success);
	CHECK(std::filesystem::exists(project->GetAssetDirectory() / "Scenes" / "Inside.pxscene"));
}

TEST_CASE("EditorContext: a scene file name that is not UTF-8 is refused before anything is written")
{
	if constexpr (sizeof(std::filesystem::path::value_type) != 1)
	{
		MESSAGE("Only POSIX file names can be bytes that are not UTF-8; nothing to check here");
	}
	else
	{
		// Latin-1 "Niveau-é": the project could not record it in the registry, so the scene is not written at all.
		const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("EditorContextNotUtf8");
		Ref<Project> project = Project::Create(root / "Game", "Game");
		REQUIRE(project);
		EditorContext context(project);
		const std::filesystem::path file = project->GetAssetDirectory() / "Scenes" / FromUtf8("Niveau-\xE9.pxscene");
		std::string error;
		CHECK_FALSE(context.ResolveProjectPath(file, error));
		CHECK(error.find("is not a valid UTF-8 file name") != std::string::npos);

		Paralax::Testing::ScopedLogCapture log;
		CHECK_FALSE(context.SaveScene(file));
		CHECK_FALSE(std::filesystem::exists(file));
		CHECK(log.Contains(spdlog::level::err, "is not a valid UTF-8 file name"));

		// Commands only carry UTF-8, but a UTF-8 link can still resolve to such a name: it is refused the same way. Some
		// file systems refuse such names outright (APFS, on macOS, requires UTF-8), and then no link can lead to one.
		std::ofstream(file) << "{}";
		std::error_code existsError;
		if (!std::filesystem::is_regular_file(file, existsError))
		{
			MESSAGE("This file system refuses names that are not UTF-8, so no link can resolve to one");
		}
		else
		{
			std::error_code linkError;
			std::filesystem::create_symlink(file, project->GetAssetDirectory() / "Scenes" / "Link.pxscene", linkError);
			if (!linkError)
			{
				const CommandResult opened = context.Execute("scene.open", {{"path", "Scenes/Link.pxscene"}});
				CHECK_FALSE(opened.Success);
				CHECK_FALSE(project->GetAssetManager().GetHandleForPath("Scenes/Link.pxscene").IsValid());
			}
		}
	}
}

TEST_CASE("EditorContext: output files must have the expected extension and stay inside the project")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("EditorContextOutput");
	std::string error;

	EditorContext scratch; // No project: paths are used as given.
	CHECK(scratch.ResolveOutputPath(root / "Shot.png", ".png", error) == root / "Shot.png");
	CHECK(scratch.ResolveOutputPath(root / "Shot.PNG", ".png", error));
	CHECK_FALSE(scratch.ResolveOutputPath(root / "notes.txt", ".png", error));
	CHECK(error.find(".png") != std::string::npos);
	CHECK_FALSE(scratch.ResolveOutputPath(root / "Shot", ".png", error));
	CHECK_FALSE(scratch.ResolveOutputPath("", ".png", error));

	Ref<Project> project = Project::Create(root / "Game", "Game");
	REQUIRE(project);
	EditorContext context(project);
	CHECK(context.ResolveOutputPath("Captures/Shot.png", ".png", error) == project->GetDirectory() / "Captures" / "Shot.png");
	CHECK_FALSE(context.ResolveOutputPath("../Shot.png", ".png", error));
	CHECK_FALSE(context.ResolveOutputPath(root / "Shot.png", ".png", error));
	CHECK(error.find("outside") != std::string::npos);
}

TEST_CASE("EditorContext: names with invalid UTF-8 can still be snapshotted, undone and described")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", std::string("Bad\xff\xfeName")}});
	CHECK(context.Execute("entity.rename", {{"id", id}, {"name", "Good"}}).Success);
	REQUIRE(context.Undo());
	const CommandResult described = context.Execute("entity.get", {{"id", id}});
	REQUIRE(described.Success);
	CHECK_NOTHROW((void)described.Result.dump(-1, ' ', false, json::error_handler_t::replace));
}

TEST_CASE("EditorContext: a value a restored snapshot cannot hold is logged and reported, and the rest is kept")
{
	// Written behind the registry's back, as no edit may: the scene format refuses the name, so loading a snapshot
	// that holds it drops it (the entity is named "Entity" again), as opening a saved file would.
	auto plant = [](EditorContext& context, const std::string& id) {
		context.GetActiveScene().GetEntityByUUID(UUID(std::stoull(id))).GetComponent<TagComponent>().Name = std::string(TagComponent::MaxLength + 1, 'n');
	};
	auto nameOf = [](EditorContext& context, const std::string& id) { return context.Execute("entity.get", {{"id", id}}).Result["name"]; };

	// Undo: the result lists what was dropped, the log has it as an error, and nothing aborts.
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Box"}});
	const std::string other = CreateEntity(context, {{"name", "Other"}});
	plant(context, id);
	REQUIRE(context.Execute("component.set", {{"id", other}, {"components", {{"Transform", {{"Translation", {1, 0, 0}}}}}}}).Success);
	{
		Paralax::Testing::ScopedLogCapture log;
		const CommandResult undone = context.Execute("edit.undo");
		REQUIRE(undone.Success);
		REQUIRE(undone.Result.contains("warnings"));
		REQUIRE(undone.Result["warnings"].size() == 1);
		CHECK(undone.Result["warnings"][0].get<std::string>().find("at most 256 bytes") != std::string::npos);
		CHECK(log.Contains(spdlog::level::err, "dropped a value"));
	}
	CHECK(nameOf(context, id) == "Entity");
	CHECK(nameOf(context, other) == "Other");
	CHECK(context.Execute("entity.get", {{"id", other}}).Result["components"]["Transform"]["Translation"] == json::array({0.0, 0.0, 0.0}));
	// The history survives: the change can be redone. The redo step was snapshotted before the undo, with the planted
	// name still in it, so redoing reports the same loss.
	std::vector<std::string> warnings;
	REQUIRE(context.Redo(&warnings));
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].find("at most 256 bytes") != std::string::npos);
	CHECK(nameOf(context, id) == "Entity");
	CHECK(context.Execute("entity.get", {{"id", other}}).Result["components"]["Transform"]["Translation"] == json::array({1.0, 0.0, 0.0}));
	plant(context, id);
	REQUIRE(context.Undo());
	const CommandResult redone = context.Execute("edit.redo");
	REQUIRE(redone.Success);
	REQUIRE(redone.Result.contains("warnings"));
	CHECK(redone.Result["warnings"].size() == 1);

	// Rollback: a failed command's error also names what restoring the scene dropped.
	EditorContext third;
	const std::string lamp = CreateEntity(third, {{"name", "Lamp"}});
	plant(third, lamp);
	const CommandResult failed = third.Execute("component.set", {{"id", lamp}, {"components", {{"PointLight", {{"Range", 3}}}, {"Bogus", json::object()}}}});
	CHECK_FALSE(failed.Success);
	CHECK(failed.Error.find("unknown component 'Bogus'") != std::string::npos);
	CHECK(failed.Error.find("rolling back also dropped") != std::string::npos);
	CHECK(failed.Error.find("at most 256 bytes") != std::string::npos);
	CHECK_FALSE(third.Execute("entity.get", {{"id", lamp}}).Result["components"].contains("PointLight"));
}

TEST_CASE("EditorContext: a command during an interactive edit is its own undo step and rolls back on its own")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Start"}});
	context.ApplyInteractiveEdit(1, "Drag");
	SetNameDirectly(context, id, "Dragged");

	// A failing command (e.g. from an agent) mid-drag: its partial changes are rolled back, the drag is kept.
	CHECK_FALSE(context.Execute("entity.create", {{"name", "Half"}, {"components", {{"Camera", json::object()}, {"Bogus", json::object()}}}}).Success);
	CHECK(context.GetEditorScene().GetEntityCount() == 1);
	CHECK(NameOf(context, id) == "Dragged");

	// A succeeding command is recorded separately from the drag.
	CreateEntity(context, {{"name", "Other"}});
	CHECK_FALSE(context.IsChangeOpen());
	CHECK(context.GetUndoDescription() == "entity.create");
	REQUIRE(context.Undo());
	CHECK(context.GetEditorScene().GetEntityCount() == 1);
	CHECK(context.GetUndoDescription() == "Drag");
	REQUIRE(context.Undo());
	CHECK(NameOf(context, id) == "Start");
}

TEST_CASE("EditorContext: undo and redo commands work while an interactive edit is open")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Start"}});
	context.ApplyInteractiveEdit(1, "Typing");
	SetNameDirectly(context, id, "Typed");
	CHECK(context.Execute("edit.undo").Success);
	CHECK(NameOf(context, id) == "Start");
	CHECK(context.Execute("edit.redo").Success);
	CHECK(NameOf(context, id) == "Typed");
}

TEST_CASE("EditorContext: changing play mode commits an open interactive edit")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Start"}});
	context.ApplyInteractiveEdit(1, "Drag");
	SetNameDirectly(context, id, "Before play");
	REQUIRE(context.StartPlaying());
	CHECK_FALSE(context.IsChangeOpen());
	CHECK(context.GetUndoDescription() == "Drag");

	// A drag that continues across stopping play opens a new, recorded step on the editor scene.
	context.ApplyInteractiveEdit(1, "Drag");
	REQUIRE(context.StopPlaying());
	context.ApplyInteractiveEdit(1, "Drag");
	SetNameDirectly(context, id, "After play");
	context.CommitInteractiveEdit();
	REQUIRE(context.Undo());
	CHECK(NameOf(context, id) == "Before play");
}

TEST_CASE("EditorContext: an untouched interactive edit is kept alive only on request")
{
	EditorContext context;
	const std::string id = CreateEntity(context, {{"name", "Start"}});
	context.ApplyInteractiveEdit(1, "Colour");
	SetNameDirectly(context, id, "Picked");
	context.EndStaleInteractiveEdit();
	// Next frame: the field itself is not active, but its picker popup is.
	context.EndStaleInteractiveEdit(true);
	CHECK(context.IsChangeOpen());
	context.EndStaleInteractiveEdit();
	CHECK_FALSE(context.IsChangeOpen());
	CHECK(context.GetUndoDescription() == "Colour");
}

TEST_CASE("EditorContext: an output symlink with the right name cannot redirect a write to another file type")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("EditorContextOutputLink");
	Ref<Project> project = Project::Create(root / "Game", "Game");
	REQUIRE(project);
	{
		std::ofstream(project->GetAssetDirectory() / "Scene.pxscene") << "{}";
	}
	std::error_code error;
	std::filesystem::create_symlink(project->GetAssetDirectory() / "Scene.pxscene", project->GetDirectory() / "Shot.png", error);
	if (error)
		return; // Symlinks unavailable (e.g. Windows without developer mode).
	EditorContext context(project);
	std::string message;
	CHECK_FALSE(context.ResolveOutputPath("Shot.png", ".png", message));
}

TEST_CASE("EditorContext: a refused path is named briefly in errors and the log, however long it is")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("EditorContextLongPaths");
	Ref<Project> project = Project::Create(root / "Game", "Game");
	REQUIRE(project);
	EditorContext context(project);
	Paralax::Testing::ScopedLogCapture log;

	// A path that resolves, outside the project, too long to quote whole: named by its start and its length. (It
	// stays short enough for Windows without long paths, where a longer one would fail to resolve instead.)
	const std::string escaping = "../" + std::string(120, 'd');
	std::string error;
	CHECK_FALSE(context.ResolveProjectPath(FromUtf8(escaping + "/A.pxscene"), error));
	CHECK(error.starts_with("'../ddd"));
	CHECK(error.find(fmt::format("...' ({} bytes) is outside the project's asset directory", escaping.size() + 10)) != std::string::npos);
	CHECK(error.size() < 200);
	CHECK_FALSE(context.ResolveOutputPath(FromUtf8(escaping + "/A.png"), ".png", error));
	CHECK(error.find(fmt::format("...' ({} bytes) is outside the project directory", escaping.size() + 6)) != std::string::npos);
	CHECK(error.size() < 200);
	// Control characters in a path cannot break the message's line.
	CHECK_FALSE(context.ResolveProjectPath(FromUtf8("../Line\nBreak.pxscene"), error));
	CHECK(error == "'../Line\\nBreak.pxscene' is outside the project's asset directory");

	// A 16 MiB path sent to scene.open and scene.save is refused, and what is logged about it stays short.
	const std::string huge(16 * 1024 * 1024, 'p');
	CHECK_FALSE(context.Execute("scene.open", {{"path", huge}}).Success);
	CHECK_FALSE(context.Execute("scene.save", {{"path", huge}}).Success);
	CHECK_FALSE(context.ResolveProjectPath(FromUtf8(huge), error));
	CHECK(error.size() < 200);
	CHECK(log.Contains(spdlog::level::err, "Cannot open scene: "));
	CHECK(log.Contains(spdlog::level::err, "Cannot save scene: "));
	for (const Paralax::Testing::ScopedLogCapture::Entry& entry : log.GetEntries())
		CHECK(entry.Message.size() < 200);
}
