#include "EditorCore/EditorContext.h"

#include <Paralax/Project/Project.h>

namespace Paralax::Editor {

	using json = nlohmann::json;

	void RegisterProjectCommands(CommandRegistry& registry)
	{
		registry.Register({"project.info", "The open project: name, directories and start scene (fails without a project).", {}, false, [](EditorContext& context, const json&) {
							   const Project* project = context.GetProject();
							   if (!project)
								   return CommandResult::Fail("No project is open");
							   json startScene = nullptr;
							   if (const AssetMetadata* metadata = project->GetAssetManager().GetMetadata(project->GetStartScene()))
								   startScene = metadata->FilePath.generic_string();
							   return CommandResult::Ok({{"name", project->GetName()}, {"directory", project->GetDirectory().generic_string()}, {"assets", project->GetAssetDirectory().generic_string()}, {"startScene", startScene}});
						   }});

		registry.Register({"project.set_start_scene", "Sets the scene the game starts with (a saved .pxscene, relative to Assets) and saves the project.", {{"path", "string", "Scene file relative to Assets, e.g. Scenes/Main.pxscene.", true}}, false, [](EditorContext& context, const json& params) {
							   Project* project = context.GetProject();
							   if (!project)
								   return CommandResult::Fail("No project is open");
							   std::string error;
							   std::optional<std::filesystem::path> path = context.ResolveProjectPath(params["path"].get<std::string>(), error);
							   if (!path)
								   return CommandResult::Fail(error);
							   if (AssetTypeFromExtension(*path) != AssetType::Scene)
								   return CommandResult::Fail("The start scene must be a .pxscene file");
							   std::error_code exists;
							   if (!std::filesystem::exists(*path, exists))
								   return CommandResult::Fail("The scene file does not exist (save the scene first)");
							   const AssetHandle handle = project->GetAssetManager().ImportAsset(*path);
							   if (!handle.IsValid())
								   return CommandResult::Fail("Could not register the scene");
							   project->SetStartScene(handle);
							   if (!project->Save())
								   return CommandResult::Fail("Could not save the project file");
							   return CommandResult::Ok({{"handle", std::to_string(static_cast<uint64_t>(handle))}});
						   }});
	}

}
