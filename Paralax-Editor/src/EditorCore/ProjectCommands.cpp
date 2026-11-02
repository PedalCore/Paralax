#include "EditorCore/EditorContext.h"

#include <Paralax/Core/EngineResources.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Project/Project.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string_view>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		// Marks a directory as a Paralax export and lists the files it wrote (one generic relative path per line after
		// the header, in UTF-8 on every platform, as ToUtf8 writes them). Only a marked directory is ever replaced,
		// and only if it holds nothing but those files.
		constexpr const char* ExportMarker = ".paralax-export";
		constexpr std::string_view MarkerHeader = "Paralax export";
		// Siblings of the output used while exporting: the new export is assembled in the staging directory and the
		// previous one is moved aside, so a failure never leaves a half-written export in place.
		constexpr const char* StagingSuffix = ".paralax-staging";
		constexpr const char* RetiredSuffix = ".paralax-old";

		bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			std::error_code error;
			const bool same = std::filesystem::equivalent(a, b, error);
			return !error && same;
		}

		// True if inner is outer or lies inside it. Besides comparing names, every existing ancestor of inner is
		// compared with outer by identity, so links, mounts and case-insensitive file systems cannot hide an overlap.
		bool IsWithin(const std::filesystem::path& inner, const std::filesystem::path& outer)
		{
			const std::filesystem::path relative = inner.lexically_relative(outer);
			if (!relative.empty() && *relative.begin() != "..")
				return true;
			for (std::filesystem::path current = inner; !current.empty(); current = current.parent_path())
			{
				if (SameFile(current, outer))
					return true;
				if (current == current.parent_path())
					break;
			}
			return false;
		}

		// Reads an export marker: the files it lists, or nullopt if path holds no marker (a link is not one).
		std::optional<std::set<std::string>> ReadMarker(const std::filesystem::path& directory)
		{
			std::error_code error;
			const std::filesystem::path marker = directory / ExportMarker;
			if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(marker, error)))
				return std::nullopt;
			std::ifstream file(marker, std::ios::binary);
			std::string line;
			if (!std::getline(file, line) || line != MarkerHeader)
				return std::nullopt;
			std::set<std::string> files;
			while (std::getline(file, line))
			{
				if (!line.empty())
					files.insert(line);
			}
			return files;
		}

		bool WriteMarker(const std::filesystem::path& directory, const std::vector<std::string>& files)
		{
			std::string contents = std::string(MarkerHeader) + "\n";
			for (const std::string& file : files)
				contents += file + "\n";
			// No document limit: the marker lists every exported file, so it grows with the project, and ReadMarker
			// reads it line by line, never whole.
			return WriteFileAtomically(directory / ExportMarker, contents, std::numeric_limits<size_t>::max());
		}

		// A previous export may be replaced only if everything in it is what the export wrote, or logs the game wrote
		// while running from there. Anything else (a save game, notes) belongs to the user.
		bool CheckPreviousExport(const std::filesystem::path& directory, const std::set<std::string>& exported, std::string& outError)
		{
			std::error_code error;
			for (auto it = std::filesystem::recursive_directory_iterator(directory, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
			{
				const std::filesystem::path relative = it->path().lexically_relative(directory);
				if (*relative.begin() == "Logs")
				{
					it.disable_recursion_pending();
					continue;
				}
				const std::filesystem::file_status status = it->symlink_status(error);
				if (error)
					break;
				if (std::filesystem::is_directory(status) || relative == ExportMarker)
					continue;
				if (!exported.contains(ToUtf8(relative)))
				{
					outError = fmt::format("The output directory holds '{}', which the previous export did not write; move it or choose another directory", ToUtf8(relative));
					return false;
				}
			}
			if (error)
			{
				outError = fmt::format("Could not inspect the previous export: {}", error.message());
				return false;
			}
			return true;
		}

		// Removes a leftover staging or retired directory from an interrupted export (it carries a marker); anything
		// else in that place is not ours and stops the export.
		bool ClearLeftover(const std::filesystem::path& path, std::string& outError)
		{
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
			if (!std::filesystem::exists(status))
				return true;
			if (!std::filesystem::is_directory(status) || !ReadMarker(path))
			{
				outError = fmt::format("'{}' is in the way of the export; move or delete it", ToUtf8(path));
				return false;
			}
			std::filesystem::remove_all(path, error);
			if (error)
			{
				outError = fmt::format("Could not remove '{}' left by an earlier export: {}", ToUtf8(path), error.message());
				return false;
			}
			return true;
		}

		// Copies the regular files of a tree (links and special files are skipped, so nothing outside it is copied),
		// recording each file's path under the export root in outFiles.
		bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to, const std::filesystem::path& root, std::vector<std::string>& outFiles,
					  std::string& outError)
		{
			std::error_code error;
			std::filesystem::create_directories(to, error);
			if (!error)
			{
				for (auto it = std::filesystem::recursive_directory_iterator(from, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
				{
					const std::filesystem::file_status status = it->symlink_status(error);
					if (error)
						break;
					const std::filesystem::path target = to / it->path().lexically_relative(from);
					if (std::filesystem::is_directory(status))
						std::filesystem::create_directories(target, error);
					else if (std::filesystem::is_regular_file(status))
					{
						std::filesystem::copy_file(it->path(), target, error);
						outFiles.push_back(ToUtf8(target.lexically_relative(root)));
					}
					if (error)
						break;
				}
			}
			if (error)
			{
				outError = fmt::format("Could not copy '{}': {}", ToUtf8(from), error.message());
				return false;
			}
			return true;
		}

		// The game executable's file name: the project name with anything but letters, digits, '-' and '_' replaced,
		// renamed if it would clash with the export's folders or a name Windows reserves.
		std::string ExecutableName(const std::string& projectName)
		{
			std::string name;
			for (char c : projectName)
				name += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
			std::string lower;
			for (char c : name)
				lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
			static const std::set<std::string> reserved = {"assets", "resources", "logs", "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
														   "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
			if (name.empty())
				return "Game";
			return reserved.contains(lower) ? name + "_Game" : name;
		}

		CommandResult ExportProject(EditorContext& context, const json& params)
		{
			Project* project = context.GetProject();
			if (!project)
				return CommandResult::Fail("No project is open");
			if (!project->GetAssetManager().GetMetadata(project->GetStartScene()))
				return CommandResult::Fail("The project has no start scene (project.set_start_scene)");

			std::error_code error;
			const std::filesystem::path projectDirectory = std::filesystem::weakly_canonical(project->GetDirectory(), error);
			if (error)
				return CommandResult::Fail(fmt::format("Could not resolve the project directory: {}", error.message()));
			// Relative paths are taken from the project directory, so "../Build" is beside the project.
			const std::filesystem::path requested = FromUtf8(params["output"].get<std::string>());
			if (requested.empty())
				return CommandResult::Fail("Invalid output directory");
			std::filesystem::path output = std::filesystem::weakly_canonical(requested.is_absolute() ? requested : projectDirectory / requested, error);
			// For a directory that does not exist yet, weakly_canonical keeps a trailing separator ("Build/", as shells
			// complete names, or "Build//" and "Build/."), leaving no name for the checks below: the directory meant is
			// the one before it. A root has no name however it is written, and stays refused.
			if (!error && !output.has_filename() && output.has_relative_path())
				output = output.parent_path();
			if (error || !output.has_filename() || output.relative_path().empty())
				return CommandResult::Fail("Invalid output directory");
			// The export creates one directory at most: a typo cannot scatter folders across the disk.
			if (!std::filesystem::is_directory(output.parent_path(), error))
				return CommandResult::Fail(fmt::format("The directory '{}' that would contain the export does not exist", ToUtf8(output.parent_path())));
			if (IsWithin(output, projectDirectory) || IsWithin(projectDirectory, output))
				return CommandResult::Fail("The output directory must not be inside the project or contain it");

			const std::filesystem::path self = GetExecutablePath();
			const std::filesystem::path runtime = self.parent_path() / FromUtf8("Paralax-Runtime" + ToUtf8(self.extension()));
			if (!std::filesystem::is_regular_file(runtime, error))
				return CommandResult::Fail(fmt::format("The runtime was not found at '{}'", ToUtf8(runtime)));
			std::filesystem::path resources = GetEngineResourceDirectory();
			if (!resources.empty())
			{
				resources = std::filesystem::weakly_canonical(std::filesystem::absolute(resources, error), error);
				if (error)
					return CommandResult::Fail(fmt::format("Could not resolve the engine resources directory: {}", error.message()));
				if (IsWithin(output, resources) || IsWithin(resources, output))
					return CommandResult::Fail("The output directory must not overlap the engine resources");
			}

			// Only a new or empty directory, or a previous export holding nothing else, is replaced.
			const std::filesystem::file_status status = std::filesystem::symlink_status(output, error);
			const bool exists = std::filesystem::exists(status);
			bool previousExport = false;
			std::string checkError;
			if (exists)
			{
				if (!std::filesystem::is_directory(status))
					return CommandResult::Fail("The output path exists and is not a directory");
				const std::optional<std::set<std::string>> exported = ReadMarker(output);
				previousExport = exported.has_value();
				if (previousExport && !CheckPreviousExport(output, *exported, checkError))
					return CommandResult::Fail(checkError);
				if (!previousExport && !std::filesystem::is_empty(output, error))
					return CommandResult::Fail("The output directory is not empty and is not a previous export");
			}
			const std::filesystem::path staging = output.parent_path() / (output.filename().native() + FromUtf8(StagingSuffix).native());
			const std::filesystem::path retired = output.parent_path() / (output.filename().native() + FromUtf8(RetiredSuffix).native());
			if (!ClearLeftover(staging, checkError) || !ClearLeftover(retired, checkError))
				return CommandResult::Fail(checkError);

			// Assemble the export in the staging directory. It is marked first, so an interrupted export is recognised
			// and cleared next time.
			std::filesystem::create_directory(staging, error);
			if (error || !WriteMarker(staging, {}))
				return CommandResult::Fail(fmt::format("Could not create '{}'", ToUtf8(staging)));
			auto abandon = [&](const std::string& message) {
				std::error_code ignored;
				std::filesystem::remove_all(staging, ignored);
				return CommandResult::Fail(message);
			};
			std::vector<std::string> files;
			const std::string executable = ExecutableName(project->GetName()) + ToUtf8(self.extension());
			std::filesystem::copy_file(runtime, staging / FromUtf8(executable), error);
			if (error)
				return abandon(fmt::format("Could not copy the runtime: {}", error.message()));
			files.push_back(executable);
			const std::filesystem::path projectFile = project->GetProjectFile().filename();
			std::filesystem::copy_file(project->GetProjectFile(), staging / projectFile, error);
			if (error)
				return abandon(fmt::format("Could not copy the project file: {}", error.message()));
			files.push_back(ToUtf8(projectFile));
			std::string copyError;
			if (!CopyTree(project->GetAssetDirectory(), staging / Project::AssetDirectoryName, staging, files, copyError))
				return abandon(copyError);
			if (!resources.empty() && std::filesystem::is_directory(resources, error) && !CopyTree(resources, staging / "Resources", staging, files, copyError))
				return abandon(copyError);
			if (!WriteMarker(staging, files))
				return abandon("Could not write the export marker");

			// Swap it into place: the previous export is moved aside first, and restored if the swap fails.
			if (previousExport)
				std::filesystem::rename(output, retired, error);
			else if (exists)
				std::filesystem::remove(output, error); // Empty.
			if (error)
				return abandon(fmt::format("Could not replace '{}': {}", ToUtf8(output), error.message()));
			std::filesystem::rename(staging, output, error);
			if (error)
			{
				std::error_code ignored;
				if (previousExport)
					std::filesystem::rename(retired, output, ignored);
				return abandon(fmt::format("Could not move the export into place: {}", error.message()));
			}

			json result = {{"path", ToUtf8(output)}, {"executable", ToUtf8(output / FromUtf8(executable))}, {"files", files.size()}};
			std::vector<std::string> warnings;
			if (previousExport)
			{
				std::filesystem::remove_all(retired, error);
				if (error)
					warnings.push_back(fmt::format("The previous export, moved to '{}', could not be deleted ({}); the next export removes it", ToUtf8(retired), error.message()));
			}
			if (context.IsSceneDirty())
				warnings.push_back("The open scene has unsaved changes; the export contains the saved files");
			if (!warnings.empty())
				result["warnings"] = warnings;
			return CommandResult::Ok(result);
		}

	}

	void RegisterProjectCommands(CommandRegistry& registry)
	{
		registry.Register({"project.export", "Exports the project as a standalone game folder: the runtime (named after the project), the engine resources, the project file and its assets. The folder must be new, empty, or a previous export holding nothing else (which is replaced); its parent must exist.", {{"output", "string", "Destination folder, outside the project (relative paths are taken from the project directory).", true}}, false, ExportProject});

		registry.Register({"project.info", "The open project: name, directories and start scene (fails without a project).", {}, false, [](EditorContext& context, const json&) {
							   const Project* project = context.GetProject();
							   if (!project)
								   return CommandResult::Fail("No project is open");
							   json startScene = nullptr;
							   if (const AssetMetadata* metadata = project->GetAssetManager().GetMetadata(project->GetStartScene()))
								   startScene = ToUtf8(metadata->FilePath);
							   return CommandResult::Ok({{"name", project->GetName()}, {"directory", ToUtf8(project->GetDirectory())}, {"assets", ToUtf8(project->GetAssetDirectory())}, {"startScene", startScene}});
						   }});

		registry.Register({"project.set_start_scene", "Sets the scene the game starts with (a saved .pxscene, relative to Assets) and saves the project.", {{"path", "string", "Scene file relative to Assets, e.g. Scenes/Main.pxscene.", true}}, false, [](EditorContext& context, const json& params) {
							   Project* project = context.GetProject();
							   if (!project)
								   return CommandResult::Fail("No project is open");
							   std::string error;
							   std::optional<std::filesystem::path> path = context.ResolveProjectPath(FromUtf8(params["path"].get<std::string>()), error);
							   if (!path)
								   return CommandResult::Fail(error);
							   if (AssetTypeFromExtension(*path) != AssetType::Scene)
								   return CommandResult::Fail("The start scene must be a .pxscene file");
							   std::error_code exists;
							   if (!std::filesystem::exists(*path, exists))
								   return CommandResult::Fail("The scene file does not exist (save the scene first)");
							   const AssetHandle handle = project->GetAssetManager().ImportAsset(*path);
							   if (!handle.IsValid())
								   return CommandResult::Fail("Could not register the scene (see the log for details: console.get)");
							   project->SetStartScene(handle);
							   if (!project->Save())
								   return CommandResult::Fail("Could not save the project file");
							   return CommandResult::Ok({{"handle", std::to_string(static_cast<uint64_t>(handle))}});
						   }});
	}

}
