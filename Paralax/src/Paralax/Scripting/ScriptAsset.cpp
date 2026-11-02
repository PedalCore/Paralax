#include "pxpch.h"
#include "Paralax/Scripting/ScriptAsset.h"

#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Scene/SceneSerializer.h"

namespace Paralax {

	Ref<ScriptAsset> ScriptAsset::LoadFromFile(const std::filesystem::path& path)
	{
		auto script = CreateRef<ScriptAsset>();
		std::string error;
		if (!ReadTextFile(path, script->Source, error, MaxSourceBytes))
		{
			PX_CORE_ERROR("Cannot load script '{}': {}", ToUtf8(path), error);
			return nullptr;
		}
		script->Name = ToUtf8(path.filename());
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
