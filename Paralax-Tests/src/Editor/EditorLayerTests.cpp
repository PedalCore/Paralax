#include <doctest/doctest.h>

#include "GPUTestContext.h"

#include <EditorCore/ConsoleSink.h>
#include <EditorCore/EditorContext.h>
#include <EditorLayer.h>

#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Core/Application.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Renderer/Image.h>

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
		explicit AttachedEditorLayer(bool viewport = false)
		{
			ApplicationSpecification specification;
			specification.Window.Width = 320;
			specification.Window.Height = 200;
			specification.Window.Headless = true;
			specification.FixedTimestep = 1.0f / 60.0f;
			specification.Graphics.Enabled = true;
			specification.Graphics.Validation = true;
			specification.ImGui.Enabled = viewport;
			specification.MaxFrames = 4;
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
										 "render.settings.set", "render.stats", "render.gi.state", "render.gi.reset", "render.gi.step", "render.gi.inspect", "render.aa.state", "render.aa.reset"});
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
	const json empty = {{"antiAliasing", {{"requested", "Off"}, {"effective", "Off"}, {"historyValid", false}, {"completedFrames", 0}, {"sampleIndex", 0}, {"resetReason", ""}, {"fallbackReason", ""}, {"passes", 0}}}, {"requestedRenderScale", 1.0}, {"effectiveRenderScale", 1.0}, {"internalSize", {0, 0}}, {"outputSize", {0, 0}}, {"scaleFallbackReason", ""}, {"nativeDepthPasses", 0}, {"upscaler", "None"}, {"profiling", {{"enabled", false}, {"status", "disabled"}, {"pending", 0}, {"skipped", 0}, {"invalidSamples", 0}, {"lastResult", "none"}, {"cpuRenderMs", nullptr}, {"gpu", nullptr}, {"device", nullptr}}}, {"bloomActive", false}, {"bloomFallbackReason", ""}, {"bloomPasses", 0}, {"ssaoActive", false}, {"ssaoFallbackReason", ""}, {"ssaoPasses", 0}, {"effectOwnedBytes", 0}, {"effectPeakBytes", 0}, {"effectAllocationAttempts", 0}, {"visibilityResolveDrawCalls", 0}, {"visibilityOwnedBytes", 0}, {"visibilityGeometryBytes", 0}, {"visibilityAllocationAttempts", 0}, {"requestedRenderPath", "Forward"}, {"effectiveRenderPath", "Forward"}, {"renderPathFallbackReason", ""}, {"surfaceOwnedBytes", 0}, {"surfaceAllocationAttempts", 0}, {"surfaceResolveDrawCalls", 0}, {"drawCalls", 0}, {"shadowDrawCalls", 0}, {"triangles", 0}, {"culledSubmeshes", 0}, {"textGlyphs", 0}, {"lights", 0}, {"staticLightingGenerations", 0}, {"requestedLocalShadows", 0}, {"shadowedLocalLights", 0}, {"localShadowLayers", 0}, {"localShadowCapacity", 0}, {"localShadowDrawCalls", 0}, {"localShadowAllocationAttempts", 0}, {"shadowedLocalLightIDs", json::array()}};
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

	Run(context, "render.settings.set", {{"settings", {{"RenderPath", "Deferred"}}}});
	CHECK(Run(context, "render.settings.get")["RenderPath"] == "Deferred");
	CHECK_FALSE(context.Execute("render.settings.set", {{"settings", {{"RenderPath", "Unknown"}}}}).Success);
	Run(context, "render.capture", {{"path", ToUtf8(image)}, {"width", 64}, {"height", 64}, {"camera", "editor"}});
	const json deferred = Run(context, "render.stats");
	CHECK(deferred["requestedRenderPath"] == "Deferred");
	CHECK(deferred["effectiveRenderPath"] == "Deferred");
	CHECK(deferred["renderPathFallbackReason"] == "");
	CHECK(deferred["surfaceOwnedBytes"].get<uint64_t>() >= 64 * 64 * 24);
	CHECK(deferred["surfaceResolveDrawCalls"] == 1);
	Run(context, "render.settings.set", {{"settings", {{"RenderPath", "Visibility"}}}});
	CHECK(Run(context, "render.settings.get")["RenderPath"] == "Visibility");
	Run(context, "render.capture", {{"path", ToUtf8(image)}, {"width", 64}, {"height", 64}, {"camera", "editor"}});
	const json visibility = Run(context, "render.stats");
	CHECK(visibility["requestedRenderPath"] == "Visibility");
	CHECK(visibility["effectiveRenderPath"] == "Visibility");
	CHECK(visibility["renderPathFallbackReason"] == "");
	CHECK(visibility["visibilityOwnedBytes"].get<uint64_t>() >= 64 * 64 * 4);
	CHECK(visibility["visibilityGeometryBytes"].get<uint64_t>() > 0);
	CHECK(visibility["visibilityResolveDrawCalls"] == 1);
	CHECK(visibility["surfaceResolveDrawCalls"] == 1);
	Run(context, "render.settings.set", {{"settings", {{"RenderPath", "Forward"}}}});

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

TEST_CASE("EditorLayer: ambient effect settings are reflected and invalid updates are atomic")
{
	if (!RequireOwnGraphicsDevice())
		return;
	AttachedEditorLayer editor;
	auto& context = editor.Context;
	const auto defaults = Run(context, "render.settings.get");
	CHECK(defaults["SSAOEnabled"] == false);
	CHECK(defaults["AmbientFillIntensity"] == 0);
	CHECK(defaults["InternalRenderScale"] == 1);
	CHECK(defaults["AntiAliasing"] == "Off");
	CHECK(defaults["AAHistoryWeight"] == doctest::Approx(0.9));
	Run(context, "render.settings.set", {{"settings", {{"SSAOEnabled", true}, {"SSAORadius", 0.75}, {"SSAOBias", 0.03}, {"SSAOStrength", 2.0}, {"SSAOQuality", "High"}, {"AmbientFillColor", {0.2, 0.4, 0.8}}, {"AmbientFillIntensity", 0.5}, {"BloomEnabled", true}, {"BloomIntensity", 0.25}, {"BloomLevels", 4}, {"BloomThreshold", 2.0}, {"BloomSoftKnee", 0.25}, {"ProfilingEnabled", true}, {"InternalRenderScale", 0.75}, {"AntiAliasing", "Temporal"}, {"AAHistoryWeight", 0.8}}}});
	const auto valid = Run(context, "render.settings.get");
	CHECK(valid["SSAOEnabled"] == true);
	CHECK(valid["SSAOQuality"] == "High");
	CHECK(valid["BloomLevels"] == 4);
	CHECK(valid["ProfilingEnabled"] == true);
	CHECK(valid["InternalRenderScale"] == 0.75);
	for (const auto& invalid : {json{{"AntiAliasing", "Unknown"}}, json{{"AAHistoryWeight", -0.1}}, json{{"AAHistoryWeight", 0.96}}, json{{"SSAORadius", -1}}, json{{"SSAOBias", 2}}, json{{"SSAOStrength", 10}}, json{{"SSAOQuality", "Ultra"}},
								json{{"BloomIntensity", 3}}, json{{"BloomLevels", 7}}, json{{"BloomSoftKnee", -1}}, json{{"BloomThreshold", 65}}, json{{"ProfilingEnabled", "yes"}}, json{{"InternalRenderScale", 0.49}}, json{{"InternalRenderScale", 1.01}}, json{{"InternalRenderScale", "half"}},
								json{{"AmbientFillIntensity", -1}}, json{{"AmbientFillColor", {1, 2}}}, json{{"SSAOEnabled", "yes"}}})
	{
		CHECK_FALSE(context.Execute("render.settings.set", {{"settings", invalid}}).Success);
		CHECK(Run(context, "render.settings.get") == valid);
	}
}

TEST_CASE("EditorLayer: temporal captures repeat from fresh history and validate accumulation without changing viewport state")
{
	if (!RequireOwnGraphicsDevice())
		return;
	AttachedEditorLayer editor;
	auto& context = editor.Context;
	Run(context, "entity.create", {{"name", "Cube"}, {"components", {{"Mesh", {{"Mesh", std::to_string(AssetManager::BuiltinCube)}}}}}});
	Run(context, "editor.camera.set", {{"position", {0, 1, 4}}, {"target", {0, 0, 0}}});
	const auto image = Testing::GetTestOutputDirectory() / "TemporalCapture.png";
	json params = {{"path", ToUtf8(image)}, {"width", 33}, {"height", 25}, {"camera", "editor"}, {"temporalFrames", 3}};
	CHECK_FALSE(context.Execute("render.capture", params).Success);
	Run(context, "render.settings.set", {{"settings", {{"AntiAliasing", "Temporal"}, {"GIEnabled", false}, {"Shadows", false}, {"AmbientFillIntensity", 0.3}}}});
	const auto before = Run(context, "render.aa.state");
	const auto first = Run(context, "render.capture", params);
	CHECK(first["stats"]["antiAliasing"]["effective"] == "Temporal");
	CHECK(first["stats"]["antiAliasing"]["completedFrames"] == 3);
	CHECK(first["stats"]["antiAliasing"]["sampleIndex"] == 2);
	const auto pixels = Image::LoadFromFile(image);
	REQUIRE_FALSE(pixels.IsEmpty());
	Run(context, "render.capture", params);
	CHECK(Image::MaxChannelDifference(pixels, Image::LoadFromFile(image)) == 0);
	CHECK(Run(context, "render.aa.state") == before);
	for (const json& invalid : {json(0), json(65), json(std::numeric_limits<uint64_t>::max()), json(1.5), json(true), json("3"), json(nullptr)})
	{
		CAPTURE(invalid);
		auto hostile = params;
		hostile["temporalFrames"] = invalid;
		CHECK_FALSE(context.Execute("render.capture", hostile).Success);
	}
	Run(context, "render.settings.set", {{"settings", {{"GIEnabled", true}, {"GIFreeze", false}}}});
	CHECK_FALSE(context.Execute("render.capture", params).Success);
	Run(context, "render.settings.set", {{"settings", {{"GIEnabled", false}}}});
	Run(context, "play.start", {{"paused", false}});
	CHECK_FALSE(context.Execute("render.capture", params).Success);
	Run(context, "play.pause");
	CHECK(Run(context, "render.capture", params)["stats"]["antiAliasing"]["completedFrames"] == 3);
	Run(context, "play.stop");
	const auto reset = Run(context, "render.aa.reset");
	CHECK(reset["historyValid"] == false);
	CHECK(reset["sampleIndex"] == 0);
	CHECK(reset["resetReason"] == "explicit reset");
	CHECK_FALSE(context.Execute("render.aa.reset", {{"unknown", true}}).Success);
	CHECK(editor.App->GetGraphicsDevice().GetValidationErrorCount() == 0);
}

TEST_CASE("EditorLayer: GI warmup capture preserves active viewport temporal history and restores AA settings")
{
	if (!RequireOwnGraphicsDevice())
		return;
	AttachedEditorLayer editor(true);
	auto& context = editor.Context;
	Run(context, "entity.create", {{"name", "Cube"}, {"components", {{"Mesh", {{"Mesh", std::to_string(AssetManager::BuiltinCube)}}}}}});
	Run(context, "entity.create", {{"name", "GI"}, {"components", {{"GIVolume", {{"ProbeCountX", 2}, {"ProbeCountY", 2}, {"ProbeCountZ", 2}}}}}});
	Run(context, "render.settings.set", {{"settings", {{"AntiAliasing", "Temporal"}, {"GIFreeze", true}, {"GIRaysPerProbe", 32}, {"Shadows", false}}}});
	editor.App->Run();
	const auto viewport = Run(context, "render.aa.state");
	REQUIRE(viewport["historyValid"] == true);
	REQUIRE(viewport["completedFrames"].get<uint64_t>() > 0);
	const auto updates = Run(context, "render.gi.state")["updates"].get<uint32_t>();
	const auto image = Testing::GetTestOutputDirectory() / "TemporalWarmup.png";
	const auto capture = Run(context, "render.capture", {{"path", ToUtf8(image)}, {"width", 33}, {"height", 25}, {"camera", "editor"}, {"giWarmupFrames", 2}, {"temporalFrames", 3}});
	CHECK(capture["stats"]["antiAliasing"]["effective"] == "Temporal");
	CHECK(capture["stats"]["antiAliasing"]["completedFrames"] == 3);
	CHECK(Run(context, "render.aa.state") == viewport);
	CHECK(Run(context, "render.settings.get")["AntiAliasing"] == "Temporal");
	CHECK(Run(context, "render.gi.state")["updates"] == updates + 2);
	Run(context, "render.aa.reset");
	CHECK(Run(context, "render.aa.state")["historyValid"] == false);
	CHECK(Run(context, "render.aa.state")["completedFrames"] == viewport["completedFrames"]);
	// Undo replaces the Scene object. The next viewport render must reject the old strong scene token.
	editor.Layer->OnRender();
	REQUIRE(Run(context, "render.aa.state")["historyValid"] == true);
	Run(context, "entity.create", {{"name", "Undo replacement"}});
	Run(context, "edit.undo");
	editor.Layer->OnRender();
	CHECK(Run(context, "render.aa.state")["resetReason"] == "scene replaced");
	CHECK(Run(context, "render.aa.state")["sampleIndex"] == 1);
	CHECK(editor.App->GetGraphicsDevice().GetValidationErrorCount() == 0);
}
