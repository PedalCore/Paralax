#include "pxpch.h"
#include "Paralax/Asset/MaterialSerializer.h"

#include "Paralax/Core/JsonText.h"
#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Scene/SceneSerializer.h"

namespace Paralax {

	using json = nlohmann::json;
	using Reflection::FieldOptions;
	using Reflection::MakeField;

	std::span<const FieldInfo> MaterialSerializer::GetMaterialFields()
	{
		static const std::vector<FieldInfo> s_Fields = {
			MakeField<&MaterialAsset::BaseColorFactor>("BaseColor", FieldOptions().Color().Describe("Linear base color (albedo) and alpha.")),
			MakeField<&MaterialAsset::BaseColorTexture>("BaseColorTexture"),
			MakeField<&MaterialAsset::MetallicFactor>("Metallic", FieldOptions().Range(0.0f, 1.0f)),
			MakeField<&MaterialAsset::RoughnessFactor>("Roughness", FieldOptions().Range(0.0f, 1.0f)),
			MakeField<&MaterialAsset::MetallicRoughnessTexture>("MetallicRoughnessTexture", FieldOptions().Describe("Roughness in G, metallic in B.")),
			MakeField<&MaterialAsset::NormalTexture>("NormalTexture"),
			MakeField<&MaterialAsset::NormalScale>("NormalScale", FieldOptions().Range(0.0f, 10.0f)),
			MakeField<&MaterialAsset::OcclusionTexture>("OcclusionTexture"),
			MakeField<&MaterialAsset::OcclusionStrength>("OcclusionStrength", FieldOptions().Range(0.0f, 1.0f)),
			MakeField<&MaterialAsset::EmissiveFactor>("Emissive", FieldOptions().Color()),
			MakeField<&MaterialAsset::EmissiveStrength>("EmissiveStrength", FieldOptions().Range(0.0f, 10000.0f)),
			MakeField<&MaterialAsset::EmissiveTexture>("EmissiveTexture"),
			MakeField<&MaterialAsset::HeightTexture>("HeightTexture", FieldOptions().Describe("Height map for parallax occlusion mapping (R, 1 = top).")),
			MakeField<&MaterialAsset::HeightScale>("HeightScale", FieldOptions().Range(0.0f, 0.5f)),
			MakeField<&MaterialAsset::Alpha>("AlphaMode", FieldOptions().Enum({"Opaque", "Mask", "Blend"})),
			MakeField<&MaterialAsset::AlphaCutoff>("AlphaCutoff", FieldOptions().Range(0.0f, 1.0f)),
			MakeField<&MaterialAsset::DoubleSided>("DoubleSided"),
		};
		return s_Fields;
	}

	json MaterialSerializer::Serialize(const MaterialAsset& material)
	{
		json document = Reflection::ToJson(GetMaterialFields(), &material);
		document["Paralax"] = {{"Type", "Material"}, {"Version", CurrentVersion}};
		return document;
	}

	bool MaterialSerializer::Deserialize(MaterialAsset& material, const json& document, std::string& outError)
	{
		outError.clear();
		// Type-checked reads only: nlohmann's value() throws on mismatched types.
		const json* header = document.is_object() && document.contains("Paralax") ? &document["Paralax"] : nullptr;
		if (!header || !header->is_object() || !header->contains("Type") || !(*header)["Type"].is_string() || (*header)["Type"].get<std::string>() != "Material")
		{
			outError = "Not a Paralax material (missing \"Paralax\": { \"Type\": \"Material\" })";
			return false;
		}
		std::string foundVersion;
		if (!ReadDocumentVersion(*header, CurrentVersion, foundVersion))
		{
			outError = fmt::format("Unsupported material version: expected 1-{}, got {}", CurrentVersion, foundVersion);
			return false;
		}

		json fields = document;
		fields.erase("Paralax");
		Reflection::FromJson(GetMaterialFields(), "Material", &material, fields, outError);
		return true;
	}

	bool MaterialSerializer::SaveToFile(const MaterialAsset& material, const std::filesystem::path& path)
	{
		return WriteFileAtomically(path, FormatJsonDocument(Serialize(material)));
	}

	Ref<MaterialAsset> MaterialSerializer::LoadFromFile(const std::filesystem::path& path)
	{
		std::string text;
		std::string error;
		if (!ReadTextFile(path, text, error))
		{
			PX_CORE_ERROR("Cannot read material '{}': {}", ToUtf8(path), error);
			return nullptr;
		}
		if (JsonExceedsDepth(text, MaxJsonDepth))
		{
			PX_CORE_ERROR("Material '{}' nests too deeply", ToUtf8(path));
			return nullptr;
		}
		const json document = json::parse(text, nullptr, false);
		if (document.is_discarded())
		{
			PX_CORE_ERROR("Material '{}' is not valid JSON", ToUtf8(path));
			return nullptr;
		}

		auto material = CreateRef<MaterialAsset>();
		if (!Deserialize(*material, document, error))
		{
			PX_CORE_ERROR("Cannot load material '{}': {}", ToUtf8(path), error);
			return nullptr;
		}
		if (!error.empty())
			PX_CORE_WARN("Material '{}': {}", ToUtf8(path), error);
		return material;
	}

}
