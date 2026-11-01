#include "pxpch.h"
#include "Paralax/Asset/Asset.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace Paralax {

	namespace {

		struct TypeName
		{
			AssetType Type;
			std::string_view Name;
		};

		constexpr std::array<TypeName, 9> s_TypeNames = {{
			{AssetType::None, "None"},
			{AssetType::Texture, "Texture"},
			{AssetType::EnvironmentMap, "EnvironmentMap"},
			{AssetType::Material, "Material"},
			{AssetType::Mesh, "Mesh"},
			{AssetType::Scene, "Scene"},
			{AssetType::Prefab, "Prefab"},
			{AssetType::Script, "Script"},
			{AssetType::Audio, "Audio"},
		}};

	}

	std::string_view AssetTypeToString(AssetType type)
	{
		for (const TypeName& entry : s_TypeNames)
		{
			if (entry.Type == type)
				return entry.Name;
		}
		return "None";
	}

	AssetType AssetTypeFromString(std::string_view name)
	{
		for (const TypeName& entry : s_TypeNames)
		{
			if (entry.Name == name)
				return entry.Type;
		}
		return AssetType::None;
	}

	AssetType AssetTypeFromExtension(const std::filesystem::path& path)
	{
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp")
			return AssetType::Texture;
		if (extension == ".hdr")
			return AssetType::EnvironmentMap;
		if (extension == ".pxmat")
			return AssetType::Material;
		if (extension == ".gltf" || extension == ".glb")
			return AssetType::Mesh;
		if (extension == ".pxscene")
			return AssetType::Scene;
		if (extension == ".pxprefab")
			return AssetType::Prefab;
		if (extension == ".lua")
			return AssetType::Script;
		// Formats AudioClip decodes (Ogg Vorbis is not among them).
		if (extension == ".wav" || extension == ".mp3" || extension == ".flac")
			return AssetType::Audio;
		return AssetType::None;
	}

}
