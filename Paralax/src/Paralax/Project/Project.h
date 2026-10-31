#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Core/Base.h"

#include <filesystem>
#include <string>

namespace Paralax {

	// A game project: a directory containing "<Name>.pxproj" and an "Assets" folder (scenes, meshes, textures,
	// materials, scripts, sounds), plus its AssetManager. Main-thread-only.
	//
	// Project file: { "Paralax": { "Type": "Project", "Version": 1 }, "Name": "...", "StartScene": "<asset handle>" }
	class Project
	{
	public:
		static constexpr int CurrentVersion = 1;
		static constexpr const char* AssetDirectoryName = "Assets";
		static constexpr const char* FileExtension = ".pxproj";

		// Creates a new project directory (which must not already contain a project). Returns nullptr on failure.
		static Ref<Project> Create(const std::filesystem::path& directory, const std::string& name);
		// Loads a project from its .pxproj file or its directory (which must contain exactly one). Returns nullptr
		// (with an error logged) on failure. Save() writes back to the file that was loaded.
		static Ref<Project> Load(const std::filesystem::path& path);

		bool Save() const;

		const std::string& GetName() const { return m_Name; }
		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		std::filesystem::path GetAssetDirectory() const { return m_Directory / AssetDirectoryName; }
		const std::filesystem::path& GetProjectFile() const { return m_ProjectFile; }

		AssetHandle GetStartScene() const { return m_StartScene; }
		void SetStartScene(AssetHandle scene) { m_StartScene = scene; }

		AssetManager& GetAssetManager() { return *m_Assets; }
		const AssetManager& GetAssetManager() const { return *m_Assets; }

	private:
		Project() = default;

	private:
		std::string m_Name;
		std::filesystem::path m_Directory;
		std::filesystem::path m_ProjectFile;
		AssetHandle m_StartScene = 0;
		Scope<AssetManager> m_Assets;
	};

}
