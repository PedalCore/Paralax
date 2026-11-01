#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"

#include <filesystem>
#include <string>

namespace Paralax {

	// Lua source code (".lua"). The script is compiled when a scene starts running (see ScriptSystem), so loading
	// only reads the text. Main-thread-only.
	class ScriptAsset : public Asset
	{
	public:
		static constexpr size_t MaxSourceBytes = 4ull * 1024 * 1024;

		AssetType GetType() const override { return AssetType::Script; }

		// Returns nullptr (with an error logged) if the file cannot be read or exceeds MaxSourceBytes.
		static Ref<ScriptAsset> LoadFromFile(const std::filesystem::path& path);
		// A script that exists only in memory (tests, generated scripts). name is used in error messages.
		static Ref<ScriptAsset> Create(std::string name, std::string source);

		// Shown in error messages and tracebacks, e.g. "Scripts/Player.lua".
		std::string Name;
		std::string Source;
	};

}
