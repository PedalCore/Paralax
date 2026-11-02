#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <limits>
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

		// Writes atomically (temporary file + rename) so a crash never leaves a truncated scene. Refuses a scene larger
		// than LoadFromFile reads (MaxTextFileBytes), leaving any existing file untouched. Returns false (with an
		// error logged) on failure.
		static bool SaveToFile(Scene& scene, const std::filesystem::path& path);
		// Returns nullptr (with an error logged) on failure; warnings are logged.
		static Ref<Scene> LoadFromFile(const std::filesystem::path& path);
	};

	// The most bytes a text document the engine reads whole (scene, prefab, material, project, asset registry) may
	// have. Real documents are a few megabytes at most; anything near this is hostile or broken, and the limit stops
	// a loader from allocating an arbitrary amount of memory before it can refuse the file.
	inline constexpr size_t MaxTextFileBytes = 64ull * 1024 * 1024;

	// Writes text to a file atomically: a temporary sibling file is written, flushed and renamed over the target.
	// Contents longer than maxBytes are refused before anything is written, so any existing file is left as it was.
	// Writers of documents that are read back pass the reader's limit, so they never write a file that cannot be
	// opened again. Returns false (with an error logged) on failure.
	bool WriteFileAtomically(const std::filesystem::path& path, const std::string& contents, size_t maxBytes = std::numeric_limits<size_t>::max());
	// Reads a whole file of at most maxBytes into outContents. Returns false, describing why in outError, if the
	// file is missing, larger than maxBytes (checked before anything is read) or cannot be read.
	bool ReadTextFile(const std::filesystem::path& path, std::string& outContents, std::string& outError, size_t maxBytes = MaxTextFileBytes);

}
