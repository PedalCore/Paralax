#include "EditorCore/EditorContext.h"

#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/ModelPrefab.h>
#include <Paralax/Scene/Prefab.h>
#include <Paralax/Scene/SceneSerializer.h>
#include <Paralax/Scripting/ScriptAsset.h>
#include <Paralax/Scripting/ScriptSystem.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		constexpr const char* s_ScriptTemplate = R"(-- {name}: attached to an entity through a Script component.
-- self.entity is the entity; see docs/Scripting.md for the API (Entity, Scene, Input, Time, Log, vec3, quat).
local {name} = {{}}

function {name}:OnCreate()
end

function {name}:OnUpdate(dt)
end

function {name}:OnDestroy()
end

return {name}
)";

		std::string IdString(UUID id)
		{
			return std::to_string(static_cast<uint64_t>(id));
		}

		// A Lua identifier derived from a file name ("player-controller" -> "PlayerController").
		std::string ClassName(const std::filesystem::path& path)
		{
			std::string name;
			bool upper = true;
			for (char c : path.stem().string())
			{
				const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
				if (!alphanumeric)
				{
					upper = true;
					continue;
				}
				name += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
				upper = false;
			}
			if (name.empty() || (name[0] >= '0' && name[0] <= '9'))
				name = "Script" + name;
			return name;
		}

		// Resolves a destination for a new asset file: forces the extension, keeps it inside the asset directory,
		// and refuses to overwrite.
		std::optional<std::filesystem::path> NewAssetPath(EditorContext& context, const std::string& requested, const char* extension, std::string& outError)
		{
			if (!context.GetProject())
			{
				outError = "No project is open";
				return std::nullopt;
			}
			std::filesystem::path path = requested;
			if (path.extension() != extension)
				path += extension;
			std::optional<std::filesystem::path> resolved = context.ResolveProjectPath(path, outError);
			if (!resolved)
				return std::nullopt;
			std::error_code error;
			if (std::filesystem::exists(*resolved, error))
			{
				outError = fmt::format("'{}' already exists", path.generic_string());
				return std::nullopt;
			}
			return resolved;
		}

		// Imports a file the command has just written to a NewAssetPath. If the import fails (the asset registry cannot
		// be saved, say), the file is removed again, so a retry is not refused because the file exists. (Folders created
		// for it stay.)
		AssetHandle ImportNewFile(EditorContext& context, const std::filesystem::path& path)
		{
			const AssetHandle handle = context.GetAssets().ImportAsset(path);
			if (!handle.IsValid())
			{
				std::error_code error;
				std::filesystem::remove(path, error);
			}
			return handle;
		}

		// An existing script file of the project, by path relative to Assets (or its handle as a string): its handle
		// and its file (resolved and checked to lie inside the project).
		std::optional<std::pair<AssetHandle, std::filesystem::path>> FindScript(EditorContext& context, const std::string& requested, std::string& outError)
		{
			if (!context.GetProject())
			{
				outError = "No project is open";
				return std::nullopt;
			}
			AssetManager& assets = context.GetAssets();
			AssetHandle handle = 0;
			// Handles are written as decimal numbers; a script named like one is addressed with its extension.
			if (uint64_t id = 0; !requested.empty() && std::from_chars(requested.data(), requested.data() + requested.size(), id).ptr == requested.data() + requested.size())
				handle = AssetHandle(id);
			else
			{
				std::filesystem::path path = requested;
				if (path.extension() != ".lua")
					path += ".lua";
				std::optional<std::filesystem::path> resolved = context.ResolveProjectPath(path, outError);
				if (!resolved)
					return std::nullopt;
				handle = assets.GetHandleForPath(*resolved);
			}
			const AssetMetadata* metadata = handle.IsValid() ? assets.GetMetadata(handle) : nullptr;
			if (!metadata || metadata->Type != AssetType::Script || metadata->FilePath.empty())
			{
				outError = fmt::format("'{}' is not a script in this project", requested);
				return std::nullopt;
			}
			// The registry's path goes through the same checks as one typed by an agent.
			std::optional<std::filesystem::path> file = context.ResolveProjectPath(metadata->FilePath, outError);
			if (!file)
				return std::nullopt;
			std::error_code error;
			if (!std::filesystem::is_regular_file(*file, error))
			{
				outError = fmt::format("The script file '{}' is missing", metadata->FilePath.generic_string());
				return std::nullopt;
			}
			return std::pair{handle, *file};
		}

		AssetHandle ResolveAsset(EditorContext& context, const json& reference)
		{
			if (!reference.is_string())
				return AssetHandle(0);
			const std::string& text = reference.get_ref<const std::string&>();
			uint64_t value = 0;
			auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (ec == std::errc() && ptr == text.data() + text.size() && !text.empty())
				return AssetHandle(value);
			return context.GetAssets().GetHandleForPath(text);
		}

	}

	void RegisterScriptingCommands(CommandRegistry& registry)
	{
		registry.Register({"script.create", "Creates a Lua script in the project (from 'source', or a template with OnCreate/OnUpdate/OnDestroy) and returns its handle.", {{"path", "string", "Destination relative to Assets, e.g. Scripts/Player.lua.", true}, {"source", "string", "Lua source; the chunk must return a table.", false}}, false, [](EditorContext& context, const json& params) {
							   std::string error;
							   std::optional<std::filesystem::path> path = NewAssetPath(context, params["path"].get<std::string>(), ".lua", error);
							   if (!path)
								   return CommandResult::Fail(error);
							   std::string source;
							   if (params.contains("source"))
							   {
								   source = params["source"].get<std::string>();
							   }
							   else
							   {
								   const std::string name = ClassName(*path);
								   source = fmt::format(fmt::runtime(s_ScriptTemplate), fmt::arg("name", name));
							   }
							   if (source.size() > ScriptAsset::MaxSourceBytes)
								   return CommandResult::Fail("The script is too large");
							   if (!WriteFileAtomically(*path, source))
								   return CommandResult::Fail("Could not write the script file");
							   const AssetHandle handle = ImportNewFile(context, *path);
							   if (!handle.IsValid())
								   return CommandResult::Fail("Import failed (see the log for details: console.get)");
							   return CommandResult::Ok({{"handle", IdString(handle)}, {"path", context.GetAssets().GetMetadata(handle)->FilePath.generic_string()}});
						   }});

		registry.Register({"script.get", "Returns a script's source as it is on disk.", {{"path", "string", "Script relative to Assets (e.g. Scripts/Player.lua), or its handle.", true}}, false, [](EditorContext& context, const json& params) {
							   std::string error;
							   auto found = FindScript(context, params["path"].get<std::string>(), error);
							   if (!found)
								   return CommandResult::Fail(error);
							   // From the file, not the asset cache: while playing, an update waits there to be reloaded.
							   Ref<ScriptAsset> script = ScriptAsset::LoadFromFile(found->second);
							   if (!script)
								   return CommandResult::Fail("The script could not be read (see the log for details: console.get)");
							   return CommandResult::Ok({{"handle", IdString(found->first)}, {"path", context.GetAssets().GetMetadata(found->first)->FilePath.generic_string()}, {"source", script->Source}});
						   }});

		registry.Register({"script.update", "Replaces an existing script's source on disk. A running game keeps the code it started with; the change applies from the next play.start (at once when not playing).", {{"path", "string", "Script relative to Assets, or its handle.", true}, {"source", "string", "The new Lua source; the chunk must return a table.", true}}, false, [](EditorContext& context, const json& params) {
							   std::string error;
							   auto found = FindScript(context, params["path"].get<std::string>(), error);
							   if (!found)
								   return CommandResult::Fail(error);
							   const std::string source = params["source"].get<std::string>();
							   if (source.size() > ScriptAsset::MaxSourceBytes)
								   return CommandResult::Fail("The script is too large");
							   if (!WriteFileAtomically(found->second, source))
								   return CommandResult::Fail("Could not write the script file");
							   context.ReloadAssetWhenStopped(found->first);
							   return CommandResult::Ok({{"handle", IdString(found->first)}, {"appliesNow", !context.IsPlaying()}});
						   }});

		registry.Register({"script.eval", "Runs Lua code in the playing scene's script environment and returns the results as text (play mode only).", {{"code", "string", "Lua statement(s) or expression.", true}}, false, [](EditorContext& context, const json& params) {
							   ScriptSystem* scripts = context.GetScriptSystem();
							   if (!scripts)
								   return CommandResult::Fail("Not playing: start play mode first (play.start)");
							   std::string error;
							   std::optional<std::string> result = scripts->Evaluate(params["code"].get<std::string>(), error);
							   if (!result)
								   return CommandResult::Fail(error);
							   return CommandResult::Ok({{"result", *result}});
						   }});

		registry.Register({"script.api", "Every name scripts can use: Table.Function for global tables, Type:Member for Entity, Component and the math types.", {}, false, [](EditorContext& context, const json&) {
							   // The live scene's system when playing; otherwise a temporary one (the listing does not need a scene).
							   if (ScriptSystem* scripts = context.GetScriptSystem())
								   return CommandResult::Ok({{"api", scripts->GetApiNames()}});
							   ScriptSystem scripts(context.GetAssets());
							   return CommandResult::Ok({{"api", scripts.GetApiNames()}});
						   }});

		registry.Register({"script.errors", "Script errors (with tracebacks) recorded in the current play session, oldest first.", {{"clear", "boolean", "Clear the list after returning it.", false}}, false, [](EditorContext& context, const json& params) {
							   json errors = json::array();
							   if (GameSession* game = context.GetGame())
							   {
								   // Across scene switches (Scene.Load) within the session.
								   for (const std::string& error : game->GetScriptErrors())
									   errors.push_back(error);
								   if (params.contains("clear") && params["clear"].get<bool>())
									   game->ClearScriptErrors();
							   }
							   return CommandResult::Ok({{"errors", errors}, {"playing", context.IsPlaying()}});
						   }});

		registry.Register({"prefab.create", "Saves an entity and its descendants as a new .pxprefab in the project and returns its handle.", {{"id", "any", "Root entity id.", true}, {"path", "string", "Destination relative to Assets, e.g. Prefabs/Enemy.pxprefab.", true}}, false, [](EditorContext& context, const json& params) {
							   std::optional<Entity> entity = context.ResolveEntity(params["id"]);
							   if (!entity)
								   return CommandResult::Fail("Entity not found");
							   std::string error;
							   std::optional<std::filesystem::path> path = NewAssetPath(context, params["path"].get<std::string>(), ".pxprefab", error);
							   if (!path)
								   return CommandResult::Fail(error);
							   Ref<PrefabAsset> prefab = PrefabAsset::CreateFromEntity(context.GetActiveScene(), *entity);
							   if (!prefab->SaveToFile(*path))
								   return CommandResult::Fail("Could not write the prefab file");
							   const AssetHandle handle = ImportNewFile(context, *path);
							   if (!handle.IsValid())
								   return CommandResult::Fail("Import failed (see the log for details: console.get)");
							   return CommandResult::Ok({{"handle", IdString(handle)}, {"entities", prefab->GetEntityCount()}});
						   }});

		registry.Register({"model.create_prefab",
						   "Turns an imported glTF/GLB model into a prefab with one entity per node (its name, local transform, mesh, camera and light), so a scene exported from Blender can be edited piece by piece. Imports the model first if needed.",
						   {{"model", "string", "Model handle, or path relative to Assets (e.g. Models/Bay.glb).", true},
							{"path", "string", "Destination relative to Assets; default Prefabs/<model name>.pxprefab.", false},
							{"name", "string", "Name of the prefab's root entity; default the model's file name.", false}},
						   false,
						   [](EditorContext& context, const json& params) {
							   if (!context.GetProject())
								   return CommandResult::Fail("No project is open");
							   // Reloading the model while a game runs would change assets under it.
							   if (context.IsPlaying())
								   return CommandResult::Fail("Stop play mode first (play.stop)");
							   for (const char* optional : {"path", "name"})
							   {
								   if (params.contains(optional) && !params[optional].is_string())
									   return CommandResult::Fail(fmt::format("'{}' must be a string", optional));
							   }
							   if (!params["model"].is_string())
								   return CommandResult::Fail("'model' must be a handle or a path, as a string");
							   AssetManager& assets = context.GetAssets();
							   std::string error;

							   // Resolve everything that can fail cheaply before importing anything.
							   const std::filesystem::path requested = params["model"].get<std::string>();
							   AssetHandle handle = ResolveAsset(context, params["model"]);
							   std::filesystem::path registryPath;
							   if (const AssetMetadata* metadata = handle.IsValid() ? assets.GetMetadata(handle) : nullptr)
								   registryPath = metadata->FilePath;
							   else
								   registryPath = requested;
							   std::string extension = registryPath.extension().string();
							   std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							   if (extension != ".gltf" && extension != ".glb")
								   return CommandResult::Fail("Not a model: expected a .gltf or .glb file in the project, or its handle");
							   const std::string stem = registryPath.stem().string();
							   const std::string destination = params.contains("path") ? params["path"].get<std::string>() : "Prefabs/" + stem + ".pxprefab";
							   std::optional<std::filesystem::path> path = NewAssetPath(context, destination, ".pxprefab", error);
							   if (!path)
								   return CommandResult::Fail(error);

							   // Not registered yet: import it, if it is a model file inside the project.
							   if (!handle.IsValid())
							   {
								   std::optional<std::filesystem::path> file = context.ResolveProjectPath(requested, error);
								   if (!file)
									   return CommandResult::Fail(error);
								   handle = assets.ImportAsset(*file);
								   // The log says why (a missing file, an unsupported type, a registry that cannot be saved).
								   if (!handle.IsValid())
									   return CommandResult::Fail("Import failed (see the log for details: console.get)");
							   }
							   const AssetMetadata* metadata = handle.IsValid() ? assets.GetMetadata(handle) : nullptr;
							   if (!metadata || metadata->Type != AssetType::Mesh || metadata->Parent.IsValid() || metadata->FilePath.empty())
								   return CommandResult::Fail("Not an imported model file (a .gltf or .glb in the project)");
							   std::optional<std::filesystem::path> source = context.ResolveProjectPath(metadata->FilePath, error);
							   if (!source)
								   return CommandResult::Fail(error);
							   MeshImportResult model;
							   if (!MeshImporter::Import(*source, handle, model, error))
								   return CommandResult::Fail(error);

							   // A model imported before per-node meshes and clips existed (or changed on disk since) lacks their registry
							   // entries: reloading it registers what the file produces now.
							   auto registered = [&] {
								   return std::all_of(model.NodeMeshes.begin(), model.NodeMeshes.end(), [&](const Ref<MeshAsset>& mesh) { return assets.IsRegistered(mesh->Handle); }) &&
										  std::all_of(model.Animations.begin(), model.Animations.end(), [&](const ImportedAnimation& animation) { return assets.IsRegistered(animation.Clip->Handle); });
							   };
							   if (!registered() && (!assets.ReloadAsset(handle) || !registered()))
								   return CommandResult::Fail("The model's meshes could not be registered (did the file change while importing?)");

							   const std::string rootName = params.contains("name") && !params["name"].get<std::string>().empty() ? params["name"].get<std::string>() : stem;
							   std::vector<std::string> warnings = model.Warnings;
							   Ref<PrefabAsset> prefab = BuildModelPrefab(model, rootName, error, &warnings);
							   if (!prefab)
								   return CommandResult::Fail(error);
							   if (prefab->ToJson().dump(1, '\t').size() + 1 > PrefabAsset::MaxFileBytes)
								   return CommandResult::Fail(fmt::format("The model has too many objects for one prefab ({} entities)", prefab->GetEntityCount()));
							   std::error_code directoryError;
							   std::filesystem::create_directories(path->parent_path(), directoryError);
							   if (!prefab->SaveToFile(*path))
								   return CommandResult::Fail("Could not write the prefab file");
							   const AssetHandle prefabHandle = ImportNewFile(context, *path);
							   if (!prefabHandle.IsValid())
								   return CommandResult::Fail("Import failed (see the log for details: console.get)");

							   size_t meshes = 0, cameras = 0, lights = 0;
							   for (const ImportedNode& node : model.Nodes)
							   {
								   meshes += node.Mesh.IsValid() ? 1 : 0;
								   cameras += node.Camera ? 1 : 0;
								   lights += node.Light ? 1 : 0;
							   }
							   return CommandResult::Ok({{"handle", IdString(prefabHandle)},
														 {"model", IdString(handle)},
														 {"entities", prefab->GetEntityCount()},
														 {"meshes", meshes},
														 {"cameras", cameras},
														 {"lights", lights},
														 {"animations", model.Animations.size()},
														 {"warnings", warnings}});
						   }});

		registry.Register({"prefab.instantiate", "Creates an instance of a prefab (by handle or asset path), optionally under a parent and at a position.", {{"prefab", "string", "Prefab handle or path relative to Assets.", true}, {"parent", "any", "Parent entity id.", false}, {"position", "array", "Local position [x, y, z].", false}}, true, [](EditorContext& context, const json& params) {
							   const AssetHandle handle = ResolveAsset(context, params["prefab"]);
							   Ref<PrefabAsset> prefab = handle.IsValid() ? context.GetAssets().GetAsset<PrefabAsset>(handle) : nullptr;
							   if (!prefab)
								   return CommandResult::Fail("No prefab with that handle or path");
							   std::optional<Entity> parent;
							   if (params.contains("parent") && !params["parent"].is_null())
							   {
								   parent = context.ResolveEntity(params["parent"]);
								   if (!parent)
									   return CommandResult::Fail("Parent entity not found");
							   }
							   std::optional<glm::vec3> position;
							   if (params.contains("position"))
							   {
								   const json& value = params["position"];
								   if (value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number())
									   return CommandResult::Fail("position must be [x, y, z]");
								   position = glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
								   if (!std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z))
									   return CommandResult::Fail("position must be finite");
							   }
							   Entity instance = prefab->Instantiate(context.GetActiveScene(), parent, position);
							   context.Select(instance.GetUUID());
							   return CommandResult::Ok({{"id", IdString(instance.GetUUID())}});
						   }});
	}

}
