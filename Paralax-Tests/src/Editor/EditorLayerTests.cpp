#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <EditorCore/ConsoleSink.h>
#include <EditorCore/EditorContext.h>
#include <EditorLayer.h>

#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Core/Application.h>
#include <Paralax/Core/PathEncoding.h>

#include <limits>
#include <memory>
#include <set>
#include <string>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	std::set<std::string> CommandNames(EditorContext& context)
	{
		std::set<std::string> names;
		for (const CommandDescriptor* command : context.GetCommands().GetAll())
			names.insert(command->Name);
		return names;
	}

	json Run(EditorContext& context, std::string_view command, const json& params = json::object())
	{
		const CommandResult result = context.Execute(command, params);
		REQUIRE_MESSAGE(result.Success, (std::string(command) + ": " + result.Error));
		return result.Result;
	}

	// The editor layer attached to a headless application with a graphics device, as the editor runs it (frames are
	// not run, so only commands render). Members are destroyed in reverse order: the application first, which
	// detaches the layer while the context it refers to is still alive.
	struct AttachedEditorLayer
	{
		AttachedEditorLayer()
		{
			ApplicationSpecification specification;
			specification.Window.Width = 320;
			specification.Window.Height = 200;
			specification.Window.Headless = true;
			specification.FixedTimestep = 1.0f / 60.0f;
			specification.Graphics.Enabled = true;
			specification.Graphics.Validation = true;
			App = CreateScope<Application>(specification);
			REQUIRE(App->IsInitialized());
			CommandsBefore = CommandNames(Context);
			Layer = App->PushLayer<EditorLayer>(Context, Sink);
		}

		EditorContext Context;
		std::shared_ptr<ConsoleSink> Sink = std::make_shared<ConsoleSink>();
		std::set<std::string> CommandsBefore;
		Scope<Application> App;
		EditorLayer* Layer = nullptr;
	};

	// GPU tests create their own device: only one may exist at a time.
	bool RequireOwnGraphicsDevice()
	{
		Paralax::Testing::ReleaseTestGraphicsDevice();
		if (!Paralax::Testing::RequireTestGraphicsDevice())
			return false;
		Paralax::Testing::ReleaseTestGraphicsDevice();
		return true;
	}

}

TEST_CASE("EditorLayer: registers its commands while attached and unregisters every one of them when detached")
{
	if (!RequireOwnGraphicsDevice())
		return;
	AttachedEditorLayer editor;

	std::set<std::string> added;
	for (const std::string& name : CommandNames(editor.Context))
	{
		if (!editor.CommandsBefore.contains(name))
			added.insert(name);
	}
	CHECK(added == std::set<std::string>{"editor.camera.focus", "editor.camera.get", "editor.camera.set", "editor.gizmo.get", "editor.gizmo.set", "render.settings.get",
										 "render.settings.set", "render.stats", "render.gi.state", "render.gi.reset", "render.gi.step", "render.gi.inspect"});
	CHECK(editor.Context.GetCaptureHandler());

	editor.App->PopLayer(editor.Layer);
	editor.Layer = nullptr;
	CHECK(CommandNames(editor.Context) == editor.CommandsBefore);
	CHECK_FALSE(editor.Context.GetCaptureHandler());
	CHECK(editor.App->GetGraphicsDevice().GetValidationErrorCount() == 0);
}

TEST_CASE("EditorLayer: editor.gizmo.get reports the operation and space editor.gizmo.set chose")
{
	if (!RequireOwnGraphicsDevice())
		return;
	AttachedEditorLayer editor;
	EditorContext& context = editor.Context;

	// The editor starts with no gizmo, in local space.
	CHECK(Run(context, "editor.gizmo.get") == json{{"operation", "none"}, {"space", "local"}});

	// Set returns the gizmo as get reports it; either part may be set alone.
	CHECK(Run(context, "editor.gizmo.set", {{"operation", "rotate"}, {"space", "world"}}) == json{{"operation", "rotate"}, {"space", "world"}});
	CHECK(Run(context, "editor.gizmo.get") == json{{"operation", "rotate"}, {"space", "world"}});
	CHECK(Run(context, "editor.gizmo.set", {{"operation", "scale"}}) == json{{"operation", "scale"}, {"space", "world"}});
	CHECK(Run(context, "editor.gizmo.set", {{"space", "local"}}) == json{{"operation", "scale"}, {"space", "local"}});
	for (const char* operation : {"none", "translate", "rotate", "scale"})
		CHECK(Run(context, "editor.gizmo.set", {{"operation", operation}})["operation"] == operation);

	// A refused change changes nothing, even the part that was valid.
	CHECK_FALSE(context.Execute("editor.gizmo.set", {{"operation", "explode"}, {"space", "world"}}).Success);
	CHECK_FALSE(context.Execute("editor.gizmo.set", {{"operation", "translate"}, {"space", "sideways"}}).Success);
	CHECK(Run(context, "editor.gizmo.get") == json{{"operation", "scale"}, {"space", "local"}});
	CHECK_FALSE(context.Execute("editor.gizmo.get", {{"operation", "scale"}}).Success); // It takes no parameters.
}

TEST_CASE("EditorLayer: render.stats reports the statistics of the latest frame the renderer drew")
{
	if (!RequireOwnGraphicsDevice())
		return;
	AttachedEditorLayer editor;
	EditorContext& context = editor.Context;

	// Nothing has been rendered yet.
	const json empty = {{"drawCalls", 0}, {"shadowDrawCalls", 0}, {"triangles", 0}, {"culledSubmeshes", 0}, {"textGlyphs", 0}, {"lights", 0}, {"staticLightingGenerations", 0}, {"requestedLocalShadows", 0}, {"shadowedLocalLights", 0}, {"localShadowLayers", 0}, {"localShadowCapacity", 0}, {"localShadowDrawCalls", 0}, {"localShadowAllocationAttempts", 0}, {"shadowedLocalLightIDs", json::array()}};
	CHECK(Run(context, "render.stats") == empty);

	// A cube in view, one behind the camera (culled, but still drawn into the shadow map), a sun and a point light,
	// rendered once by a capture through the editor camera.
	const std::string cube = std::to_string(AssetManager::BuiltinCube);
	Run(context, "entity.create", {{"name", "Seen"}, {"components", {{"Mesh", {{"Mesh", cube}}}}}});
	Run(context, "entity.create", {{"name", "Behind"}, {"components", {{"Mesh", {{"Mesh", cube}}}, {"Transform", {{"Translation", {0, 0, 40}}}}}}});
	Run(context, "entity.create", {{"name", "Sun"}, {"components", {{"DirectionalLight", json::object()}}}});
	Run(context, "entity.create", {{"name", "Lamp"}, {"components", {{"PointLight", json::object()}, {"Transform", {{"Translation", {0, 2, 0}}}}}}});
	Run(context, "editor.camera.set", {{"position", {0, 1, 4}}, {"target", {0, 0, 0}}});
	const std::filesystem::path image = Paralax::Testing::GetTestOutputDirectory() / "EditorLayerStats.png";
	const json capture = Run(context, "render.capture", {{"path", ToUtf8(image)}, {"width", 64}, {"height", 64}, {"camera", "editor"}});

	Ref<MeshAsset> mesh = context.GetAssets().GetAsset<MeshAsset>(AssetHandle(AssetManager::BuiltinCube));
	REQUIRE(mesh);
	const json stats = Run(context, "render.stats");
	CHECK(capture["stats"] == stats);
	CHECK(stats["drawCalls"] == 1);
	CHECK(stats["triangles"] == mesh->Indices.size() / 3);
	CHECK(stats["culledSubmeshes"] == 1);
	CHECK(stats["shadowDrawCalls"].get<uint64_t>() >= 1);
	CHECK(stats["lights"] == 1); // Point and spot lights; the sun is not counted.
	CHECK(stats["textGlyphs"] == 0);
	CHECK(stats.size() == empty.size());

	// The statistics are the latest frame's, not totals: an empty scene renders nothing.
	Run(context, "scene.new");
	Run(context, "render.capture", {{"path", ToUtf8(image)}, {"width", 64}, {"height", 64}, {"camera", "editor"}});
	const json emptied = Run(context, "render.stats");
	CHECK(emptied["drawCalls"] == 0);
	CHECK(emptied["triangles"] == 0);
	CHECK(emptied["lights"] == 0);
	CHECK(editor.App->GetGraphicsDevice().GetValidationErrorCount() == 0);
}

TEST_CASE("EditorLayer: live play captures succeed while positive GI warmup requires paused simulation")
{
	if (!RequireOwnGraphicsDevice())
		return;
	AttachedEditorLayer editor;
	EditorContext& context = editor.Context;
	Run(context, "render.settings.set", {{"settings", {{"GIFreeze", true}, {"GIRaysPerProbe", 32}}}});
	const std::filesystem::path image = Paralax::Testing::GetTestOutputDirectory() / "EditorLayerPlaying.png";
	const json capture = {{"path", ToUtf8(image)}, {"width", 64}, {"height", 64}, {"camera", "editor"}};
	for (bool withVolume : {false, true})
	{
		CAPTURE(withVolume);
		if (withVolume)
			Run(context, "entity.create", {{"name", "GI"}, {"components", {{"GIVolume", {{"ProbeCountX", 2}, {"ProbeCountY", 2}, {"ProbeCountZ", 2}}}}}});
		Run(context, "play.start", {{"paused", false}});
		CHECK(Run(context, "render.capture", capture)["width"] == 64);
		json zeroWarmup = capture;
		zeroWarmup["giWarmupFrames"] = 0;
		CHECK(Run(context, "render.capture", zeroWarmup)["height"] == 64);
		CHECK(context.IsPlaying());
		CHECK_FALSE(context.IsPaused());

		json warmup = capture;
		warmup["giWarmupFrames"] = 1;
		const auto refused = context.Execute("render.capture", warmup);
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("Pause play mode") != std::string::npos);
		Run(context, "play.pause");
		if (withVolume && Run(context, "render.gi.state")["supported"].get<bool>())
		{
			const uint32_t before = Run(context, "render.gi.state")["updates"];
			Run(context, "render.capture", warmup);
			CHECK(Run(context, "render.gi.state")["updates"] == before + 1);
		}
		else
			CHECK_FALSE(context.Execute("render.capture", warmup).Success);
		Run(context, "play.stop");
	}

	for (const json& invalid : {json(-1), json(1025), json(std::numeric_limits<uint64_t>::max()), json(1.5), json(true), json("1"), json(nullptr)})
	{
		CAPTURE(invalid);
		json params = capture;
		params["giWarmupFrames"] = invalid;
		CHECK_FALSE(context.Execute("render.capture", params).Success);
	}
	CHECK(editor.App->GetGraphicsDevice().GetValidationErrorCount() == 0);
}
