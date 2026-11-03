#include "EditorLayer.h"

#include "EditorCore/FocusBounds.h"
#include "Panels/FieldWidgets.h"

#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Core/Application.h>
#include <Paralax/Core/Input.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Events/KeyEvent.h>
#include <Paralax/Events/MouseEvent.h>
#include <Paralax/ImGui/ImGuiLayer.h>
#include <Paralax/ImGui/ImGuiRenderer.h>
#include <Paralax/Math/Math.h>
#include <Paralax/Renderer/GraphicsDevice.h>
#include <Paralax/Renderer/TextureReadback.h>
#include <Paralax/Scene/Entity.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
// ImGuizmo.h relies on imgui.h being included first.
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		const char* AAName(AntiAliasingMode mode)
		{
			switch (mode)
			{
				case AntiAliasingMode::Off: return "Off";
				case AntiAliasingMode::Spatial: return "Spatial";
				case AntiAliasingMode::Temporal: return "Temporal";
			}
			return "Invalid";
		}

		json ViewportAAJson(SceneRenderer& renderer)
		{
			const auto& history = renderer.GetDefaultTemporalHistory();
			const auto& state = history.GetState();
			return {{"requested", AAName(renderer.GetSettings().AntiAliasing)}, {"effective", AAName(history.GetEffectiveMode())}, {"historyValid", state.IsValid()}, {"completedFrames", state.GetCompletedFrames()}, {"sampleIndex", state.GetSampleIndex()}, {"resetReason", state.GetResetReason()}, {"fallbackReason", history.GetRenderFallbackReason()}, {"temporalOwnedBytes", history.GetOwnedBytes()}, {"passes", history.GetPassCount()}};
		}

		json RenderStatsJson(const SceneRenderStats& stats)
		{
			json ids = json::array();
			for (uint64_t id : stats.ShadowedLocalLightIDs)
				ids.push_back(std::to_string(id));
			const auto pathName = [](RenderPath path) {
				switch (path)
				{
					case RenderPath::Forward: return "Forward";
					case RenderPath::Deferred: return "Deferred";
					case RenderPath::Visibility: return "Visibility";
				}
				return "Invalid";
			};
			json profiling = {{"enabled", stats.ProfilingEnabled}, {"status", stats.GPUProfilingStatus}, {"pending", stats.GPUProfilingPending}, {"skipped", stats.GPUProfilingSkipped}, {"invalidSamples", stats.GPUProfilingInvalid}, {"lastResult", stats.GPUProfilingLastResult}, {"cpuRenderMs", stats.CPURenderMilliseconds ? json(*stats.CPURenderMilliseconds) : json(nullptr)}, {"gpu", nullptr}, {"device", nullptr}};
			if (stats.GPUTiming)
			{
				const auto& sample = *stats.GPUTiming;
				const auto& context = sample.Context;
				json sections = json::object();
				for (size_t i = 0; i < sample.Milliseconds.size(); ++i)
					sections[RenderProfiler::GetSectionName(static_cast<RenderTimingSection>(i))] = sample.Milliseconds[i] ? json(*sample.Milliseconds[i]) : json(nullptr);
				profiling["gpu"] = {{"frame", context.Frame}, {"internalSize", {context.InternalWidth, context.InternalHeight}}, {"outputSize", {context.OutputWidth, context.OutputHeight}}, {"requestedRenderPath", pathName(static_cast<RenderPath>(context.RequestedPath))}, {"effectiveRenderPath", pathName(static_cast<RenderPath>(context.EffectivePath))}, {"ssaoActive", context.AmbientOcclusionActive}, {"bloomActive", context.BloomActive}, {"effectiveAA", AAName(static_cast<AntiAliasingMode>(context.EffectiveAA))}, {"settings", json::parse(context.Settings, nullptr, false)}, {"milliseconds", std::move(sections)}, {"scope", "SceneRenderer commands; excludes presentation. Scene includes AO; do not sum nested sections."}};
			}
			if (stats.ProfilingDevice)
			{
				const auto& device = *stats.ProfilingDevice;
				profiling["device"] = {{"adapter", device.AdapterName}, {"driver", device.DriverName}, {"backend", "Vulkan/NVRHI"}, {"build", PX_BUILD_CONFIGURATION_NAME}, {"software", device.IsSoftwareRasterizer}, {"validation", device.ValidationEnabled}, {"synchronizationValidation", device.SynchronizationValidation}};
			}
			return {{"antiAliasing", {{"requested", AAName(stats.RequestedAA)}, {"effective", AAName(stats.EffectiveAA)}, {"historyValid", stats.AAHistoryValid}, {"completedFrames", stats.AACompletedFrames}, {"sampleIndex", stats.AASampleIndex}, {"resetReason", stats.AAResetReason}, {"fallbackReason", stats.AAFallbackReason}, {"passes", stats.AAPasses}}}, {"requestedRenderScale", stats.RequestedRenderScale}, {"effectiveRenderScale", stats.EffectiveRenderScale}, {"internalSize", {stats.InternalWidth, stats.InternalHeight}}, {"outputSize", {stats.OutputWidth, stats.OutputHeight}}, {"scaleFallbackReason", stats.ScaleFallbackReason}, {"nativeDepthPasses", stats.NativeDepthPasses}, {"upscaler", stats.InternalWidth > stats.OutputWidth || stats.InternalHeight > stats.OutputHeight ? "AreaHDR" : stats.InternalWidth != stats.OutputWidth || stats.InternalHeight != stats.OutputHeight ? "SpatialBilinearHDR"
																																																																																																																																																																																																																													 : "None"},
					{"profiling", std::move(profiling)},
					{"requestedRenderPath", pathName(stats.RequestedPath)},
					{"effectiveRenderPath", pathName(stats.EffectivePath)},
					{"renderPathFallbackReason", stats.PathFallbackReason},
					{"bloomActive", stats.BloomActive},
					{"bloomFallbackReason", stats.BloomFallbackReason},
					{"bloomPasses", stats.BloomPasses},
					{"ssaoActive", stats.SSAOActive},
					{"ssaoFallbackReason", stats.SSAOFallbackReason},
					{"ssaoPasses", stats.SSAOPasses},
					{"effectOwnedBytes", stats.EffectOwnedBytes},
					{"effectPeakBytes", stats.EffectPeakBytes},
					{"effectAllocationAttempts", stats.EffectAllocationAttempts},
					{"surfaceOwnedBytes", stats.SurfaceOwnedBytes},
					{"surfaceAllocationAttempts", stats.SurfaceAllocationAttempts},
					{"surfaceResolveDrawCalls", stats.SurfaceResolveDrawCalls},
					{"visibilityResolveDrawCalls", stats.VisibilityResolveDrawCalls},
					{"visibilityOwnedBytes", stats.VisibilityOwnedBytes},
					{"visibilityGeometryBytes", stats.VisibilityGeometryBytes},
					{"visibilityAllocationAttempts", stats.VisibilityAllocationAttempts},
					{"drawCalls", stats.DrawCalls},
					{"shadowDrawCalls", stats.ShadowDrawCalls},
					{"triangles", stats.Triangles},
					{"culledSubmeshes", stats.CulledSubmeshes},
					{"textGlyphs", stats.TextGlyphs},
					{"lights", stats.Lights},
					{"staticLightingGenerations", stats.StaticLightingGenerations},
					{"requestedLocalShadows", stats.RequestedLocalShadows},
					{"shadowedLocalLights", stats.ShadowedLocalLights},
					{"localShadowLayers", stats.LocalShadowLayers},
					{"localShadowCapacity", stats.LocalShadowCapacity},
					{"localShadowDrawCalls", stats.LocalShadowDrawCalls},
					{"localShadowAllocationAttempts", stats.LocalShadowAllocationAttempts},
					{"shadowedLocalLightIDs", std::move(ids)}};
		}

		std::string IdString(UUID id)
		{
			return std::to_string(static_cast<uint64_t>(id));
		}

		nvrhi::TextureDesc ColorTargetDesc(uint32_t width, uint32_t height, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.debugName = name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.setClearValue(nvrhi::Color(0.0f));
			return desc;
		}

		// Ray-AABB slab test; returns the entry distance or a negative value on a miss.
		float RayBoxDistance(const glm::vec3& origin, const glm::vec3& direction, const AABB& box)
		{
			float tMin = 0.0f;
			float tMax = std::numeric_limits<float>::max();
			for (int axis = 0; axis < 3; axis++)
			{
				if (std::abs(direction[axis]) < 1e-8f)
				{
					if (origin[axis] < box.Min[axis] || origin[axis] > box.Max[axis])
						return -1.0f;
					continue;
				}
				float t1 = (box.Min[axis] - origin[axis]) / direction[axis];
				float t2 = (box.Max[axis] - origin[axis]) / direction[axis];
				if (t1 > t2)
					std::swap(t1, t2);
				tMin = std::max(tMin, t1);
				tMax = std::min(tMax, t2);
				if (tMin > tMax)
					return -1.0f;
			}
			return tMin;
		}

		// The gizmo operations as editor.gizmo.get reports them and editor.gizmo.set takes them; 0 shows no gizmo.
		// These are the only values the editor's toolbar, shortcuts and commands set.
		constexpr std::pair<std::string_view, int> GizmoOperations[] = {
			{"none", 0},
			{"translate", ImGuizmo::TRANSLATE},
			{"rotate", ImGuizmo::ROTATE},
			{"scale", ImGuizmo::SCALE},
		};

	}

	EditorLayer::EditorLayer(EditorContext& context, std::shared_ptr<ConsoleSink> consoleSink)
		: Layer("EditorLayer")
		, m_Context(context)
		, m_ConsoleSink(std::move(consoleSink))
		, m_Console(m_ConsoleSink)
	{
	}

	EditorLayer::~EditorLayer() = default;

	void EditorLayer::OnAttach()
	{
		Application& app = Application::Get();
		m_Renderer = CreateScope<SceneRenderer>(app.GetGraphicsDevice(), m_Context.GetAssets());
		if (!m_Renderer->Initialize())
		{
			PX_CORE_ERROR("Failed to initialise the scene renderer; the viewport will be empty");
			m_Renderer.reset();
		}
		m_Context.SetCaptureHandler([this](const json& params) { return Capture(params); });
		RegisterCommands();
	}

	void EditorLayer::OnDetach()
	{
		m_Context.SetCaptureHandler({});
		for (const char* command : s_LayerCommands)
			m_Context.GetCommands().Unregister(command);
		m_Context.CommitInteractiveEdit();
		Application::Get().GetGraphicsDevice().WaitForIdle();
		m_ViewportFramebuffer = nullptr;
		m_ViewportTexture = nullptr;
		m_Renderer.reset();
	}

	void EditorLayer::RegisterCommands()
	{
		CommandRegistry& commands = m_Context.GetCommands();
		auto vec3 = [](const json& value) -> std::optional<glm::vec3> {
			if (!value.is_array() || value.size() != 3)
				return std::nullopt;
			glm::vec3 result;
			for (int i = 0; i < 3; i++)
			{
				if (!value[static_cast<size_t>(i)].is_number())
					return std::nullopt;
				result[i] = value[static_cast<size_t>(i)].get<float>();
				if (!std::isfinite(result[i]))
					return std::nullopt;
			}
			return result;
		};
		auto describeCamera = [this] {
			const glm::vec3 position = m_Camera.GetPosition();
			const glm::vec3 target = m_Camera.GetFocalPoint();
			return json{{"position", {position.x, position.y, position.z}}, {"target", {target.x, target.y, target.z}}, {"verticalFov", glm::degrees(m_Camera.GetVerticalFOV())}};
		};

		commands.Register({"editor.camera.get", "The editor viewport camera (position, orbit target, vertical field of view in degrees).", {}, false, [describeCamera](EditorContext&, const json&) { return CommandResult::Ok(describeCamera()); }});

		commands.Register({"editor.camera.set", "Places the editor camera at 'position' looking at 'target' (render.capture camera \"editor\" uses it).", {{"position", "array", "[x, y, z] in meters.", true}, {"target", "array", "[x, y, z] to look at.", true}}, false, [this, vec3, describeCamera](EditorContext&, const json& params) {
							   const std::optional<glm::vec3> position = vec3(params["position"]);
							   const std::optional<glm::vec3> target = vec3(params["target"]);
							   if (!position || !target)
								   return CommandResult::Fail("position and target must be arrays of three finite numbers");
							   if (glm::length(*target - *position) < 1e-3f)
								   return CommandResult::Fail("position and target must differ");
							   m_Camera.LookAt(*position, *target);
							   if (m_Renderer)
								   m_Renderer->GetDefaultTemporalHistory().GetState().Reset("camera cut");
							   return CommandResult::Ok(describeCamera());
						   }});

		commands.Register({"editor.camera.focus", "Frames an entity and everything below it in the editor camera (like pressing F).", {{"id", "any", "Entity id.", true}}, false, [this, describeCamera](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return CommandResult::Fail("Entity not found");
							   FocusEntity(*entity);
							   return CommandResult::Ok(describeCamera());
						   }});

		auto describeGizmo = [this] {
			std::string_view operation = "none";
			for (const auto& [name, value] : GizmoOperations)
			{
				if (value == m_GizmoOperation)
					operation = name;
			}
			return json{{"operation", operation}, {"space", m_GizmoWorldSpace ? "world" : "local"}};
		};

		commands.Register({"editor.gizmo.get", "The viewport gizmo: operation \"none\", \"translate\", \"rotate\" or \"scale\", and space \"local\" or \"world\".", {}, false, [describeGizmo](EditorContext&, const json&) { return CommandResult::Ok(describeGizmo()); }});

		commands.Register({"editor.gizmo.set", "Sets the viewport gizmo: operation \"none\", \"translate\", \"rotate\" or \"scale\"; space \"local\" or \"world\". Returns the gizmo as editor.gizmo.get does.", {{"operation", "string", "Gizmo operation.", false}, {"space", "string", "Manipulation space.", false}}, false, [this, describeGizmo](EditorContext&, const json& params) {
							   int operation = m_GizmoOperation;
							   if (params.contains("operation"))
							   {
								   const std::string& name = params["operation"].get_ref<const std::string&>();
								   const auto* found = std::find_if(std::begin(GizmoOperations), std::end(GizmoOperations), [&](const auto& entry) { return entry.first == name; });
								   if (found == std::end(GizmoOperations))
									   return CommandResult::Fail("operation must be none, translate, rotate or scale");
								   operation = found->second;
							   }
							   bool world = m_GizmoWorldSpace;
							   if (params.contains("space"))
							   {
								   const std::string& space = params["space"].get_ref<const std::string&>();
								   if (space != "local" && space != "world")
									   return CommandResult::Fail("space must be local or world");
								   world = space == "world";
							   }
							   m_GizmoOperation = operation;
							   m_GizmoWorldSpace = world;
							   return CommandResult::Ok(describeGizmo());
						   }});

		commands.Register({"render.aa.state", "Temporal history state for the viewport, independent of captures.", {}, false, [this](EditorContext&, const json&) {
							   return m_Renderer ? CommandResult::Ok(ViewportAAJson(*m_Renderer)) : CommandResult::Fail("The renderer is not available");
						   }});
		commands.Register({"render.aa.reset", "Invalidates viewport temporal history and restarts its deterministic sample sequence.", {}, false, [this](EditorContext&, const json&) {
							   if (!m_Renderer)
								   return CommandResult::Fail("The renderer is not available");
							   m_Renderer->GetDefaultTemporalHistory().GetState().Reset("explicit reset");
							   return CommandResult::Ok(ViewportAAJson(*m_Renderer));
						   }});

		commands.Register({"render.settings.get", "The renderer settings (exposure, tonemapper, shadows, sky intensity, experimental GI, debug view).", {}, false, [this](EditorContext&, const json&) {
							   if (!m_Renderer)
								   return CommandResult::Fail("The renderer is not available");
							   return CommandResult::Ok(Reflection::ToJson(SceneRendererSettings::GetFields(), &m_Renderer->GetSettings()));
						   }});

		commands.Register({"render.settings.set", "Changes renderer settings; fields as returned by render.settings.get. Nothing changes if any field is invalid.", {{"settings", "object", "Field name -> value.", true}}, false, [this](EditorContext&, const json& params) {
							   if (!m_Renderer)
								   return CommandResult::Fail("The renderer is not available");
							   SceneRendererSettings edited = m_Renderer->GetSettings();
							   std::string error;
							   if (!Reflection::FromJson(SceneRendererSettings::GetFields(), "render.settings", &edited, params["settings"], error))
								   return CommandResult::Fail(error);
							   m_Renderer->GetSettings() = edited;
							   return CommandResult::Ok(Reflection::ToJson(SceneRendererSettings::GetFields(), &edited));
						   }});

		// What the Statistics panel shows. The renderer resets its statistics at the start of every frame it renders,
		// and both the viewport (every editor frame) and render.capture render through it.
		commands.Register({"render.stats", "Statistics of the renderer's latest frame (the viewport's, or a render.capture made since): draw calls, shadow-map draw calls, triangles, culled submeshes, text glyphs, point and spot lights, and environment-lighting regenerations.", {}, false, [this](EditorContext&, const json&) {
							   if (!m_Renderer)
								   return CommandResult::Fail("The renderer is not available");
							   const SceneRenderStats& stats = m_Renderer->GetStats();
							   return CommandResult::Ok(RenderStatsJson(stats));
						   }});

		auto giState = [this] {
			const GIState state = m_Renderer->GetGIState();
			return json{{"enabled", state.Enabled}, {"supported", state.Supported}, {"frozen", state.Frozen}, {"historyValid", state.HistoryValid}, {"updates", state.Updates}, {"queued", state.Queued}, {"origin", {state.Origin.x, state.Origin.y, state.Origin.z}}, {"spacing", state.Spacing}, {"counts", {state.Counts.x, state.Counts.y, state.Counts.z}}, {"frame", state.Frame}};
		};
		commands.Register({"render.gi.state", "Reports GI history and exact update counters without rendering.", {}, false, [this, giState](EditorContext&, const json&) {
							   return m_Renderer ? CommandResult::Ok(giState()) : CommandResult::Fail("The renderer is not available");
						   }});
		commands.Register({"render.gi.reset", "Resets GI history. The next render performs one complete initial fill, even when frozen.", {}, false, [this, giState](EditorContext& context, const json&) {
							   if (context.IsPlaying() && !context.IsPaused())
								   return CommandResult::Fail("Pause play mode before GI diagnostics");
							   if (!m_Renderer || !m_Renderer->ResetGIHistory())
								   return CommandResult::Fail("GI requires an enabled, supported volume rendered at least once");
							   return CommandResult::Ok(giState());
						   }});
		commands.Register({"render.gi.step", "Performs exactly frames GI updates without advancing simulation; requires GIFreeze.", {{"frames", "integer", "Number of updates (1..1024).", true}}, false, [this, giState](EditorContext& context, const json& params) {
							   if (context.IsPlaying() && !context.IsPaused())
								   return CommandResult::Fail("Pause play mode before GI diagnostics");
							   if (!m_Renderer || !m_ViewportFramebuffer)
								   return CommandResult::Fail("The renderer is not available");
							   if (params["frames"] < 1 || params["frames"] > 1024)
								   return CommandResult::Fail("frames must be 1..1024");
							   const uint32_t frames = params["frames"].get<uint32_t>();
							   const uint32_t before = m_Renderer->GetGIState().Updates;
							   if (!m_Renderer->QueueGIUpdates(frames))
								   return CommandResult::Fail("Set GIFreeze first; GI requires an enabled, supported volume rendered at least once");
							   const float aspect = static_cast<float>(m_ViewportWidth) / static_cast<float>(m_ViewportHeight);
							   for (uint32_t frame = 0; frame < frames; ++frame)
							   {
								   m_Renderer->Render(context.GetActiveScene(), GetViewportCamera(aspect), m_ViewportFramebuffer, &m_ShadowHistory);
								   auto& graphics = Application::Get().GetGraphicsDevice();
								   graphics.WaitForIdle();
								   graphics.CollectGarbage();
							   }
							   const GIState after = m_Renderer->GetGIState();
							   if (after.Queued != 0 || after.Updates != before + frames)
								   return CommandResult::Fail("GI did not complete the requested updates; inspect render.gi.state");
							   json result = giState();
							   result["before"] = before;
							   return CommandResult::Ok(std::move(result));
						   }});
		commands.Register({"render.gi.inspect", "Synchronous GPU readback of eight probe neighbours; stalls the GPU and does not advance history.", {{"position", "array", "World receiver position.", true}, {"normal", "array", "World surface normal.", true}, {"view", "array", "World direction from receiver to camera.", true}, {"viewDistance", "number", "Optional receiver-to-camera distance in metres; omitted means unbounded (orthographic).", false}}, false, [this, vec3](EditorContext& context, const json& params) {
							   if (context.IsPlaying() && !context.IsPaused())
								   return CommandResult::Fail("Pause play mode before GI diagnostics");
							   if (!m_Renderer)
								   return CommandResult::Fail("The renderer is not available");
							   const auto position = vec3(params["position"]), normal = vec3(params["normal"]), view = vec3(params["view"]);
							   if (!position || !normal || !view || glm::length(*normal) < 1e-6f || glm::length(*view) < 1e-6f)
								   return CommandResult::Fail("Expected finite position and nonzero normal/view vectors");
							   float viewDistance = 1000000.0f;
							   if (params.contains("viewDistance"))
							   {
								   if (!params["viewDistance"].is_number())
									   return CommandResult::Fail("viewDistance must be a finite number of metres in [0,1000000]");
								   viewDistance = params["viewDistance"].get<float>();
								   if (!std::isfinite(viewDistance) || viewDistance < 0.0f || viewDistance > 1000000.0f)
									   return CommandResult::Fail("viewDistance must be a finite number of metres in [0,1000000]");
							   }
							   const auto inspection = m_Renderer->InspectGI(*position, *normal, *view, viewDistance);
							   if (!inspection)
								   return CommandResult::Fail("No initialized GI data at this position, or GPU readback failed");
							   auto vector = [](const glm::vec4& v) { return json::array({v.x, v.y, v.z}); };
							   auto ray = [&vector](const GPU::ProbeVisibilityRay& value) {
								   return json{{"origin", vector(value.OriginLength)}, {"end", vector(value.EndOffset)}, {"length", value.OriginLength.w}, {"epsilon", value.EndOffset.w}, {"hitPosition", vector(value.HitPositionDistance)}, {"hitDistance", value.HitPositionDistance.w}, {"triangle", value.Metadata.x}, {"material", value.Metadata.y}, {"blocked", value.Metadata.z != 0}, {"backface", (value.Metadata.w & 1) != 0}, {"reliable", (value.Metadata.w & 2) == 0}};
							   };
							   bool surfaceReliable = true, biasedReliable = true;
							   json neighbours = json::array();
							   for (const auto& neighbour : inspection->Neighbours)
							   {
								   if (neighbour.Metadata.y != 0 && neighbour.Weights.x > 0.0f)
								   {
									   surfaceReliable &= (neighbour.SurfaceRay.Metadata.w & 2) == 0;
									   biasedReliable &= (neighbour.BiasedRay.Metadata.w & 2) == 0;
								   }
								   neighbours.push_back({{"index", neighbour.Metadata.x}, {"active", neighbour.Metadata.y != 0}, {"position", {neighbour.PositionDistance.x, neighbour.PositionDistance.y, neighbour.PositionDistance.z}}, {"distance", neighbour.PositionDistance.w}, {"meanDistance", neighbour.Moments.x}, {"secondMoment", neighbour.Moments.y}, {"distanceSupportMass", neighbour.Sampling.x}, {"distanceExpectedMass", neighbour.Sampling.y}, {"distanceConfidence", neighbour.Sampling.z}, {"rawVariance", neighbour.Moments.z}, {"variance", neighbour.Moments.w}, {"trilinear", neighbour.Weights.x}, {"facing", neighbour.Weights.y}, {"visibility", neighbour.Weights.z}, {"weight", neighbour.Weights.w}, {"normalizedWeight", neighbour.IrradianceWeight.w}, {"irradiance", {neighbour.IrradianceWeight.x, neighbour.IrradianceWeight.y, neighbour.IrradianceWeight.z}}, {"surfaceRay", ray(neighbour.SurfaceRay)}, {"biasedRay", ray(neighbour.BiasedRay)}});
							   }
							   return CommandResult::Ok({{"irradiance", {inspection->Irradiance.x, inspection->Irradiance.y, inspection->Irradiance.z}},
														 {"surfaceReliable", surfaceReliable},
														 {"biasedReliable", biasedReliable},
														 {"surfaceRejected", vector(inspection->SurfaceRejected)},
														 {"surfaceExact", vector(inspection->SurfaceExact)},
														 {"biasedRejected", vector(inspection->BiasedRejected)},
														 {"biasedExact", vector(inspection->BiasedExact)},
														 {"weightSum", inspection->Summary.x},
														 {"activeNeighbours", inspection->Summary.y},
														 {"neighbours", std::move(neighbours)}});
						   }});
	}

	void EditorLayer::EnsureViewportTarget(uint32_t width, uint32_t height)
	{
		if (m_ViewportTexture && width == m_ViewportWidth && height == m_ViewportHeight)
			return;
		nvrhi::IDevice* device = Application::Get().GetGraphicsDevice().GetDevice();
		m_ViewportTexture = device->createTexture(ColorTargetDesc(width, height, "EditorViewport"));
		m_ViewportFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_ViewportTexture));
		m_ViewportWidth = width;
		m_ViewportHeight = height;
		m_Camera.SetViewportSize(static_cast<float>(width), static_cast<float>(height));
	}

	bool EditorLayer::IsViewingThroughSceneCamera()
	{
		return m_Context.IsPlaying() && m_Context.GetActiveScene().GetPrimaryCameraEntity().has_value();
	}

	CameraView EditorLayer::GetViewportCamera(float aspect)
	{
		// While playing, look through the scene's primary camera (as the game would).
		if (m_Context.IsPlaying())
		{
			Scene& scene = m_Context.GetActiveScene();
			if (std::optional<Entity> camera = scene.GetPrimaryCameraEntity())
			{
				if (std::optional<CameraView> view = CameraView::FromEntity(scene, *camera, aspect))
					return *view;
			}
		}
		return m_Camera.GetView();
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		m_FrameTime = ts.GetMilliseconds();
		m_Camera.OnUpdate(ts, m_ViewportHovered);
		if (m_Context.IsPlaying())
			m_Context.TickRuntime(ts);
	}

	void EditorLayer::OnRender()
	{
		if (!m_Renderer || m_ViewportWidth == 0 || m_ViewportHeight == 0 || !m_ViewportFramebuffer)
			return;
		const float aspect = static_cast<float>(m_ViewportWidth) / static_cast<float>(m_ViewportHeight);
		m_Renderer->Render(m_Context.GetActiveScene(), GetViewportCamera(aspect), m_ViewportFramebuffer, &m_ShadowHistory);
	}

	void EditorLayer::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<MouseScrolledEvent>([this](MouseScrolledEvent& e) {
			if (!m_ViewportHovered)
				return false;
			m_Camera.OnScroll(e.GetYOffset());
			return true;
		});
	}

	CommandResult EditorLayer::Capture(const json& params)
	{
		if (!m_Renderer)
			return CommandResult::Fail("The renderer is not available");

		if (params.contains("temporalFrames") && (params["temporalFrames"] < 1 || params["temporalFrames"] > 64))
			return CommandResult::Fail("temporalFrames must be 1..64");
		const auto temporalFrames = params.value("temporalFrames", int64_t(1));
		if (temporalFrames > 1)
		{
			if (m_Renderer->GetSettings().AntiAliasing != AntiAliasingMode::Temporal)
				return CommandResult::Fail("Multiple temporalFrames require Temporal AA");
			if (m_Context.IsPlaying() && !m_Context.IsPaused())
				return CommandResult::Fail("Pause play mode before temporal capture accumulation");
			if (m_Renderer->GetSettings().GIEnabled && !m_Renderer->GetSettings().GIFreeze)
				return CommandResult::Fail("Temporal capture accumulation requires frozen or disabled GI");
		}

		uint32_t warmup = 0;
		if (params.contains("giWarmupFrames"))
		{
			if (params["giWarmupFrames"] < 0 || params["giWarmupFrames"] > 1024)
				return CommandResult::Fail("giWarmupFrames must be 0..1024");
			warmup = params["giWarmupFrames"].get<uint32_t>();
			if (warmup > 0 && m_Context.IsPlaying() && !m_Context.IsPaused())
				return CommandResult::Fail("Pause play mode before GI capture warmup");
			if (warmup > 0 && (!m_Renderer->GetGIState().Available || !m_Renderer->GetSettings().GIFreeze))
				return CommandResult::Fail("Warmup requires GIFreeze and an enabled, supported volume rendered at least once");
		}

		const int64_t width = params.value("width", int64_t(1280));
		const int64_t height = params.value("height", int64_t(720));
		if (width < 16 || height < 16 || width > 8192 || height > 8192)
			return CommandResult::Fail("width and height must be 16-8192");
		const std::string cameraMode = params.value("camera", std::string("scene"));
		std::string pathError;
		const std::optional<std::filesystem::path> path = m_Context.ResolveOutputPath(FromUtf8(params["path"].get<std::string>()), ".png", pathError);
		if (!path)
			return CommandResult::Fail(pathError);

		nvrhi::IDevice* device = Application::Get().GetGraphicsDevice().GetDevice();
		nvrhi::TextureHandle texture = device->createTexture(ColorTargetDesc(static_cast<uint32_t>(width), static_cast<uint32_t>(height), "Capture"));
		nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));

		const float aspect = static_cast<float>(width) / static_cast<float>(height);
		Scene& scene = m_Context.GetActiveScene();
		CameraView view = m_Camera.GetView();
		view.Projection = glm::perspectiveRH_ZO(m_Camera.GetVerticalFOV(), aspect, view.Far, view.Near);
		if (cameraMode == "scene")
		{
			if (std::optional<Entity> camera = scene.GetPrimaryCameraEntity())
			{
				if (std::optional<CameraView> sceneView = CameraView::FromEntity(scene, *camera, aspect))
					view = *sceneView;
			}
		}
		else if (cameraMode != "editor")
		{
			return CommandResult::Fail("camera must be \"scene\" or \"editor\"");
		}

		auto captureHistory = m_Renderer->CreateTemporalHistory();
		if (warmup > 0)
		{
			auto warmupHistory = m_Renderer->CreateTemporalHistory();
			const uint32_t before = m_Renderer->GetGIState().Updates;
			if (!m_Renderer->QueueGIUpdates(warmup))
				return CommandResult::Fail("Could not queue GI warmup updates");
			const auto savedAA = m_Renderer->GetSettings().AntiAliasing;
			m_Renderer->GetSettings().AntiAliasing = AntiAliasingMode::Off;
			for (uint32_t frame = 0; frame < warmup; ++frame)
			{
				m_Renderer->Render(scene, view, framebuffer, nullptr, warmupHistory.get());
				auto& graphics = Application::Get().GetGraphicsDevice();
				graphics.WaitForIdle();
				graphics.CollectGarbage();
			}
			m_Renderer->GetSettings().AntiAliasing = savedAA;
			const GIState after = m_Renderer->GetGIState();
			if (after.Queued != 0 || after.Updates != before + warmup)
				return CommandResult::Fail("GI did not complete warmup; inspect render.gi.state");
		}
		for (int64_t frame = 0; frame < temporalFrames; ++frame)
			m_Renderer->Render(scene, view, framebuffer, nullptr, captureHistory.get());
		Image image = ReadbackTexture(device, texture);
		std::error_code error;
		std::filesystem::create_directories(path->parent_path(), error);
		if (image.IsEmpty() || !image.SavePNG(*path))
			return CommandResult::Fail("Could not render or write the image");
		return CommandResult::Ok({{"path", ToUtf8(*path)}, {"width", width}, {"height", height}, {"stats", RenderStatsJson(m_Renderer->GetStats())}});
	}

	void EditorLayer::OnImGuiRender()
	{
		HandleShortcuts();

		ImGuiViewport* mainViewport = ImGui::GetMainViewport();
		const unsigned int dockspace = ImGui::DockSpaceOverViewport(0, mainViewport, ImGuiDockNodeFlags_PassthruCentralNode);
		if (!m_LayoutInitialized)
		{
			SetupDefaultLayout(dockspace);
			m_LayoutInitialized = true;
		}

		DrawMenuBar();
		DrawViewport();
		m_Hierarchy.Draw(m_Context);
		ImGuiLayer* imgui = Application::Get().GetImGuiLayer();
		m_Inspector.Draw(m_Context, imgui ? imgui->GetBoldFont() : nullptr);
		m_AssetBrowser.Draw(m_Context);
		m_Console.Draw(m_Context);
		if (m_ShowRenderSettings)
			DrawRenderSettings();
		if (m_ShowStatistics)
			DrawStatistics();
		if (m_ShowImGuiDemo)
			ImGui::ShowDemoWindow(&m_ShowImGuiDemo);

		// Panels hold scene references while drawing, so their structural changes run now, after all drawing.
		m_Context.ExecuteDeferred();
		// An interactive edit ends once nothing is being dragged or typed into any more. Widgets and the gizmo touch
		// their edit every active frame; any other active item (e.g. a colour picker popup of the edited field) keeps
		// it open too. This runs every frame, so an edit can never outlive its widget.
		m_Context.EndStaleInteractiveEdit(ImGui::IsAnyItemActive());
		// Mouse and keyboard reach the viewport camera only while the viewport is hovered.
		if (imgui)
			imgui->SetBlockEvents(!m_ViewportHovered);
	}

	void EditorLayer::SetupDefaultLayout(unsigned int dockspace)
	{
		// Only build the default arrangement when no saved layout exists for this dockspace.
		ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
		if (node && node->IsSplitNode())
			return;

		ImGui::DockBuilderRemoveNode(dockspace);
		ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);

		ImGuiID center = dockspace;
		ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
		ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
		ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
		ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.35f, nullptr, &right);

		ImGui::DockBuilderDockWindow("Viewport", center);
		ImGui::DockBuilderDockWindow("Hierarchy", left);
		ImGui::DockBuilderDockWindow("Inspector", right);
		ImGui::DockBuilderDockWindow("Renderer", rightBottom);
		ImGui::DockBuilderDockWindow("Statistics", rightBottom);
		ImGui::DockBuilderDockWindow("Assets", bottom);
		ImGui::DockBuilderDockWindow("Console", bottom);
		ImGui::DockBuilderFinish(dockspace);
	}

	void EditorLayer::HandleShortcuts()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput)
			return;
		const bool ctrl = io.KeyCtrl || io.KeySuper;

		// A refused shortcut (during play, or with nothing to undo) says so in the console rather than doing nothing.
		auto run = [this](const char* command) {
			if (const CommandResult result = m_Context.Execute(command); !result.Success)
				PX_WARN("{}: {}", command, result.Error);
		};
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
			run(io.KeyShift ? "edit.redo" : "edit.undo");
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
			run("edit.redo");
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
		{
			const CommandResult result = m_Context.Execute("scene.save");
			if (!result.Success)
				PX_WARN("Save: {}", result.Error);
		}
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
		{
			if (std::optional<UUID> selected = m_Context.GetPrimarySelection())
				m_Context.Execute("entity.duplicate", {{"id", IdString(*selected)}});
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			for (UUID id : std::vector<UUID>(m_Context.GetSelection()))
				m_Context.Execute("entity.destroy", {{"id", IdString(id)}});
		}

		// Gizmo modes and focus only while working in the viewport (not while flying the camera).
		if ((m_ViewportHovered || m_ViewportFocused) && !m_Camera.IsControlling() && !ctrl)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
				m_GizmoOperation = 0;
			if (ImGui::IsKeyPressed(ImGuiKey_W, false))
				m_GizmoOperation = ImGuizmo::TRANSLATE;
			if (ImGui::IsKeyPressed(ImGuiKey_E, false))
				m_GizmoOperation = ImGuizmo::ROTATE;
			if (ImGui::IsKeyPressed(ImGuiKey_R, false))
				m_GizmoOperation = ImGuizmo::SCALE;
			if (ImGui::IsKeyPressed(ImGuiKey_F, false))
				FocusSelection();
		}
	}

	void EditorLayer::FocusSelection()
	{
		std::optional<UUID> selected = m_Context.GetPrimarySelection();
		if (!selected)
			return;
		if (std::optional<Entity> entity = m_Context.GetActiveScene().TryGetEntityByUUID(*selected))
			FocusEntity(*entity);
	}

	void EditorLayer::FocusEntity(Entity entity)
	{
		const AABB bounds = ComputeFocusBounds(m_Context.GetActiveScene(), m_Context.GetAssets(), entity);
		m_Camera.Focus(bounds.GetCenter(), glm::length(bounds.GetExtents()));
		if (m_Renderer)
			m_Renderer->GetDefaultTemporalHistory().GetState().Reset("camera cut");
	}

	void EditorLayer::PickEntity(float u, float v)
	{
		// CPU picking against mesh bounds, through the camera the viewport shows: the nearest hit wins.
		const CameraView view = GetViewportCamera(static_cast<float>(m_ViewportWidth) / static_cast<float>(std::max(m_ViewportHeight, 1u)));
		const glm::mat4 inverse = glm::inverse(view.Projection * view.View);
		const glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
		glm::vec4 nearPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
		glm::vec4 farPoint = inverse * glm::vec4(ndc, 0.0001f, 1.0f);
		nearPoint /= nearPoint.w;
		farPoint /= farPoint.w;
		const glm::vec3 origin(nearPoint);
		const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) - origin);

		Scene& scene = m_Context.GetActiveScene();
		std::optional<UUID> best;
		float bestDistance = std::numeric_limits<float>::max();
		for (entt::entity handle : scene.GetAllEntitiesWith<MeshComponent>())
		{
			Entity entity(handle, &scene);
			Ref<MeshAsset> mesh = m_Context.GetAssets().GetAsset<MeshAsset>(entity.GetComponent<MeshComponent>().Mesh);
			if (!mesh)
				continue;
			const float distance = RayBoxDistance(origin, direction, mesh->Bounds.Transformed(scene.GetWorldTransform(entity)));
			if (distance >= 0.0f && distance < bestDistance)
			{
				bestDistance = distance;
				best = entity.GetUUID();
			}
		}
		if (best)
			m_Context.Select(*best, ImGui::GetIO().KeyCtrl);
		else if (!ImGui::GetIO().KeyCtrl)
			m_Context.ClearSelection();
	}

	void EditorLayer::DrawMenuBar()
	{
		bool openSceneDialog = false;
		bool saveAsDialog = false;
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New Scene"))
					m_Context.Execute("scene.new");
				if (ImGui::MenuItem("Open Scene..."))
					openSceneDialog = true;
				if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !m_Context.GetScenePath().empty()))
					m_Context.Execute("scene.save");
				if (ImGui::MenuItem("Save Scene As...", nullptr, false, m_Context.GetProject() != nullptr))
					saveAsDialog = true;
				ImGui::Separator();
				if (ImGui::MenuItem("Exit"))
					Application::Get().Close();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				// The history is unavailable while a game runs on the scene's copy (edit.undo refuses too).
				const bool editing = !m_Context.IsPlaying();
				const std::string undo = m_Context.CanUndo() ? "Undo " + m_Context.GetUndoDescription() : "Undo";
				const std::string redo = m_Context.CanRedo() ? "Redo " + m_Context.GetRedoDescription() : "Redo";
				if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, editing && m_Context.CanUndo()))
					m_Context.Execute("edit.undo");
				if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, editing && m_Context.CanRedo()))
					m_Context.Execute("edit.redo");
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("View"))
			{
				ImGui::MenuItem("Renderer Settings", nullptr, &m_ShowRenderSettings);
				ImGui::MenuItem("Statistics", nullptr, &m_ShowStatistics);
				ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
				ImGui::EndMenu();
			}

			// Scene title and dirty marker on the right.
			const std::string title = fmt::format("{}{}", m_Context.GetEditorScene().GetName(), m_Context.IsSceneDirty() ? " *" : "");
			ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(title.c_str()).x - 16.0f);
			ImGui::TextDisabled("%s", title.c_str());
			ImGui::EndMainMenuBar();
		}

		if (openSceneDialog || saveAsDialog)
		{
			m_ScenePathInput.clear();
			ImGui::OpenPopup(openSceneDialog ? "Open Scene" : "Save Scene As");
		}
		for (const char* popup : {"Open Scene", "Save Scene As"})
		{
			if (ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextDisabled("Path relative to the project's Assets folder");
				ImGui::SetNextItemWidth(420.0f);
				const bool submit = ImGui::InputTextWithHint("##path", "Scenes/Level.pxscene", &m_ScenePathInput, ImGuiInputTextFlags_EnterReturnsTrue);
				if ((submit || ImGui::Button("OK")) && !m_ScenePathInput.empty())
				{
					const bool open = std::strcmp(popup, "Open Scene") == 0;
					const CommandResult result = m_Context.Execute(open ? "scene.open" : "scene.save", {{"path", m_ScenePathInput}});
					if (!result.Success)
						PX_ERROR("{}", result.Error);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
					ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
		}
	}

	void EditorLayer::DrawToolbar()
	{
		// Gizmo mode buttons on the left, play/stop in the middle.
		auto modeButton = [this](const char* label, int operation, const char* tooltip) {
			const bool active = m_GizmoOperation == operation;
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
			if (ImGui::Button(label))
				m_GizmoOperation = operation;
			if (active)
				ImGui::PopStyleColor();
			ImGui::SetItemTooltip("%s", tooltip);
			ImGui::SameLine();
		};
		modeButton("Select", 0, "No gizmo (Q)");
		modeButton("Move", ImGuizmo::TRANSLATE, "Translate (W)");
		modeButton("Rotate", ImGuizmo::ROTATE, "Rotate (E)");
		modeButton("Scale", ImGuizmo::SCALE, "Scale (R)");
		ImGui::Checkbox("World", &m_GizmoWorldSpace);
		ImGui::SetItemTooltip("Manipulate in world space instead of local space");

		const bool playing = m_Context.IsPlaying();
		const char* label = playing ? "  Stop  " : "  Play  ";
		const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SameLine();
		const float contentRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		ImGui::SameLine(std::max((contentRight - width) * 0.5f, ImGui::GetCursorPosX() + 8.0f));
		ImGui::PushStyleColor(ImGuiCol_Button, playing ? ImVec4(0.75f, 0.30f, 0.28f, 1.0f) : ImVec4(0.20f, 0.55f, 0.35f, 1.0f));
		if (ImGui::Button(label))
			m_Context.Execute(playing ? "play.stop" : "play.start");
		ImGui::PopStyleColor();
		if (playing)
		{
			ImGui::SameLine();
			const bool paused = m_Context.IsPaused();
			if (ImGui::Button(paused ? "Resume" : "Pause"))
				m_Context.Execute(paused ? "play.resume" : "play.pause");
			ImGui::SameLine();
			ImGui::BeginDisabled(!paused);
			if (ImGui::Button("Step"))
				m_Context.Execute("play.step");
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("Advance one fixed step (1/60 s) while paused");
		}
	}

	void EditorLayer::DrawViewport()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
		ImGui::Begin("Viewport");
		ImGui::PopStyleVar();
		DrawToolbar();

		const ImVec2 available = ImGui::GetContentRegionAvail();
		const uint32_t width = static_cast<uint32_t>(std::max(available.x, 1.0f));
		const uint32_t height = static_cast<uint32_t>(std::max(available.y, 1.0f));
		EnsureViewportTarget(width, height);

		const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
		ImGui::Image(ImTextureRef(ImGuiRenderer::ToImTextureID(m_ViewportTexture)), ImVec2(static_cast<float>(width), static_cast<float>(height)));
		m_ViewportHovered = ImGui::IsItemHovered();
		m_ViewportFocused = ImGui::IsWindowFocused();

		// Accept meshes dragged from the asset browser: create an entity in front of the camera.
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PARALAX_ASSET"))
			{
				uint64_t handle = 0;
				if (payload->DataSize == sizeof(handle))
					std::memcpy(&handle, payload->Data, sizeof(handle));
				if (m_Context.GetAssets().GetAssetType(AssetHandle(handle)) == AssetType::Mesh)
				{
					const glm::vec3 position = m_Camera.GetPosition() + m_Camera.GetForward() * 4.0f;
					m_Context.Execute("entity.create", {{"name", "Mesh"}, {"components", {{"Mesh", {{"Mesh", std::to_string(handle)}}}, {"Transform", {{"Translation", {position.x, position.y, position.z}}}}}}});
				}
				else if (m_Context.GetAssets().GetAssetType(AssetHandle(handle)) == AssetType::Scene)
				{
					if (const AssetMetadata* metadata = m_Context.GetAssets().GetMetadata(AssetHandle(handle)))
						m_Context.Execute("scene.open", {{"path", ToUtf8(metadata->FilePath)}});
				}
			}
			ImGui::EndDragDropTarget();
		}

		// Gizmo for the primary selection.
		Scene& scene = m_Context.GetActiveScene();
		std::optional<Entity> selected;
		if (std::optional<UUID> id = m_Context.GetPrimarySelection())
			selected = scene.TryGetEntityByUUID(*id);

		// The gizmo works in the editor camera's space, so it is hidden while looking through a scene camera.
		bool gizmoHovered = false;
		if (selected && m_GizmoOperation != 0 && !m_Camera.IsControlling() && !IsViewingThroughSceneCamera())
		{
			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist();
			ImGuizmo::SetRect(imageOrigin.x, imageOrigin.y, static_cast<float>(width), static_cast<float>(height));

			const glm::mat4 view = m_Camera.GetViewMatrix();
			const glm::mat4 projection = m_Camera.GetGizmoProjection();
			glm::mat4 world = scene.GetWorldTransform(*selected);
			const bool snap = ImGui::GetIO().KeyCtrl;
			const float snapValue = m_GizmoOperation == ImGuizmo::ROTATE ? 15.0f : 0.5f;
			const float snapValues[3] = {snapValue, snapValue, snapValue};

			const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), static_cast<ImGuizmo::OPERATION>(m_GizmoOperation),
													  m_GizmoWorldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL, glm::value_ptr(world), nullptr, snap ? snapValues : nullptr);

			// One undo step per drag: the interactive edit stays open while the gizmo is in use. Opening it only
			// snapshots the scene (never replaces it), so `selected` stays valid.
			constexpr uint64_t gizmoEditKey = 0x67697A6D6F000000ull;
			if (changed && ImGuizmo::IsUsing())
			{
				m_Context.ApplyInteractiveEdit(gizmoEditKey, "Transform");
				scene.SetWorldTransform(*selected, world);
			}
			m_GizmoInUse = ImGuizmo::IsUsing();
			if (m_GizmoInUse)
				m_Context.TouchInteractiveEdit(gizmoEditKey);
			gizmoHovered = ImGuizmo::IsOver();
		}
		else
		{
			m_GizmoInUse = false;
		}

		// Click to select (not while using the gizmo or controlling the camera).
		if (m_ViewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoHovered && !m_GizmoInUse && !ImGui::GetIO().KeyAlt)
		{
			const ImVec2 mouse = ImGui::GetMousePos();
			PickEntity((mouse.x - imageOrigin.x) / static_cast<float>(width), (mouse.y - imageOrigin.y) / static_cast<float>(height));
		}

		// Overlay: frame time and play state.
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const std::string overlay = fmt::format("{:.2f} ms{}", m_FrameTime, m_Context.IsPlaying() ? (m_Context.IsPaused() ? "   PAUSED" : "   PLAYING") : "");
		drawList->AddText(ImVec2(imageOrigin.x + 10.0f, imageOrigin.y + 8.0f), IM_COL32(230, 235, 240, 200), overlay.c_str());

		ImGui::End();
	}

	void EditorLayer::DrawRenderSettings()
	{
		ImGui::Begin("Renderer", &m_ShowRenderSettings);
		if (!m_Renderer)
		{
			ImGui::TextDisabled("Renderer unavailable");
			ImGui::End();
			return;
		}
		// Renderer settings are view state, not scene data: edits apply immediately and are not undoable.
		DrawFields(m_Context, SceneRendererSettings::GetFields(), &m_Renderer->GetSettings(), nullptr);
		ImGui::End();
	}

	void EditorLayer::DrawStatistics()
	{
		ImGui::Begin("Statistics", &m_ShowStatistics);
		if (m_Renderer)
		{
			const SceneRenderStats& stats = m_Renderer->GetStats();
			ImGui::Text("Frame timestep: %.2f ms", static_cast<double>(m_FrameTime));
			const auto& timingStats = m_Renderer->GetStats();
			if (timingStats.CPURenderMilliseconds)
				ImGui::Text("CPU Render: %.2f ms", *timingStats.CPURenderMilliseconds);
			if (timingStats.ProfilingEnabled && timingStats.GPUTiming && timingStats.GPUTiming->Milliseconds[0])
				ImGui::Text("GPU Render: %.2f ms (frame %llu)", *timingStats.GPUTiming->Milliseconds[0], static_cast<unsigned long long>(timingStats.GPUTiming->Context.Frame));
			if (timingStats.ProfilingEnabled)
				ImGui::Text("GPU timing: %s", timingStats.GPUProfilingStatus.c_str());
			ImGui::Text("Draw calls: %u (+%u shadow)", stats.DrawCalls, stats.ShadowDrawCalls);
			ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(stats.Triangles));
			ImGui::Text("Culled submeshes: %u", stats.CulledSubmeshes);
			ImGui::Text("Lights: %u", stats.Lights);
		}
		ImGui::Text("Entities: %zu", m_Context.GetActiveScene().GetEntityCount());
		ImGui::End();
	}

}
