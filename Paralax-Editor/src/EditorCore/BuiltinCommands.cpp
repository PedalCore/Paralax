#include "EditorCore/EditorContext.h"

#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MaterialSerializer.h>
#include <Paralax/Core/JsonText.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Core/Reflection.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <charconv>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		std::string IdString(UUID id)
		{
			return std::to_string(static_cast<uint64_t>(id));
		}

		json DescribeEntity(Scene& scene, Entity entity, bool includeComponents)
		{
			json result = {
				{"id", IdString(entity.GetUUID())},
				{"name", entity.GetName()},
				{"tag", entity.GetComponent<TagComponent>().Tag},
				{"parent", IdString(entity.GetComponent<RelationshipComponent>().Parent)},
			};
			json children = json::array();
			for (UUID child : entity.GetComponent<RelationshipComponent>().Children)
				children.push_back(IdString(child));
			result["children"] = children;

			json components = includeComponents ? json::object() : json::array();
			for (const ComponentInfo& info : ComponentRegistry::GetAll())
			{
				void* component = info.Get(scene.GetRegistry(), entity.GetHandle());
				if (!component)
					continue;
				if (includeComponents)
					components[info.Name] = ComponentRegistry::ToJson(info, component);
				else
					components.push_back(info.Name);
			}
			result["components"] = components;
			return result;
		}

		CommandResult EntityNotFound(const json& params)
		{
			// Described, not dumped: the id is whatever the caller sent, a 16 MiB string included.
			const auto id = params.find("id");
			return CommandResult::Fail(fmt::format("No entity with id {}", id == params.end() ? std::string("null") : DescribeJsonValue(*id)));
		}

		// Names are written through the registry, so the editor accepts exactly the names scene files, scripts and
		// agents do, and never one that saving or an undo would then drop.
		CommandResult RenameEntity(Entity entity, const std::string& name)
		{
			if (name.empty())
				return CommandResult::Fail("Names cannot be empty");
			std::string error;
			if (!ComponentRegistry::SetField(*ComponentRegistry::Find("Tag")->FindField("Name"), &entity.GetComponent<TagComponent>(), name, error))
				return CommandResult::Fail(error);
			return CommandResult::Ok();
		}

		// Applies { "<Component>": { fields } } to an entity, adding missing components.
		CommandResult ApplyComponents(Scene& scene, Entity entity, const json& components)
		{
			Reflection::ErrorList errors;
			for (auto it = components.begin(); it != components.end(); ++it)
			{
				const ComponentInfo* info = ComponentRegistry::Find(it.key());
				if (!info)
				{
					errors.Add(fmt::format("unknown component {}", QuoteText(it.key())));
					continue;
				}
				if (!info->Has(scene.GetRegistry(), entity.GetHandle()) && !info->UserEditable)
				{
					errors.Add(fmt::format("component '{}' cannot be added", info->Name));
					continue;
				}
				std::string error;
				if (!ComponentRegistry::FromJson(*info, info->Add(scene.GetRegistry(), entity.GetHandle()), it.value(), error))
					errors.Add(std::move(error));
			}
			// Same rule as entity.rename (and what the scene loader would otherwise silently fix up).
			if (entity.GetName().empty())
				errors.Add("the entity name cannot be empty");
			if (!errors.IsEmpty())
				return CommandResult::Fail(errors.Join());
			return CommandResult::Ok({{"id", IdString(entity.GetUUID())}});
		}

	}

	bool CreateDemoScene(EditorContext& context)
	{
		// A scene file that opened is the user's, empty or not: filling it would put demo entities into their file at
		// the next save, unmarked as changes.
		if (!context.GetScenePath().empty() || context.GetEditorScene().GetEntityCount() != 0)
			return false;
		// Through the same commands an agent would use, so the demo is a scene like any other.
		auto mesh = [](uint64_t handle) { return json{{"Mesh", std::to_string(handle)}}; };
		const json entities[] = {
			{{"name", "Sun"}, {"components", {{"DirectionalLight", {{"Intensity", 2.8}}}, {"Transform", {{"Rotation", {-0.40, 0.30, 0.14, 0.85}}}}}}},
			{{"name", "Ground"}, {"components", {{"Mesh", {{"Mesh", std::to_string(AssetManager::BuiltinPlane)}, {"Materials", {std::to_string(AssetManager::BuiltinGroundMaterial)}}}}}}},
			{{"name", "Sphere"}, {"components", {{"Mesh", mesh(AssetManager::BuiltinSphere)}, {"Transform", {{"Translation", {-1.2, 0.5, 0}}}}}}},
			{{"name", "Cube"}, {"components", {{"Mesh", mesh(AssetManager::BuiltinCube)}, {"Transform", {{"Translation", {1.2, 0.5, 0.3}}}}}}},
			{{"name", "Capsule"}, {"components", {{"Mesh", mesh(AssetManager::BuiltinCapsule)}, {"Transform", {{"Translation", {0, 1, -1.5}}}}}}},
			{{"name", "Camera"}, {"components", {{"Camera", json::object()}, {"Transform", {{"Translation", {0, 2.2, 6}}, {"Rotation", {-0.17, 0, 0, 0.985}}}}}}},
		};
		for (const json& entity : entities)
		{
			if (const CommandResult result = context.Execute("entity.create", entity); !result.Success)
				PX_CORE_ERROR("Demo scene: cannot create '{}': {}", entity["name"].get<std::string>(), result.Error);
		}
		context.ClearSelection();
		// Nobody has edited anything yet: the editor starts clean, with nothing to undo.
		context.ResetHistory();
		return true;
	}

	void RegisterBuiltinCommands(CommandRegistry& registry)
	{
		// --- Discovery -------------------------------------------------------------------------------
		registry.Register({"help", "Lists every command with its parameters.", {}, false, [&registry](EditorContext&, const json&) { return CommandResult::Ok({{"commands", registry.Describe()}}); }});

		registry.Register({"component.types", "Lists component types with their fields (name, type, range, enum values).", {}, false, [](EditorContext&, const json&) {
							   json types = json::array();
							   for (const ComponentInfo& info : ComponentRegistry::GetAll())
							   {
								   json fields = json::array();
								   for (const FieldInfo& field : info.Fields)
								   {
									   json f = {{"name", field.Name}, {"description", field.Description}};
									   static const char* typeNames[] = {"bool", "int", "float", "vec2", "vec3", "vec4", "quat", "string", "asset", "asset[]", "enum"};
									   f["type"] = typeNames[static_cast<int>(field.Type)];
									   if (field.Min < field.Max)
										   f["range"] = {field.Min, field.Max};
									   if (!field.EnumNames.empty())
										   f["values"] = field.EnumNames;
									   if (field.MaxLength > 0)
										   f["maxLength"] = field.MaxLength;
									   if (field.Flags & FieldFlagAngle)
										   f["units"] = "radians";
									   fields.push_back(std::move(f));
								   }
								   types.push_back({{"name", info.Name}, {"description", info.Description}, {"fields", fields}, {"removable", info.UserEditable}});
							   }
							   return CommandResult::Ok({{"components", types}});
						   }});

		// --- Scene -----------------------------------------------------------------------------------
		registry.Register({"scene.info", "Name, file, entity count and state of the open scene.", {}, false, [](EditorContext& context, const json&) {
							   Scene& scene = context.GetActiveScene();
							   return CommandResult::Ok({{"name", scene.GetName()}, {"path", ToUtf8(context.GetScenePath())}, {"entities", scene.GetEntityCount()}, {"dirty", context.IsSceneDirty()}, {"playing", context.IsPlaying()}, {"canUndo", context.CanUndo()}, {"canRedo", context.CanRedo()}});
						   }});

		registry.Register({"scene.new", "Replaces the open scene with an empty one (unsaved changes are lost).", {{"name", "string", "Scene name.", false}}, false, [](EditorContext& context, const json& params) {
							   if (!context.NewScene(params.value("name", "Untitled")))
								   return CommandResult::Fail("Cannot replace the scene while an edit is in progress");
							   return CommandResult::Ok();
						   }});

		registry.Register({"scene.open", "Opens a .pxscene file (relative to the project's Assets folder; absolute paths must lie inside it).", {{"path", "string", "Scene file.", true}}, false, [](EditorContext& context, const json& params) {
							   if (!context.OpenScene(FromUtf8(params["path"].get<std::string>())))
								   return CommandResult::Fail("Could not open the scene (see the log for details: console.get)");
							   return CommandResult::Ok({{"entities", context.GetEditorScene().GetEntityCount()}});
						   }});

		registry.Register({"scene.save", "Saves the scene; 'path' (relative to Assets, and inside it) is required the first time.", {{"path", "string", "Destination .pxscene file.", false}}, false, [](EditorContext& context, const json& params) {
							   if (context.IsPlaying())
								   return CommandResult::Fail("Stop playing before saving");
							   if (!context.SaveScene(FromUtf8(params.value("path", ""))))
								   return CommandResult::Fail("Could not save the scene (no path, or the file could not be written)");
							   json result = {{"path", ToUtf8(context.GetScenePath())}};
							   // The file is saved either way, but a scene the registry does not list cannot be the start scene or
							   // be found by handle in the next session.
							   if (Project* project = context.GetProject(); project && !project->GetAssetManager().GetHandleForPath(context.GetScenePath()).IsValid())
								   result["warnings"] = json::array({"Saved, but not registered: the asset registry could not be saved (see the log for details: console.get)"});
							   return CommandResult::Ok(result);
						   }});

		registry.Register({"scene.list", "Lists every entity in hierarchy order (id, name, tag, parent, children, component names).", {}, false, [](EditorContext& context, const json&) {
							   Scene& scene = context.GetActiveScene();
							   json entities = json::array();
							   scene.ForEachEntity([&](Entity entity) { entities.push_back(DescribeEntity(scene, entity, false)); });
							   return CommandResult::Ok({{"entities", entities}});
						   }});

		registry.Register({"scene.find", "Finds entities by exact name and/or tag.", {{"name", "string", "Entity name.", false}, {"tag", "string", "Entity tag.", false}}, false, [](EditorContext& context, const json& params) {
							   Scene& scene = context.GetActiveScene();
							   json ids = json::array();
							   scene.ForEachEntity([&](Entity entity) {
								   const auto& tag = entity.GetComponent<TagComponent>();
								   if ((!params.contains("name") || tag.Name == params["name"].get<std::string>()) && (!params.contains("tag") || tag.Tag == params["tag"].get<std::string>()))
									   ids.push_back(IdString(entity.GetUUID()));
							   });
							   return CommandResult::Ok({{"ids", ids}});
						   }});

		// --- Entities --------------------------------------------------------------------------------
		registry.Register({"entity.create", "Creates an entity, optionally with a parent and components ({\"Mesh\": {\"Mesh\": \"1\"}, ...}).", {{"name", "string", "Entity name.", false}, {"parent", "any", "Parent entity id (null or omitted = root).", false}, {"components", "object", "Components to add, with field values.", false}}, true, [](EditorContext& context, const json& params) {
							   Scene& scene = context.GetActiveScene();
							   std::optional<Entity> parent;
							   // null means root, as it does for entity.set_parent and prefab.instantiate.
							   if (params.contains("parent") && !params["parent"].is_null())
							   {
								   parent = context.ResolveEntity(params["parent"]);
								   if (!parent)
									   return CommandResult::Fail("Parent entity not found");
							   }
							   Entity entity = scene.CreateEntity();
							   if (const std::string name = params.value("name", std::string()); !name.empty())
							   {
								   if (CommandResult renamed = RenameEntity(entity, name); !renamed.Success)
									   return renamed;
							   }
							   if (parent)
								   scene.SetParent(entity, parent, false);
							   if (params.contains("components"))
							   {
								   CommandResult applied = ApplyComponents(scene, entity, params["components"]);
								   if (!applied.Success)
									   return applied;
							   }
							   context.Select(entity.GetUUID());
							   return CommandResult::Ok({{"id", IdString(entity.GetUUID())}});
						   }});

		registry.Register({"entity.get", "Returns an entity with all of its components and field values.", {{"id", "any", "Entity id.", true}}, false, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return EntityNotFound(params);
							   return CommandResult::Ok(DescribeEntity(context.GetActiveScene(), *entity, true));
						   }});

		registry.Register({"entity.destroy", "Destroys an entity and its children.", {{"id", "any", "Entity id.", true}}, true, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return EntityNotFound(params);
							   context.GetActiveScene().DestroyEntity(*entity);
							   context.SetSelection(context.GetSelection());
							   return CommandResult::Ok();
						   }});

		registry.Register({"entity.duplicate", "Duplicates an entity (with children) and selects the copy.", {{"id", "any", "Entity id.", true}}, true, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return EntityNotFound(params);
							   Entity copy = context.GetActiveScene().DuplicateEntity(*entity);
							   context.Select(copy.GetUUID());
							   return CommandResult::Ok({{"id", IdString(copy.GetUUID())}});
						   }});

		registry.Register({"entity.rename", "Renames an entity.", {{"id", "any", "Entity id.", true}, {"name", "string", "New name.", true}}, true, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return EntityNotFound(params);
							   return RenameEntity(*entity, params["name"].get<std::string>());
						   }});

		registry.Register({"entity.set_parent", "Reparents an entity (null or omitted parent = root), keeping its world transform.", {{"id", "any", "Entity id.", true}, {"parent", "any", "New parent id, or null.", false}}, true, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return EntityNotFound(params);
							   std::optional<Entity> parent;
							   if (params.contains("parent") && !params["parent"].is_null())
							   {
								   parent = context.ResolveEntity(params["parent"]);
								   if (!parent)
									   return CommandResult::Fail("Parent entity not found");
							   }
							   if (!context.GetActiveScene().SetParent(*entity, parent, true))
								   return CommandResult::Fail("Invalid parent (an entity cannot be parented to itself or a descendant)");
							   return CommandResult::Ok();
						   }});

		// --- Components ------------------------------------------------------------------------------
		registry.Register({"component.set", "Sets (and adds if missing) components: {\"Transform\": {\"Translation\": [0,1,0]}, ...}. Unspecified fields keep their values.", {{"id", "any", "Entity id.", true}, {"components", "object", "Component name -> fields.", true}}, true, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return EntityNotFound(params);
							   return ApplyComponents(context.GetActiveScene(), *entity, params["components"]);
						   }});

		registry.Register({"component.remove", "Removes a component from an entity.", {{"id", "any", "Entity id.", true}, {"component", "string", "Component name.", true}}, true, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return EntityNotFound(params);
							   const ComponentInfo* info = ComponentRegistry::Find(params["component"].get<std::string>());
							   if (!info)
								   return CommandResult::Fail("Unknown component");
							   if (!info->Remove)
								   return CommandResult::Fail(fmt::format("'{}' cannot be removed", info->Name));
							   if (!info->Has(context.GetActiveScene().GetRegistry(), entity->GetHandle()))
								   return CommandResult::Fail(fmt::format("The entity has no '{}' component", info->Name));
							   info->Remove(context.GetActiveScene().GetRegistry(), entity->GetHandle());
							   return CommandResult::Ok();
						   }});

		// --- Selection -------------------------------------------------------------------------------
		registry.Register({"selection.get", "Returns the selected entity ids (last = primary).", {}, false, [](EditorContext& context, const json&) {
							   json ids = json::array();
							   for (UUID id : context.GetSelection())
								   ids.push_back(IdString(id));
							   return CommandResult::Ok({{"ids", ids}});
						   }});

		registry.Register({"selection.set", "Selects entities by id (unknown ids are ignored).", {{"ids", "array", "Entity ids.", true}}, false, [](EditorContext& context, const json& params) {
							   std::vector<UUID> selection;
							   for (const json& id : params["ids"])
							   {
								   if (std::optional<Entity> entity = context.ResolveEntity(id))
									   selection.push_back(entity->GetUUID());
							   }
							   context.SetSelection(std::move(selection));
							   return CommandResult::Ok({{"count", context.GetSelection().size()}});
						   }});

		// --- Undo ------------------------------------------------------------------------------------
		// The history belongs to the editor scene, which is not being edited while a game runs on its copy: a precise
		// error keeps an agent from concluding the history is gone (scene.info still reports canUndo).
		registry.Register({"edit.undo", "Undoes the last scene change (not while playing).", {}, false, [](EditorContext& context, const json&) {
							   if (context.IsPlaying())
								   return CommandResult::Fail("Stop play mode first (play.stop)");
							   const std::string description = context.GetUndoDescription();
							   context.CommitInteractiveEdit();
							   if (context.IsChangeOpen())
								   return CommandResult::Fail("An edit is in progress");
							   std::vector<std::string> warnings;
							   if (!context.Undo(&warnings))
								   return CommandResult::Fail("Nothing to undo");
							   json result = {{"undone", description}};
							   if (!warnings.empty())
								   result["warnings"] = warnings; // Values the restored scene could not hold (logged as errors).
							   return CommandResult::Ok(std::move(result));
						   }});

		registry.Register({"edit.redo", "Redoes the last undone scene change (not while playing).", {}, false, [](EditorContext& context, const json&) {
							   if (context.IsPlaying())
								   return CommandResult::Fail("Stop play mode first (play.stop)");
							   const std::string description = context.GetRedoDescription();
							   context.CommitInteractiveEdit();
							   if (context.IsChangeOpen())
								   return CommandResult::Fail("An edit is in progress");
							   std::vector<std::string> warnings;
							   if (!context.Redo(&warnings))
								   return CommandResult::Fail("Nothing to redo");
							   json result = {{"redone", description}};
							   if (!warnings.empty())
								   result["warnings"] = warnings;
							   return CommandResult::Ok(std::move(result));
						   }});

		// --- Play mode -------------------------------------------------------------------------------
		registry.Register({"play.start", "Starts play mode on a copy of the scene. With paused, the game only advances through play.step (deterministic).", {{"paused", "boolean", "Start paused (default false).", false}}, false, [](EditorContext& context, const json& params) {
							   const bool paused = params.contains("paused") && params["paused"].get<bool>();
							   if (!context.StartPlaying(paused))
								   return CommandResult::Fail("Already playing");
							   return CommandResult::Ok({{"paused", paused}});
						   }});

		registry.Register({"play.pause", "Pauses the running game: it then only advances through play.step.", {}, false, [](EditorContext& context, const json&) {
							   if (!context.IsPlaying())
								   return CommandResult::Fail("Not playing");
							   context.SetPaused(true);
							   return CommandResult::Ok();
						   }});

		registry.Register({"play.resume", "Resumes a paused game (it advances with the editor's frames again).", {}, false, [](EditorContext& context, const json&) {
							   if (!context.IsPlaying())
								   return CommandResult::Fail("Not playing");
							   context.SetPaused(false);
							   return CommandResult::Ok();
						   }});

		registry.Register({"audio.set_output", "Chooses how play mode sounds from the next play.start: Device (heard) or Offline (silent and deterministic; the default for headless editors).", {{"output", "string", "Device or Offline.", true}}, false, [](EditorContext& context, const json& params) {
							   const std::string output = params["output"].get<std::string>();
							   if (output != "Device" && output != "Offline")
								   return CommandResult::Fail("output must be Device or Offline");
							   context.SetAudioOutput(output == "Device" ? AudioOutput::Device : AudioOutput::Offline);
							   return CommandResult::Ok({{"output", output}});
						   }});

		registry.Register({"audio.status", "The audio of the running game: the output in use (Device may have fallen back to Offline), sounds playing, master volume and paused state.", {}, false, [](EditorContext& context, const json&) {
							   const char* chosen = context.GetAudioOutput() == AudioOutput::Device ? "Device" : "Offline";
							   AudioSystem* audio = context.GetAudioSystem();
							   if (!audio)
								   return CommandResult::Ok({{"playing", false}, {"chosenOutput", chosen}});
							   return CommandResult::Ok({{"playing", true},
														 {"chosenOutput", chosen},
														 {"output", audio->GetOutput() == AudioOutput::Device ? "Device" : "Offline"},
														 {"voices", audio->GetVoiceCount()},
														 {"masterVolume", audio->GetMasterVolume()},
														 {"paused", audio->IsPaused()}});
						   }});

		registry.Register({"play.stop", "Stops play mode, discarding runtime changes.", {}, false, [](EditorContext& context, const json&) {
							   if (!context.StopPlaying())
								   return CommandResult::Fail("Not playing");
							   return CommandResult::Ok();
						   }});

		registry.Register({"play.step", "Advances the running scene by a number of fixed steps (deterministic). Each step is one input frame: input injected before (input.*) is pressed or released in the first step and held after.", {{"frames", "integer", "Number of steps (1-10000).", false}, {"dt", "number", "Seconds per step (default 1/60).", false}}, false, [](EditorContext& context, const json& params) {
							   if (!context.IsPlaying())
								   return CommandResult::Fail("Not playing");
							   const int64_t frames = params.value("frames", int64_t(1));
							   const double dt = params.value("dt", 1.0 / 60.0);
							   if (frames < 1 || frames > 10000 || !(dt > 0.0) || dt > 1.0)
								   return CommandResult::Fail("frames must be 1-10000 and dt in (0, 1]");
							   context.StepRuntime(frames, static_cast<float>(dt));
							   return CommandResult::Ok({{"frames", frames}});
						   }});

		// --- Assets ----------------------------------------------------------------------------------
		registry.Register({"asset.list", "Lists registered assets (handle, type, path, parent) and the built-in assets, and whether the registry is read-only (readOnly; skipped counts the entries the last load could not read).", {}, false, [](EditorContext& context, const json&) {
							   json assets = json::array();
							   for (const AssetMetadata& metadata : context.GetAssets().GetRegisteredAssets())
							   {
								   assets.push_back({{"handle", IdString(metadata.Handle)}, {"type", std::string(AssetTypeToString(metadata.Type))}, {"path", ToUtf8(metadata.FilePath)}, {"parent", IdString(metadata.Parent)}});
							   }
							   json builtins = json::array();
							   for (const BuiltinAssetInfo& info : AssetManager::GetBuiltinAssets())
								   builtins.push_back({{"handle", IdString(AssetHandle(info.Handle))}, {"type", std::string(AssetTypeToString(info.Type))}, {"name", info.Name}});
							   // Whether commands that register files can save the registry at all (see asset.reload_registry).
							   return CommandResult::Ok({{"assets", assets},
														 {"builtins", builtins},
														 {"readOnly", context.GetAssets().IsRegistryReadOnly()},
														 {"skipped", context.GetAssets().GetSkippedRegistryEntryCount()}});
						   }});

		registry.Register({"asset.import", "Registers a file inside the project's Assets folder (path relative to Assets, or absolute).", {{"path", "string", "File to import.", true}}, false, [](EditorContext& context, const json& params) {
							   if (!context.GetProject())
								   return CommandResult::Fail("No project is open");
							   const AssetHandle handle = context.GetAssets().ImportAsset(FromUtf8(params["path"].get<std::string>()));
							   if (!handle.IsValid())
								   return CommandResult::Fail("Import failed (see the log for details: console.get)");
							   return CommandResult::Ok({{"handle", IdString(handle)}});
						   }});

		// The registry file is never saved over once it differs from what was last read or written, so a checkout or
		// merge under a running editor survives; this is how an agent takes the new file in.
		registry.Register({"asset.reload_registry",
						   "Reads Assets/AssetRegistry.json from disk again, replacing the registry in memory (not while playing). Commands that register files "
						   "(asset.import, material.create, a first scene.save, ...) refuse to save over a registry that changed on disk since it was read or last "
						   "saved, by a version-control checkout or merge or another editor, that could not be read (malformed, or of a newer version), or that had "
						   "entries this build skipped (readOnly in the result and in asset.list, with skipped counting them); once the file is as it should be, "
						   "reload it with this. Assets already loaded stay loaded. A file that cannot be read fails the reload and leaves nothing registered until "
						   "a reload succeeds.",
						   {},
						   false,
						   [](EditorContext& context, const json&) {
							   if (!context.GetProject())
								   return CommandResult::Fail("No project is open");
							   // The running game resolves paths through the registry; it must not change under it.
							   if (context.IsPlaying())
								   return CommandResult::Fail("Stop play mode first (play.stop)");
							   AssetManager& assets = context.GetAssets();
							   if (!assets.LoadRegistry())
								   return CommandResult::Fail("The asset registry could not be read: nothing is registered, and it is not saved over until it loads (see the log for details: console.get)");
							   const size_t skipped = assets.GetSkippedRegistryEntryCount();
							   json result = {{"assets", assets.GetRegisteredAssets().size()}, {"skipped", skipped}, {"readOnly", assets.IsRegistryReadOnly()}};
							   if (skipped > 0)
								   result["warnings"] = json::array({fmt::format("{} registry {} could not be read and {} skipped (see the log for details: console.get). Until the file is fixed and reloaded, the registry is read-only: commands that register files fail.",
																				 skipped, skipped == 1 ? "entry" : "entries", skipped == 1 ? "was" : "were")});
							   return CommandResult::Ok(std::move(result));
						   }});

		registry.Register({"material.create", "Creates a new .pxmat material file in the project (failing if it exists), imports it and returns its handle.", {{"path", "string", "Destination (relative to Assets), e.g. Materials/Red.pxmat.", true}, {"fields", "object", "Material fields, e.g. {\"BaseColor\": [1,0,0,1], \"Roughness\": 0.4}.", false}}, false, [](EditorContext& context, const json& params) {
							   if (!context.GetProject())
								   return CommandResult::Fail("No project is open");
							   MaterialAsset material;
							   std::string error;
							   if (params.contains("fields") && !Reflection::FromJson(MaterialSerializer::GetMaterialFields(), "Material", &material, params["fields"], error))
								   return CommandResult::Fail(error);
							   std::filesystem::path path = FromUtf8(params["path"].get<std::string>());
							   if (path.extension() != ".pxmat")
								   path += ".pxmat";
							   std::string nameError;
							   const std::optional<std::filesystem::path> registryPath = context.GetAssets().ToRegistryPath(path, &nameError);
							   if (!registryPath)
								   return CommandResult::Fail(nameError.empty() ? std::string("The path must be inside the project's Assets folder") : nameError);
							   std::error_code existsError;
							   if (std::filesystem::exists(context.GetAssets().ToAbsolutePath(*registryPath), existsError))
								   return CommandResult::Fail(fmt::format("'{}' already exists; use material.set to change it", ToUtf8(*registryPath)));
							   const std::filesystem::path file = context.GetAssets().ToAbsolutePath(*registryPath);
							   if (!MaterialSerializer::SaveToFile(material, file))
								   return CommandResult::Fail("Could not write the material file");
							   const AssetHandle handle = context.GetAssets().ImportAsset(*registryPath);
							   if (!handle.IsValid())
							   {
								   // The file is new (checked above): it is removed, so a retry is not refused because the file
								   // exists. (Folders created for it stay.)
								   std::error_code removeError;
								   std::filesystem::remove(file, removeError);
								   return CommandResult::Fail("Import failed (see the log for details: console.get)");
							   }
							   return CommandResult::Ok({{"handle", IdString(handle)}});
						   }});

		registry.Register({"material.set", "Changes material fields; file-backed materials are saved, others (e.g. glTF sub-materials) change in memory.", {{"handle", "string", "Material asset handle.", true}, {"fields", "object", "Fields to change.", true}}, false, [](EditorContext& context, const json& params) {
							   uint64_t value = 0;
							   const std::string& text = params["handle"].get_ref<const std::string&>();
							   auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
							   if (ec != std::errc() || ptr != text.data() + text.size())
								   return CommandResult::Fail("Invalid handle");
							   Ref<MaterialAsset> material = context.GetAssets().GetAsset<MaterialAsset>(AssetHandle(value));
							   if (!material)
								   return CommandResult::Fail("No material with that handle");
							   if (context.GetAssets().IsBuiltin(material->Handle))
								   return CommandResult::Fail("Built-in materials cannot be changed; create a material instead");
							   // Edit a copy so an invalid field (or a failed save) leaves both the asset and its file untouched.
							   MaterialAsset edited = *material;
							   std::string error;
							   if (!Reflection::FromJson(MaterialSerializer::GetMaterialFields(), "Material", &edited, params["fields"], error))
								   return CommandResult::Fail(error);
							   if (const AssetMetadata* metadata = context.GetAssets().GetMetadata(material->Handle); metadata && !metadata->Parent.IsValid())
							   {
								   if (!MaterialSerializer::SaveToFile(edited, context.GetAssets().ToAbsolutePath(metadata->FilePath)))
									   return CommandResult::Fail("Could not write the material file");
							   }
							   edited.Version = material->Version + 1;
							   *material = std::move(edited);
							   return CommandResult::Ok();
						   }});

		// --- Rendering -------------------------------------------------------------------------------
		registry.Register({"render.capture", "Renders the scene to a PNG (from the primary camera, or the editor camera).", {{"path", "string", "Output PNG path.", true}, {"width", "integer", "Width in pixels (default 1280).", false}, {"height", "integer", "Height in pixels (default 720).", false}, {"camera", "string", "\"scene\" (primary camera) or \"editor\".", false}}, false, [](EditorContext& context, const json& params) {
							   if (!context.GetCaptureHandler())
								   return CommandResult::Fail("Rendering is not available (no graphics device)");
							   return context.GetCaptureHandler()(params);
						   }});
	}

}
