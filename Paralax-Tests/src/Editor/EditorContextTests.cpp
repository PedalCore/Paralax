#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

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
	CHECK_FALSE(context.Execute("scene.save", {{"path", (root / "Absolute").string()}}).Success);
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
