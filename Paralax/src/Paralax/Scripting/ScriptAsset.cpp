#include "pxpch.h"
#include "Paralax/Scripting/ScriptAsset.h"

#include "Paralax/Scene/SceneSerializer.h"

namespace Paralax {

	Ref<ScriptAsset> ScriptAsset::LoadFromFile(const std::filesystem::path& path)
	{
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error || size > MaxSourceBytes)
		{
			PX_CORE_ERROR("Cannot load script '{}': {}", path.string(), error ? error.message() : "the file is too large");
			return nullptr;
		}
		auto script = CreateRef<ScriptAsset>();
		if (!ReadTextFile(path, script->Source))
		{
			PX_CORE_ERROR("Cannot open script '{}'", path.string());
			return nullptr;
		}
		script->Name = path.filename().string();
		return script;
	}

	Ref<ScriptAsset> ScriptAsset::Create(std::string name, std::string source)
	{
		auto script = CreateRef<ScriptAsset>();
		script->Name = std::move(name);
		script->Source = std::move(source);
		return script;
	}

}
