#pragma once

#include "Paralax/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Lightweight field reflection: named, typed fields of plain structs (components, materials, settings) that can be
// read and written as JSON. Drives serialization, the editor inspector, scripting and the agent API.

namespace Paralax {

	// Field types understood by the reflection system. JSON representations are noted.
	enum class FieldType
	{
		Bool,       // true/false
		Int,        // integer
		Float,      // number
		Vec2,       // [x, y]
		Vec3,       // [x, y, z]
		Vec4,       // [x, y, z, w]
		Quat,       // [x, y, z, w]
		String,     // string
		Asset,      // string (decimal UUID); "0" for none
		AssetArray, // array of asset handle strings
		Enum        // string (one of EnumNames)
	};

	enum FieldFlags : uint32_t
	{
		FieldFlagNone = 0,
		FieldFlagColor = 1u << 0, // Vec3/Vec4 shown as a color picker
		FieldFlagAngle = 1u << 1, // Float stored in radians, shown in degrees
	};

	struct FieldInfo
	{
		std::string Name;
		FieldType Type = FieldType::Float;
		uint32_t Flags = FieldFlagNone;
		// Inclusive editor/validation range for numeric fields (ignored when Min >= Max).
		float Min = 0.0f;
		float Max = 0.0f;
		std::vector<std::string> EnumNames; // Enum fields: index == underlying value.
		std::string Description;

		// Returns a pointer to this field inside a component instance.
		void* (*Access)(void* component) = nullptr;
	};

	const FieldInfo* FindField(std::span<const FieldInfo> fields, std::string_view name);

	namespace Reflection {

		template<typename T>
		struct MemberPointerTraits;

		template<typename C, typename M>
		struct MemberPointerTraits<M C::*>
		{
			using Class = C;
			using Member = M;
		};

		template<typename T>
		constexpr FieldType DeduceFieldType()
		{
			if constexpr (std::is_same_v<T, bool>)
				return FieldType::Bool;
			else if constexpr (std::is_same_v<T, int>)
				return FieldType::Int;
			else if constexpr (std::is_same_v<T, float>)
				return FieldType::Float;
			else if constexpr (std::is_same_v<T, glm::vec2>)
				return FieldType::Vec2;
			else if constexpr (std::is_same_v<T, glm::vec3>)
				return FieldType::Vec3;
			else if constexpr (std::is_same_v<T, glm::vec4>)
				return FieldType::Vec4;
			else if constexpr (std::is_same_v<T, glm::quat>)
				return FieldType::Quat;
			else if constexpr (std::is_same_v<T, std::string>)
				return FieldType::String;
			else if constexpr (std::is_same_v<T, UUID>)
				return FieldType::Asset;
			else if constexpr (std::is_same_v<T, std::vector<UUID>>)
				return FieldType::AssetArray;
			else if constexpr (std::is_enum_v<T>)
			{
				static_assert(std::is_same_v<std::underlying_type_t<T>, int>, "Reflected enums must have an int underlying type");
				return FieldType::Enum;
			}
			else
				static_assert(sizeof(T) == 0, "Unsupported reflected field type");
		}

		// Accessor for a direct member.
		template<auto Member>
		void* AccessMember(void* component)
		{
			using Class = typename MemberPointerTraits<decltype(Member)>::Class;
			return &(static_cast<Class*>(component)->*Member);
		}

		// Accessor for a member of a member (e.g. CameraComponent::Camera.PerspectiveFOV).
		template<auto Outer, auto Inner>
		void* AccessNestedMember(void* component)
		{
			using Class = typename MemberPointerTraits<decltype(Outer)>::Class;
			return &((static_cast<Class*>(component)->*Outer).*Inner);
		}

		// Optional field metadata, built fluently: FieldOptions().Color().Range(0, 1).Describe("...").
		struct FieldOptions
		{
			uint32_t Flags = FieldFlagNone;
			float Min = 0.0f;
			float Max = 0.0f;
			std::vector<std::string> EnumNames;
			std::string Description;

			FieldOptions& Color()
			{
				Flags |= FieldFlagColor;
				return *this;
			}
			FieldOptions& Angle()
			{
				Flags |= FieldFlagAngle;
				return *this;
			}
			FieldOptions& Range(float min, float max)
			{
				Min = min;
				Max = max;
				return *this;
			}
			FieldOptions& Enum(std::vector<std::string> names)
			{
				EnumNames = std::move(names);
				return *this;
			}
			FieldOptions& Describe(std::string description)
			{
				Description = std::move(description);
				return *this;
			}
		};

		template<auto Member>
		FieldInfo MakeField(std::string name, FieldOptions options = {})
		{
			using MemberType = typename MemberPointerTraits<decltype(Member)>::Member;
			FieldInfo field;
			field.Name = std::move(name);
			field.Type = DeduceFieldType<MemberType>();
			field.Flags = options.Flags;
			field.Min = options.Min;
			field.Max = options.Max;
			field.EnumNames = std::move(options.EnumNames);
			field.Description = std::move(options.Description);
			field.Access = &AccessMember<Member>;
			return field;
		}

		template<auto Outer, auto Inner>
		FieldInfo MakeNestedField(std::string name, FieldOptions options = {})
		{
			using MemberType = typename MemberPointerTraits<decltype(Inner)>::Member;
			FieldInfo field;
			field.Name = std::move(name);
			field.Type = DeduceFieldType<MemberType>();
			field.Flags = options.Flags;
			field.Min = options.Min;
			field.Max = options.Max;
			field.EnumNames = std::move(options.EnumNames);
			field.Description = std::move(options.Description);
			field.Access = &AccessNestedMember<Outer, Inner>;
			return field;
		}

		// Reads a field as JSON.
		nlohmann::json GetField(const FieldInfo& field, const void* object);
		// Writes a field from JSON. Returns false (and sets outError) if the value has the wrong shape, is out of range
		// or names an unknown enum value; the field is left unchanged in that case.
		bool SetField(const FieldInfo& field, void* object, const nlohmann::json& value, std::string& outError);
		// All fields <-> JSON object. FromJson ignores missing fields (keeping current values) and reports unknown or
		// invalid ones via outError, prefixed with ownerName (but still applies the valid ones).
		nlohmann::json ToJson(std::span<const FieldInfo> fields, const void* object);
		bool FromJson(std::span<const FieldInfo> fields, std::string_view ownerName, void* object, const nlohmann::json& document, std::string& outError);

	}

}
