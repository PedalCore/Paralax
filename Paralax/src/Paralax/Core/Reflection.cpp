#include "pxpch.h"
#include "Paralax/Core/Reflection.h"

#include <spdlog/fmt/ranges.h>

#include <charconv>
#include <cfloat>
#include <cmath>

namespace Paralax {

	using json = nlohmann::json;

	const FieldInfo* FindField(std::span<const FieldInfo> fields, std::string_view name)
	{
		for (const FieldInfo& field : fields)
		{
			if (field.Name == name)
				return &field;
		}
		return nullptr;
	}

	namespace Reflection {

		namespace {

			std::string HandleToString(UUID handle)
			{
				return std::to_string(static_cast<uint64_t>(handle));
			}

			bool ParseHandle(const json& value, UUID& outHandle)
			{
				// Strings are canonical (64-bit values exceed JSON's safe integer range); unsigned numbers are accepted.
				if (value.is_number_unsigned())
				{
					outHandle = UUID(value.get<uint64_t>());
					return true;
				}
				if (!value.is_string())
					return false;

				const std::string& text = value.get_ref<const std::string&>();
				uint64_t parsed = 0;
				auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
				if (ec != std::errc() || ptr != text.data() + text.size() || text.empty())
					return false;
				outHandle = UUID(parsed);
				return true;
			}

			template<int N>
			bool ParseFloats(const json& value, float (&out)[N])
			{
				if (!value.is_array() || value.size() != N)
					return false;
				for (int i = 0; i < N; i++)
				{
					if (!value[static_cast<size_t>(i)].is_number())
						return false;
					const double number = value[static_cast<size_t>(i)].get<double>();
					if (!std::isfinite(number) || std::abs(number) > static_cast<double>(FLT_MAX))
						return false;
					out[i] = static_cast<float>(number);
				}
				return true;
			}

			bool InRange(const FieldInfo& field, double value)
			{
				return field.Min >= field.Max || (value >= static_cast<double>(field.Min) - 1e-6 && value <= static_cast<double>(field.Max) + 1e-6);
			}

		}

		json GetField(const FieldInfo& field, const void* object)
		{
			void* pointer = field.Access(const_cast<void*>(object));
			switch (field.Type)
			{
				case FieldType::Bool: return *static_cast<bool*>(pointer);
				case FieldType::Int: return *static_cast<int*>(pointer);
				case FieldType::Float: return *static_cast<float*>(pointer);
				case FieldType::Vec2:
				{
					const auto& v = *static_cast<glm::vec2*>(pointer);
					return json::array({v.x, v.y});
				}
				case FieldType::Vec3:
				{
					const auto& v = *static_cast<glm::vec3*>(pointer);
					return json::array({v.x, v.y, v.z});
				}
				case FieldType::Vec4:
				{
					const auto& v = *static_cast<glm::vec4*>(pointer);
					return json::array({v.x, v.y, v.z, v.w});
				}
				case FieldType::Quat:
				{
					const auto& q = *static_cast<glm::quat*>(pointer);
					return json::array({q.x, q.y, q.z, q.w});
				}
				case FieldType::String: return *static_cast<std::string*>(pointer);
				case FieldType::Asset: return HandleToString(*static_cast<UUID*>(pointer));
				case FieldType::AssetArray:
				{
					json array = json::array();
					for (UUID handle : *static_cast<std::vector<UUID>*>(pointer))
						array.push_back(HandleToString(handle));
					return array;
				}
				case FieldType::Enum:
				{
					const int value = *static_cast<int*>(pointer);
					if (value >= 0 && static_cast<size_t>(value) < field.EnumNames.size())
						return field.EnumNames[static_cast<size_t>(value)];
					return value;
				}
			}
			return nullptr;
		}

		bool SetField(const FieldInfo& field, void* object, const json& value, std::string& outError)
		{
			void* pointer = field.Access(object);
			auto fail = [&](const char* expected) {
				outError = fmt::format("Field '{}' expects {}, got {}", field.Name, expected, value.dump(-1, ' ', false, json::error_handler_t::replace));
				return false;
			};

			switch (field.Type)
			{
				case FieldType::Bool:
					if (!value.is_boolean())
						return fail("a boolean");
					*static_cast<bool*>(pointer) = value.get<bool>();
					return true;

				case FieldType::Int:
				{
					if (!value.is_number_integer())
						return fail("an integer");
					const int64_t number = value.get<int64_t>();
					if (number < INT32_MIN || number > INT32_MAX || !InRange(field, static_cast<double>(number)))
						return fail(field.Min < field.Max ? fmt::format("an integer in [{}, {}]", field.Min, field.Max).c_str() : "a 32-bit integer");
					*static_cast<int*>(pointer) = static_cast<int>(number);
					return true;
				}

				case FieldType::Float:
				{
					if (!value.is_number() || !std::isfinite(value.get<double>()) || std::abs(value.get<double>()) > static_cast<double>(FLT_MAX))
						return fail("a finite number");
					const double number = value.get<double>();
					if (!InRange(field, number))
						return fail(fmt::format("a number in [{}, {}]", field.Min, field.Max).c_str());
					*static_cast<float*>(pointer) = static_cast<float>(number);
					return true;
				}

				case FieldType::Vec2:
				{
					float v[2];
					if (!ParseFloats(value, v))
						return fail("an array of 2 numbers");
					// A range on a vector field applies to each component (e.g. a collider size must be positive).
					for (float component : v)
					{
						if (!InRange(field, static_cast<double>(component)))
							return fail(fmt::format("2 numbers, each in [{}, {}]", field.Min, field.Max).c_str());
					}
					*static_cast<glm::vec2*>(pointer) = {v[0], v[1]};
					return true;
				}

				case FieldType::Vec3:
				{
					float v[3];
					if (!ParseFloats(value, v))
						return fail("an array of 3 numbers");
					// A range on a vector field applies to each component (e.g. a collider size must be positive).
					for (float component : v)
					{
						if (!InRange(field, static_cast<double>(component)))
							return fail(fmt::format("3 numbers, each in [{}, {}]", field.Min, field.Max).c_str());
					}
					*static_cast<glm::vec3*>(pointer) = {v[0], v[1], v[2]};
					return true;
				}

				case FieldType::Vec4:
				{
					float v[4];
					if (!ParseFloats(value, v))
						return fail("an array of 4 numbers");
					// A range on a vector field applies to each component (e.g. a collider size must be positive).
					for (float component : v)
					{
						if (!InRange(field, static_cast<double>(component)))
							return fail(fmt::format("4 numbers, each in [{}, {}]", field.Min, field.Max).c_str());
					}
					*static_cast<glm::vec4*>(pointer) = {v[0], v[1], v[2], v[3]};
					return true;
				}

				case FieldType::Quat:
				{
					float v[4];
					if (!ParseFloats(value, v))
						return fail("a quaternion [x, y, z, w]");
					// The length is taken in double: squaring float components above ~1.8e19 overflows to infinity, which
					// would pass the zero check and store a zero quaternion (the value this very check refuses).
					const double x = static_cast<double>(v[0]), y = static_cast<double>(v[1]), z = static_cast<double>(v[2]), w = static_cast<double>(v[3]);
					const double length = std::sqrt(x * x + y * y + z * z + w * w);
					if (!std::isfinite(length) || length < 1e-6)
						return fail("a non-zero quaternion");
					// A quaternion already unit to float precision is kept bit for bit, so saved rotations round-trip
					// exactly (dividing by a length a few ulps from 1 would nudge them on every load). Anything this
					// stores is within the tolerance, so storing is a fixed point.
					if (std::abs(length - 1.0) <= 4.0 * static_cast<double>(FLT_EPSILON))
						*static_cast<glm::quat*>(pointer) = glm::quat(v[3], v[0], v[1], v[2]);
					else
						*static_cast<glm::quat*>(pointer) = glm::quat(static_cast<float>(w / length), static_cast<float>(x / length), static_cast<float>(y / length), static_cast<float>(z / length));
					return true;
				}

				case FieldType::String:
					if (!value.is_string())
						return fail("a string");
					if (field.MaxLength > 0 && value.get_ref<const std::string&>().size() > field.MaxLength)
						return fail(fmt::format("at most {} bytes", field.MaxLength).c_str());
					*static_cast<std::string*>(pointer) = value.get<std::string>();
					return true;

				case FieldType::Asset:
				{
					UUID handle = 0;
					if (!ParseHandle(value, handle))
						return fail("an asset handle string");
					*static_cast<UUID*>(pointer) = handle;
					return true;
				}

				case FieldType::AssetArray:
				{
					if (!value.is_array())
						return fail("an array of asset handle strings");
					std::vector<UUID> handles;
					for (const json& element : value)
					{
						UUID handle = 0;
						if (!ParseHandle(element, handle))
							return fail("an array of asset handle strings");
						handles.push_back(handle);
					}
					*static_cast<std::vector<UUID>*>(pointer) = std::move(handles);
					return true;
				}

				case FieldType::Enum:
				{
					if (value.is_string())
					{
						for (size_t i = 0; i < field.EnumNames.size(); i++)
						{
							if (field.EnumNames[i] == value.get_ref<const std::string&>())
							{
								*static_cast<int*>(pointer) = static_cast<int>(i);
								return true;
							}
						}
					}
					return fail(fmt::format("one of: {}", fmt::join(field.EnumNames, ", ")).c_str());
				}
			}
			return fail("a supported value");
		}

		json ToJson(std::span<const FieldInfo> fields, const void* object)
		{
			json document = json::object();
			for (const FieldInfo& field : fields)
				document[field.Name] = GetField(field, object);
			return document;
		}

		bool FromJson(std::span<const FieldInfo> fields, std::string_view ownerName, void* object, const json& document, std::string& outError)
		{
			if (!document.is_object())
			{
				outError = fmt::format("'{}' expects a JSON object", ownerName);
				return false;
			}

			std::vector<std::string> errors;
			for (auto it = document.begin(); it != document.end(); ++it)
			{
				const FieldInfo* field = FindField(fields, it.key());
				if (!field)
				{
					errors.push_back(fmt::format("Unknown field '{}.{}'", ownerName, it.key()));
					continue;
				}

				std::string error;
				if (!SetField(*field, object, it.value(), error))
					errors.push_back(std::move(error));
			}

			outError = fmt::format("{}", fmt::join(errors, "; "));
			return errors.empty();
		}

	}

}
