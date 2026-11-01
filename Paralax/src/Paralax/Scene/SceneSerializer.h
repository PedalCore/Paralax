#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Paralax {

	// Scene <-> JSON (".pxscene"). Format:
	//   { "Paralax": { "Type": "Scene", "Version": 1 }, "Name": "...",
	//     "Entities": [ { "ID": "<uuid>", "Parent": "<uuid or 0>", "Components": { "<Name>": { fields } } } ] }
	// Entities are written depth-first in hierarchy order, so sibling order round-trips. UUIDs are strings.
	//
	// Loading is tolerant of unknown components/fields and invalid values (each produces a warning and is
	// skipped) but rejects structurally invalid documents, unsupported versions and duplicate UUIDs.
	class SceneSerializer
	{
	public:
		static constexpr int CurrentVersion = 1;

		// documentType is the header's "Type": "Scene" for scenes, "Prefab" for prefabs (same format otherwise).
		static nlohmann::json Serialize(Scene& scene, std::string_view documentType = "Scene");
		// Loads into an empty scene. Returns false (with outError set) if the document cannot be loaded or is not of
		// the expected type; the scene is then left empty. Warnings about skipped data are appended to outWarnings.
		static bool Deserialize(Scene& scene, const nlohmann::json& document, std::string& outError, std::vector<std::string>* outWarnings = nullptr,
								std::string_view expectedType = "Scene");

		// Writes atomically (temporary file + rename) so a crash never leaves a truncated scene.
		static bool SaveToFile(Scene& scene, const std::filesystem::path& path);
		// Returns nullptr (with an error logged) on failure; warnings are logged.
		static Ref<Scene> LoadFromFile(const std::filesystem::path& path);
	};

	// Writes text to a file atomically: a temporary sibling file is written, flushed and renamed over the target.
	bool WriteFileAtomically(const std::filesystem::path& path, const std::string& contents);
	// Reads a whole file. Returns false if it cannot be opened.
	bool ReadTextFile(const std::filesystem::path& path, std::string& outContents);

}
