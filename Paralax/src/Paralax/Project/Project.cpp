#include "pxpch.h"
#include "Paralax/Project/Project.h"

#include "Paralax/Core/JsonText.h"
#include "Paralax/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <charconv>

namespace Paralax {

	using json = nlohmann::json;

	namespace {

		bool IsValidProjectName(const std::string& name)
		{
			if (name.empty() || name.size() > 64)
				return false;
			for (char c : name)
			{
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ';
				if (!ok)
					return false;
			}
			return true;
		}

	}

	Ref<Project> Project::Create(const std::filesystem::path& directory, const std::string& name)
	{
		if (!IsValidProjectName(name))
		{
			PX_CORE_ERROR("Invalid project name '{}' (letters, digits, space, '_' and '-', up to 64 characters)", name);
			return nullptr;
		}

		std::error_code error;
		if (std::filesystem::exists(directory, error))
		{
			for (const auto& entry : std::filesystem::directory_iterator(directory, error))
			{
				if (entry.path().extension() == FileExtension)
				{
					PX_CORE_ERROR("'{}' already contains a project", directory.string());
					return nullptr;
				}
			}
		}
		std::filesystem::create_directories(directory / AssetDirectoryName / "Scenes", error);
		if (error)
		{
			PX_CORE_ERROR("Cannot create project directory '{}': {}", directory.string(), error.message());
			return nullptr;
		}

		Ref<Project> project(new Project());
		project->m_Name = name;
		project->m_Directory = std::filesystem::weakly_canonical(directory, error);
		if (error)
		{
			PX_CORE_ERROR("Cannot resolve project directory '{}': {}", directory.string(), error.message());
			return nullptr;
		}
		project->m_ProjectFile = project->m_Directory / (name + FileExtension);
		project->m_Assets = CreateScope<AssetManager>(project->GetAssetDirectory());
		if (!project->Save())
			return nullptr;
		return project;
	}

	Ref<Project> Project::Load(const std::filesystem::path& path)
	{
		std::error_code error;
		std::filesystem::path file = path;
		if (std::filesystem::is_directory(path, error))
		{
			// Directory iteration order is unspecified, so more than one project file would load nondeterministically.
			std::vector<std::filesystem::path> candidates;
			for (const auto& entry : std::filesystem::directory_iterator(path, error))
			{
				if (entry.path().extension() == FileExtension)
					candidates.push_back(entry.path());
			}
			if (candidates.size() != 1)
			{
				PX_CORE_ERROR("{} {} files in '{}'; open the project file directly", candidates.empty() ? "No" : "Several", FileExtension, path.string());
				return nullptr;
			}
			file = candidates.front();
		}

		std::string text;
		if (!ReadTextFile(file, text))
		{
			PX_CORE_ERROR("Cannot read project '{}'", file.string());
			return nullptr;
		}
		if (JsonExceedsDepth(text, MaxJsonDepth))
		{
			PX_CORE_ERROR("Project '{}' nests too deeply", file.string());
			return nullptr;
		}
		const json document = json::parse(text, nullptr, false);
		const json* header = document.is_object() && document.contains("Paralax") ? &document["Paralax"] : nullptr;
		if (!header || !header->is_object() || !header->contains("Type") || !(*header)["Type"].is_string() || (*header)["Type"].get<std::string>() != "Project")
		{
			PX_CORE_ERROR("'{}' is not a Paralax project", file.string());
			return nullptr;
		}
		const int version = header->contains("Version") && (*header)["Version"].is_number_integer() ? (*header)["Version"].get<int>() : 0;
		if (version < 1 || version > CurrentVersion)
		{
			PX_CORE_ERROR("Unsupported project version {} in '{}'", version, file.string());
			return nullptr;
		}

		Ref<Project> project(new Project());
		project->m_Name = file.stem().string();
		if (document.contains("Name") && document["Name"].is_string() && IsValidProjectName(document["Name"].get<std::string>()))
			project->m_Name = document["Name"].get<std::string>();
		project->m_ProjectFile = std::filesystem::weakly_canonical(file, error);
		if (error)
		{
			PX_CORE_ERROR("Cannot resolve project file '{}': {}", file.string(), error.message());
			return nullptr;
		}
		project->m_Directory = project->m_ProjectFile.parent_path();
		if (document.contains("StartScene") && document["StartScene"].is_string())
		{
			const std::string& value = document["StartScene"].get_ref<const std::string&>();
			uint64_t handle = 0;
			auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), handle);
			if (ec == std::errc() && ptr == value.data() + value.size())
				project->m_StartScene = AssetHandle(handle);
		}
		project->m_Assets = CreateScope<AssetManager>(project->GetAssetDirectory());
		return project;
	}

	bool Project::Save() const
	{
		const json document = {
			{"Paralax", {{"Type", "Project"}, {"Version", CurrentVersion}}},
			{"Name", m_Name},
			{"StartScene", std::to_string(static_cast<uint64_t>(m_StartScene))},
		};
		return WriteFileAtomically(GetProjectFile(), document.dump(1, '\t') + "\n");
	}

}
