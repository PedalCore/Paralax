#pragma once

#include "Paralax/Asset/MaterialAsset.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Core/Reflection.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <span>

namespace Paralax {

	// Material <-> JSON (".pxmat"): { "Paralax": { "Type": "Material", "Version": 1 }, "<Field>": value, ... }.
	// Field names are those of GetMaterialFields(), which the inspector, scripts and agents also use.
	class MaterialSerializer
	{
	public:
		static constexpr int CurrentVersion = 1;

		static std::span<const FieldInfo> GetMaterialFields();

		static nlohmann::json Serialize(const MaterialAsset& material);
		// Applies the document to the material. Unknown or invalid fields are reported in outError but the valid
		// ones are applied; returns false if the document is not a material.
		static bool Deserialize(MaterialAsset& material, const nlohmann::json& document, std::string& outError);

		static bool SaveToFile(const MaterialAsset& material, const std::filesystem::path& path);
		// Returns nullptr (with an error logged) if the file cannot be read or is not a material.
		static Ref<MaterialAsset> LoadFromFile(const std::filesystem::path& path);
	};

}
