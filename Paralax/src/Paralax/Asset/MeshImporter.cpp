#include "pxpch.h"
#include "Paralax/Asset/MeshImporter.h"

#include "Paralax/Animation/AnimationClip.h"
#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Math/Math.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <simdjson.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Paralax {

	namespace {

		uint64_t SplitMix64(uint64_t value)
		{
			value += 0x9E3779B97F4A7C15ull;
			value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
			value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
			return value ^ (value >> 31);
		}

		// Upper bound on accessor element counts (also bounds zero-filled accessors, which have no backing bytes). The
		// bytes behind them are bounded too: external buffers by MeshImporter::MaxBufferBytes (all of a model's
		// together, read by ReadExternalBuffers) and images by TextureAsset::MaxEncodedBytes.
		constexpr size_t s_MaxElements = size_t(1) << 24;
		constexpr size_t s_MaxNodeDepth = 256;

		struct AttributeType
		{
			std::string_view Name;
			fastgltf::AccessorType Type;
		};
		// Vertex attributes read when present, and the element type each must have.
		constexpr AttributeType s_OptionalAttributes[] = {
			{"NORMAL", fastgltf::AccessorType::Vec3},
			{"TANGENT", fastgltf::AccessorType::Vec4},
			{"TEXCOORD_0", fastgltf::AccessorType::Vec2},
		};

		std::string_view AccessorTypeName(fastgltf::AccessorType type)
		{
			const std::string_view name = fastgltf::getAccessorTypeName(type);
			return name.empty() ? std::string_view("invalid") : name;
		}

		// The bytes a buffer actually holds. The sources fastgltf leaves without bytes (a URI it was not asked to
		// load, a fallback) hold none; ReadExternalBuffers has replaced every URI by the time anything reads data.
		size_t GetBufferSize(const fastgltf::Buffer& buffer)
		{
			return std::visit(fastgltf::visitor{
								  [](const fastgltf::sources::Array& array) -> size_t { return array.bytes.size_bytes(); },
								  [](const fastgltf::sources::Vector& vector) -> size_t { return vector.bytes.size(); },
								  [](const fastgltf::sources::ByteView& view) -> size_t { return view.bytes.size(); },
								  [](const auto&) -> size_t { return 0; },
							  },
							  buffer.data);
		}

		// Reads a file the model refers to, which must stay inside the model's directory (no "../" or absolute escapes)
		// and hold at most maxBytes. Returns an empty vector (with outProblem set) if it does not exist, would leave the
		// directory or is too large; the size is checked before anything is read or allocated.
		template<typename Byte>
		std::vector<Byte> ReadModelFile(const std::filesystem::path& directory, const std::filesystem::path& relativePath, size_t maxBytes, std::string& outProblem)
		{
			static_assert(sizeof(Byte) == 1);
			std::error_code error;
			const std::filesystem::path base = std::filesystem::weakly_canonical(directory, error);
			const std::filesystem::path target = std::filesystem::weakly_canonical(directory / relativePath, error);
			const std::filesystem::path relative = target.lexically_relative(base);
			if (error || relative.empty() || *relative.begin() == ".." || relativePath.is_absolute())
			{
				outProblem = fmt::format("path '{}' leaves the model's directory", ToUtf8(relativePath));
				return {};
			}
			std::ifstream file(target, std::ios::binary);
			if (!file)
			{
				outProblem = fmt::format("file '{}' not found", ToUtf8(relativePath));
				return {};
			}
			const uintmax_t size = std::filesystem::file_size(target, error);
			if (error)
			{
				outProblem = fmt::format("file '{}' cannot be read: {}", ToUtf8(relativePath), error.message());
				return {};
			}
			if (size > maxBytes)
			{
				outProblem = fmt::format("file '{}' is too large ({} bytes; at most {} can be read)", ToUtf8(relativePath), size, maxBytes);
				return {};
			}
			std::vector<Byte> bytes(static_cast<size_t>(size));
			if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
			{
				outProblem = fmt::format("file '{}' cannot be read", ToUtf8(relativePath));
				return {};
			}
			return bytes;
		}

		// Reads the buffers kept in files beside the model. fastgltf is not asked to (Options::LoadExternalBuffers):
		// it would follow any path and read any size. Here they are confined to the model's directory, like images,
		// and to MeshImporter::MaxBufferBytes together. Data URIs and GLB chunks arrive decoded from fastgltf and are
		// left as they are. Returns the problem ("buffer N: ...") or an empty string.
		// A glTF URI as a filesystem path. fastgltf percent-decodes the URI into bytes that need not be valid UTF-8, and
		// building a path from them can throw on Windows (no Unicode translation); that is the boundary, so it is
		// caught here and such a URI is treated as unusable rather than left to unwind through the importer.
		std::optional<std::filesystem::path> UriPath(const fastgltf::URI& uri)
		{
			if (!uri.isLocalPath())
				return std::nullopt;
			try
			{
				return uri.fspath();
			}
			catch (const std::exception&)
			{
				return std::nullopt;
			}
		}

		std::string ReadExternalBuffers(fastgltf::Asset& asset, const std::filesystem::path& directory)
		{
			size_t budget = MeshImporter::MaxBufferBytes;
			for (size_t i = 0; i < asset.buffers.size(); i++)
			{
				const auto* source = std::get_if<fastgltf::sources::URI>(&asset.buffers[i].data);
				if (!source)
					continue;
				std::string problem;
				std::vector<std::byte> bytes;
				if (const std::optional<std::filesystem::path> file = UriPath(source->uri))
					bytes = ReadModelFile<std::byte>(directory, *file, budget, problem);
				else
					problem = "its URI is not a local file path";
				if (!problem.empty())
					return fmt::format("buffer {}: {}", i, problem);
				budget -= bytes.size();
				const fastgltf::MimeType mimeType = source->mimeType; // source dies with the assignment below.
				asset.buffers[i].data = fastgltf::sources::Vector{std::move(bytes), mimeType};
			}
			return {};
		}

		// Parallax settings some exporters put in a material's extras (glTF has no height maps): "pom" (or
		// "parallax_mode": "POM") enables it, "height_map" names the height image (a file beside the model or beside the
		// material's other textures; 1 = surface), "height_depth_m" is the relief depth and "texture_repeat_m" the size
		// of one texture repeat, both in metres.
		struct MaterialExtras
		{
			bool Parallax = false;
			std::string HeightMap;
			double DepthMeters = 0.0;
			double RepeatMeters = 0.0;
		};

		// Settings Blender's exporter puts in a node's extras (an object's custom properties): "cast_shadow" turns the
		// node's mesh shadows on or off.
		struct NodeExtras
		{
			std::optional<bool> CastShadows;
			bool InvalidCastShadows = false; // Present, but not a boolean.
		};

		// Everything the extras callback records, by object index; Parse clears it before every attempt.
		struct DocumentExtras
		{
			std::vector<MaterialExtras> Materials;
			std::vector<NodeExtras> Nodes;
		};

		void ReadMaterialExtras(const simdjson::dom::object& extras, MaterialExtras& material)
		{
			bool pom = false;
			std::string_view mode;
			std::string_view heightMap;
			double depth = 0.0;
			double repeat = 0.0;
			// Any field of the wrong type is simply absent.
			if (extras.at_key("pom").get_bool().get(pom) != simdjson::SUCCESS)
				pom = false;
			if (extras.at_key("parallax_mode").get_string().get(mode) != simdjson::SUCCESS)
				mode = {};
			if (extras.at_key("height_map").get_string().get(heightMap) == simdjson::SUCCESS && heightMap.size() <= 1024)
				material.HeightMap = std::string(heightMap);
			if (extras.at_key("height_depth_m").get_double().get(depth) == simdjson::SUCCESS)
				material.DepthMeters = depth;
			if (extras.at_key("texture_repeat_m").get_double().get(repeat) == simdjson::SUCCESS)
				material.RepeatMeters = repeat;
			material.Parallax = pom || mode == "POM";
		}

		void ReadNodeExtras(const simdjson::dom::object& extras, NodeExtras& node)
		{
			simdjson::dom::element value;
			if (extras.at_key("cast_shadow").get(value) != simdjson::SUCCESS)
				return; // Not specified.
			bool castShadow = true;
			if (value.get_bool().get(castShadow) == simdjson::SUCCESS)
				node.CastShadows = castShadow;
			else
				node.InvalidCastShadows = true;
		}

		// fastgltf extras callback, the parser's only one: records material and node extras (userPointer:
		// DocumentExtras). Indices are bounded, so a hostile file cannot make either list grow without limit.
		void ReadExtras(simdjson::dom::object* extras, std::size_t objectIndex, fastgltf::Category objectType, void* userPointer)
		{
			constexpr size_t MaxMaterials = size_t(1) << 20;
			constexpr size_t MaxNodes = size_t(1) << 20;
			if (!extras || !userPointer)
				return;
			auto& all = *static_cast<DocumentExtras*>(userPointer);
			if (objectType == fastgltf::Category::Materials && objectIndex < MaxMaterials)
			{
				if (all.Materials.size() <= objectIndex)
					all.Materials.resize(objectIndex + 1);
				ReadMaterialExtras(*extras, all.Materials[objectIndex]);
			}
			else if (objectType == fastgltf::Category::Nodes && objectIndex < MaxNodes)
			{
				if (all.Nodes.size() <= objectIndex)
					all.Nodes.resize(objectIndex + 1);
				ReadNodeExtras(*extras, all.Nodes[objectIndex]);
			}
		}

		struct ParseOutcome
		{
			fastgltf::Error Parse = fastgltf::Error::None;
			fastgltf::Error Validation = fastgltf::Error::None;
			std::string BufferProblem; // Why an external buffer of a parsed, valid document could not be read.
			bool Threw = false;
		};

		// Parses (from the start of data) and validates a document, reading material and node extras into outExtras
		// (cleared first, so a retry never keeps what a failed attempt read), then reads its external buffers
		// (ReadExternalBuffers). fastgltf can throw (std::bad_alloc for absurd sizes in a hostile file); that is caught
		// here, at the boundary, and reported as a parse failure. outAsset is set only on success: no error, and no
		// BufferProblem.
		ParseOutcome Parse(fastgltf::GltfDataGetter& data, const std::filesystem::path& directory, fastgltf::Extensions extensions, fastgltf::Options options,
						   DocumentExtras& outExtras, std::optional<fastgltf::Asset>& outAsset)
		{
			ParseOutcome outcome;
			outAsset.reset();
			outExtras.Materials.clear();
			outExtras.Nodes.clear();
			try
			{
				fastgltf::Parser parser(extensions);
				parser.setExtrasParseCallback(&ReadExtras);
				parser.setUserPointer(&outExtras);
				data.reset();
				auto loaded = parser.loadGltf(data, directory, options);
				outcome.Parse = loaded.error();
				if (outcome.Parse != fastgltf::Error::None)
					return outcome;
				// Validation needs only the declared sizes, so a malformed document costs no file reads.
				outcome.Validation = fastgltf::validate(loaded.get());
				if (outcome.Validation != fastgltf::Error::None)
					return outcome;
				outcome.BufferProblem = ReadExternalBuffers(loaded.get(), directory);
				if (outcome.BufferProblem.empty())
					outAsset.emplace(std::move(loaded.get()));
			}
			catch (const std::exception&)
			{
				outcome.Parse = fastgltf::Error::InvalidGltf;
				outcome.BufferProblem.clear();
				outcome.Threw = true;
				outAsset.reset();
			}
			return outcome;
		}

		// Returns the encoded bytes of an image, or an empty vector if the source is unsupported.
		std::vector<uint8_t> GetImageBytes(const fastgltf::Asset& asset, const fastgltf::Image& image, const std::filesystem::path& directory, std::string& outProblem)
		{
			std::vector<uint8_t> bytes;
			std::visit(fastgltf::visitor{
						   [&](const fastgltf::sources::Array& array) {
							   const auto* data = reinterpret_cast<const uint8_t*>(array.bytes.data());
							   bytes.assign(data, data + array.bytes.size_bytes());
						   },
						   [&](const fastgltf::sources::Vector& vector) {
							   const auto* data = reinterpret_cast<const uint8_t*>(vector.bytes.data());
							   bytes.assign(data, data + vector.bytes.size());
						   },
						   [&](const fastgltf::sources::BufferView& view) {
							   if (view.bufferViewIndex >= asset.bufferViews.size() || asset.bufferViews[view.bufferViewIndex].bufferIndex >= asset.buffers.size())
							   {
								   outProblem = "image buffer view does not exist";
								   return;
							   }
							   const fastgltf::BufferView& bufferView = asset.bufferViews[view.bufferViewIndex];
							   const size_t bufferSize = GetBufferSize(asset.buffers[bufferView.bufferIndex]);
							   if (bufferView.byteOffset > bufferSize || bufferView.byteLength > bufferSize - bufferView.byteOffset)
							   {
								   outProblem = "image buffer view exceeds its buffer's data";
								   return;
							   }
							   const auto span = fastgltf::DefaultBufferDataAdapter{}(asset, view.bufferViewIndex);
							   const auto* data = reinterpret_cast<const uint8_t*>(span.data());
							   bytes.assign(data, data + span.size());
						   },
						   [&](const fastgltf::sources::URI& uri) {
							   const std::optional<std::filesystem::path> file = UriPath(uri.uri);
							   if (!file)
							   {
								   outProblem = "image URI is not a local file path";
								   return;
							   }
							   bytes = ReadModelFile<uint8_t>(directory, *file, TextureAsset::MaxEncodedBytes, outProblem);
						   },
						   [&](const auto&) { outProblem = "unsupported image source"; },
					   },
					   image.data);
			return bytes;
		}

		// Checks that the accessor holds elements of the type its use requires (fastgltf's typed readers only assert it,
		// and a mismatch would write past the destination) and that reading it stays within the bytes actually present,
		// not just the declared lengths.
		bool ValidateAccessor(const fastgltf::Asset& asset, size_t index, fastgltf::AccessorType expectedType, std::string& outProblem)
		{
			if (index >= asset.accessors.size())
			{
				outProblem = fmt::format("accessor {} does not exist", index);
				return false;
			}
			const fastgltf::Accessor& accessor = asset.accessors[index];
			if (accessor.type != expectedType)
			{
				outProblem = fmt::format("accessor {} has type {}, expected {}", index, AccessorTypeName(accessor.type), AccessorTypeName(expectedType));
				return false;
			}
			if (accessor.sparse.has_value())
			{
				outProblem = fmt::format("accessor {} is sparse (unsupported)", index);
				return false;
			}
			if (accessor.count > s_MaxElements)
			{
				outProblem = fmt::format("accessor {} has too many elements ({})", index, accessor.count);
				return false;
			}
			if (!accessor.bufferViewIndex.has_value() || accessor.count == 0)
				return true; // Zero-filled (bounded by s_MaxElements) or empty.

			if (*accessor.bufferViewIndex >= asset.bufferViews.size())
			{
				outProblem = fmt::format("accessor {} references a missing buffer view", index);
				return false;
			}
			const fastgltf::BufferView& view = asset.bufferViews[*accessor.bufferViewIndex];
			if (view.bufferIndex >= asset.buffers.size())
			{
				outProblem = fmt::format("buffer view of accessor {} references a missing buffer", index);
				return false;
			}
			const size_t bufferSize = GetBufferSize(asset.buffers[view.bufferIndex]);
			if (view.byteOffset > bufferSize || view.byteLength > bufferSize - view.byteOffset)
			{
				outProblem = fmt::format("buffer view of accessor {} exceeds its buffer's data", index);
				return false;
			}
			const size_t elementSize = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
			const size_t stride = view.byteStride ? *view.byteStride : elementSize;
			if (elementSize == 0 || stride < elementSize)
			{
				outProblem = fmt::format("accessor {} has an invalid layout", index);
				return false;
			}
			// The offset and stride come straight from the file, so each is checked against the view on its own before
			// entering any arithmetic: an offset near 2^64 would wrap the sum below and pass it (fastgltf's validation
			// sums the same way and wraps the same way). A lone element never advances by the stride.
			if (accessor.byteOffset > view.byteLength)
			{
				outProblem = fmt::format("accessor {} starts {} bytes into a buffer view of {} bytes", index, accessor.byteOffset, view.byteLength);
				return false;
			}
			if (accessor.count > 1 && stride > view.byteLength)
			{
				outProblem = fmt::format("accessor {} has a byte stride of {}, more than its buffer view's {} bytes", index, stride, view.byteLength);
				return false;
			}
			// Offset and stride are at most byteLength, itself at most the bytes the buffer holds in memory, and count
			// is at most 2^24: the sum cannot wrap.
			const size_t lastByte = accessor.byteOffset + (accessor.count - 1) * stride + elementSize;
			if (lastByte > view.byteLength)
			{
				outProblem = fmt::format("accessor {} reads past its buffer view", index);
				return false;
			}
			return true;
		}

		// Photometric glTF light units back to the radiometric strengths exporters such as Blender start from.
		constexpr float s_LumensPerWatt = 683.0f;
		// Field ranges of the light and camera components (see ComponentRegistry).
		constexpr float s_MaxLightValue = 100000.0f;
		constexpr size_t s_MaxNameBytes = 256;

		float Clamped(float value, float low, float high, float fallback)
		{
			return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
		}

		// Scripts may set names of at most s_MaxNameBytes; imported names keep to that too, cut at a UTF-8 boundary.
		std::string NodeName(std::string_view name, size_t index)
		{
			if (name.empty())
				return fmt::format("Node{}", index);
			if (name.size() <= s_MaxNameBytes)
				return std::string(name);
			size_t end = s_MaxNameBytes;
			while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80)
				end--;
			return std::string(name.substr(0, end));
		}

		bool IsFinite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// FNV-1a: a stable key for a mesh name.
		uint64_t HashName(std::string_view name)
		{
			uint64_t hash = 0xCBF29CE484222325ull;
			for (const char c : name)
				hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
			return hash;
		}

		// A node's name, local transform, camera and light, in Paralax's conventions.
		ImportedNode MakeNode(const fastgltf::Asset& asset, const fastgltf::Node& node, size_t index, std::vector<std::string>& warnings)
		{
			ImportedNode record;
			record.Name = NodeName(std::string_view(node.name.data(), node.name.size()), index);

			bool usable = true;
			if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform))
			{
				record.Translation = glm::vec3(trs->translation.x(), trs->translation.y(), trs->translation.z());
				record.Rotation = glm::quat(trs->rotation.w(), trs->rotation.x(), trs->rotation.y(), trs->rotation.z());
				record.Scale = glm::vec3(trs->scale.x(), trs->scale.y(), trs->scale.z());
				const float length = glm::length(record.Rotation);
				usable = std::isfinite(length) && length > 1e-6f;
				if (usable)
					record.Rotation /= length;
			}
			else if (const auto* matrix = std::get_if<fastgltf::math::fmat4x4>(&node.transform))
			{
				glm::mat4 transform;
				std::memcpy(glm::value_ptr(transform), matrix->data(), sizeof(float) * 16);
				if (!Math::DecomposeTransform(transform, record.Translation, record.Rotation, record.Scale))
				{
					// No scale to take apart (a node collapsed to a point): keep it collapsed where it is, as the
					// combined mesh shows it.
					record.Translation = glm::vec3(transform[3]);
					record.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
					record.Scale = glm::vec3(0.0f);
					usable = IsFinite(record.Translation);
				}
			}
			if (!usable || !IsFinite(record.Translation) || !IsFinite(record.Scale))
			{
				warnings.push_back(fmt::format("node '{}' has an unusable transform; imported at its parent's origin", record.Name));
				record.Translation = glm::vec3(0.0f);
				record.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
				record.Scale = glm::vec3(1.0f);
			}

			if (node.cameraIndex.has_value() && *node.cameraIndex < asset.cameras.size())
			{
				ImportedCamera camera;
				if (const auto* perspective = std::get_if<fastgltf::Camera::Perspective>(&asset.cameras[*node.cameraIndex].camera))
				{
					camera.FieldOfView = Clamped(perspective->yfov, glm::radians(1.0f), glm::radians(179.0f), glm::radians(60.0f));
					camera.Near = Clamped(perspective->znear, 0.0001f, 100000.0f, 0.1f);
					camera.Far = Clamped(perspective->zfar.value_or(1000.0f), 0.001f, 1000000.0f, 1000.0f);
				}
				else if (const auto* orthographic = std::get_if<fastgltf::Camera::Orthographic>(&asset.cameras[*node.cameraIndex].camera))
				{
					camera.Orthographic = true;
					camera.OrthographicSize = Clamped(2.0f * std::abs(orthographic->ymag), 0.0001f, 1000000.0f, 10.0f); // ymag is half the height.
					camera.Near = Clamped(orthographic->znear, -1000000.0f, 1000000.0f, 0.0f);
					camera.Far = Clamped(orthographic->zfar, -1000000.0f, 1000000.0f, 100.0f);
				}
				if (camera.Far <= camera.Near)
					camera.Far = camera.Near + 1.0f;
				record.Camera = camera;
			}

			if (node.lightIndex.has_value() && *node.lightIndex < asset.lights.size())
			{
				const fastgltf::Light& source = asset.lights[*node.lightIndex];
				ImportedLight light;
				light.Type = source.type == fastgltf::LightType::Directional ? ImportedLightType::Directional
							 : source.type == fastgltf::LightType::Spot      ? ImportedLightType::Spot
																			 : ImportedLightType::Point;
				light.Color = glm::vec3(source.color.x(), source.color.y(), source.color.z());
				light.Color = IsFinite(light.Color) ? glm::clamp(light.Color, glm::vec3(0.0f), glm::vec3(1.0f)) : glm::vec3(1.0f);
				light.Intensity = Clamped(source.intensity / s_LumensPerWatt, 0.0f, s_MaxLightValue, 1.0f);
				// Without a range the light reaches as far as it stays above 0.01 (intensity / d^2).
				const float range = source.range.has_value() ? source.range.value() : 10.0f * std::sqrt(light.Intensity);
				light.Range = Clamped(range, 0.01f, s_MaxLightValue, 10.0f);
				const float maxCone = glm::radians(89.0f);
				light.OuterConeAngle = Clamped(source.outerConeAngle.value_or(glm::quarter_pi<float>()), 0.0f, maxCone, glm::quarter_pi<float>());
				light.InnerConeAngle = std::min(Clamped(source.innerConeAngle.value_or(0.0f), 0.0f, maxCone, 0.0f), light.OuterConeAngle);
				record.Light = light;
			}
			return record;
		}

		// glTF indices are unsigned integers; anything else is a malformed file.
		bool ValidateIndexComponents(const fastgltf::Accessor& accessor, size_t index, std::string& outProblem)
		{
			switch (accessor.componentType)
			{
				case fastgltf::ComponentType::UnsignedByte:
				case fastgltf::ComponentType::UnsignedShort:
				case fastgltf::ComponentType::UnsignedInt:
					return true;
				default:
					outProblem = fmt::format("index accessor {} does not hold unsigned integers", index);
					return false;
			}
		}

	}

	namespace {

		// Reads one channel's keys, or explains why it cannot be used.
		bool ReadChannel(const fastgltf::Asset& asset, const fastgltf::AnimationSampler& sampler, fastgltf::AnimationPath path, AnimationChannel& outChannel, size_t& budget, std::string& outProblem)
		{
			const bool rotation = path == fastgltf::AnimationPath::Rotation;
			const fastgltf::AccessorType valueType = rotation ? fastgltf::AccessorType::Vec4 : fastgltf::AccessorType::Vec3;
			if (!ValidateAccessor(asset, sampler.inputAccessor, fastgltf::AccessorType::Scalar, outProblem) || !ValidateAccessor(asset, sampler.outputAccessor, valueType, outProblem))
				return false;
			const fastgltf::Accessor& input = asset.accessors[sampler.inputAccessor];
			const fastgltf::Accessor& output = asset.accessors[sampler.outputAccessor];
			if (input.componentType != fastgltf::ComponentType::Float || output.componentType != fastgltf::ComponentType::Float)
			{
				outProblem = "only float keyframes are supported";
				return false;
			}
			const bool cubic = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
			if (input.count == 0 || output.count != input.count * (cubic ? 3 : 1))
			{
				outProblem = fmt::format("{} key times but {} values", input.count, output.count);
				return false;
			}
			if (input.count + output.count > budget)
			{
				outProblem = "the file has too many keyframes";
				return false;
			}
			budget -= input.count + output.count;

			outChannel.Property = rotation ? AnimationProperty::Rotation : path == fastgltf::AnimationPath::Scale ? AnimationProperty::Scale
																												  : AnimationProperty::Translation;
			outChannel.Interpolation = cubic                                                             ? AnimationInterpolation::CubicSpline
									   : sampler.interpolation == fastgltf::AnimationInterpolation::Step ? AnimationInterpolation::Step
																										 : AnimationInterpolation::Linear;
			outChannel.Times.resize(input.count);
			fastgltf::copyFromAccessor<float>(asset, input, outChannel.Times.data());
			outChannel.Values.resize(output.count);
			if (rotation)
				fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, output, [&](glm::vec4 value, size_t index) { outChannel.Values[index] = value; });
			else
				fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, output, [&](glm::vec3 value, size_t index) { outChannel.Values[index] = glm::vec4(value, 0.0f); });
			return true;
		}

		// The glTF file's animations as clips. Each clip is meant to be played by the deepest node above (or at) every
		// node it animates (its host; none means the model's root); its channels name their targets relative to it.
		void ImportAnimations(const fastgltf::Asset& asset, AssetHandle meshHandle, const std::vector<std::optional<size_t>>& recordOf, MeshImportResult& outResult)
		{
			std::unordered_map<std::string_view, size_t> nameCounts;
			for (const fastgltf::Animation& animation : asset.animations)
				nameCounts[std::string_view(animation.name.data(), animation.name.size())]++;
			size_t budget = s_MaxElements * 4; // Keyframes (times and values) in all clips.
			if (asset.animations.empty())
				return;

			// Targets are found by name below the playing entity, so a node sharing its name with a sibling is ambiguous.
			// Counted once for all clips (records with no parent are siblings under the model's root).
			std::map<std::pair<size_t, std::string_view>, size_t> siblingNames;
			for (const ImportedNode& node : outResult.Nodes)
				siblingNames[{node.Parent ? *node.Parent + 1 : 0, node.Name}]++;
			auto shared = [&](size_t record) {
				const ImportedNode& node = outResult.Nodes[record];
				return siblingNames[{node.Parent ? *node.Parent + 1 : 0, node.Name}] > 1;
			};

			for (size_t a = 0; a < asset.animations.size(); a++)
			{
				const fastgltf::Animation& source = asset.animations[a];
				const std::string_view name(source.name.data(), source.name.size());
				auto clip = CreateRef<AnimationClipAsset>();
				clip->Name = name.empty() ? fmt::format("Animation{}", a) : NodeName(name, a);
				clip->Handle = !name.empty() && nameCounts[name] == 1 ? DeriveSubAssetHandle(meshHandle, SubAssetKind::Animation, HashName(name))
																	  : DeriveSubAssetHandle(meshHandle, SubAssetKind::AnimationByIndex, a);

				std::vector<size_t> targets; // Node records, one per channel.
				for (size_t c = 0; c < source.channels.size(); c++)
				{
					const fastgltf::AnimationChannel& channel = source.channels[c];
					std::string problem;
					if (channel.path == fastgltf::AnimationPath::Weights)
						problem = "morph target weights are not supported";
					else if (!channel.nodeIndex || *channel.nodeIndex >= recordOf.size() || !recordOf[*channel.nodeIndex])
						problem = "it animates a node outside the scene";
					else if (channel.samplerIndex >= source.samplers.size())
						problem = "its sampler does not exist";
					AnimationChannel imported;
					if (problem.empty() && ReadChannel(asset, source.samplers[channel.samplerIndex], channel.path, imported, budget, problem))
					{
						clip->Channels.push_back(std::move(imported));
						targets.push_back(*recordOf[*channel.nodeIndex]);
					}
					else
					{
						outResult.Warnings.push_back(fmt::format("animation '{}' channel {} is skipped: {}", clip->Name, c, problem));
					}
				}
				if (clip->Channels.empty())
				{
					outResult.Warnings.push_back(fmt::format("animation '{}' has nothing Paralax can play; skipped", clip->Name));
					continue;
				}

				// The host: the deepest node that is (or is above) every target. Ancestor chains run root first.
				auto chainOf = [&](size_t record) {
					std::vector<size_t> chain;
					for (std::optional<size_t> at = record; at; at = outResult.Nodes[*at].Parent)
						chain.push_back(*at);
					std::reverse(chain.begin(), chain.end());
					return chain;
				};
				std::vector<std::vector<size_t>> chains;
				chains.reserve(targets.size());
				for (size_t target : targets)
					chains.push_back(chainOf(target));
				std::vector<size_t> common = chains[0];
				for (const std::vector<size_t>& chain : chains)
				{
					size_t same = 0;
					while (same < common.size() && same < chain.size() && common[same] == chain[same])
						same++;
					common.resize(same);
				}
				std::optional<size_t> host = common.empty() ? std::nullopt : std::optional<size_t>(common.back());

				// Each target as the names of the nodes from just below the host down to it.
				bool ambiguous = false;
				for (size_t c = 0; c < clip->Channels.size(); c++)
				{
					for (size_t i = host ? common.size() : 0; i < chains[c].size(); i++)
					{
						clip->Channels[c].Target.push_back(outResult.Nodes[chains[c][i]].Name);
						ambiguous = ambiguous || shared(chains[c][i]);
					}
				}
				if (ambiguous)
					outResult.Warnings.push_back(fmt::format("animation '{}' animates a node whose name a sibling shares; give them different names", clip->Name));

				for (const AnimationChannel& channel : clip->Channels)
					clip->Duration = std::max(clip->Duration, channel.Times.back());
				std::string problem;
				if (!clip->Validate(problem))
				{
					outResult.Warnings.push_back(fmt::format("animation '{}' is skipped: {}", clip->Name, problem));
					continue;
				}
				outResult.Animations.push_back({clip, host});
			}
		}

	}

	AssetHandle DeriveSubAssetHandle(AssetHandle parent, SubAssetKind kind, uint64_t index)
	{
		uint64_t value = SplitMix64(static_cast<uint64_t>(parent) ^ SplitMix64(static_cast<uint64_t>(kind) * 0x100000001B3ull + index));
		// Never collide with the reserved built-in range.
		while (value < AssetManager::FirstUserHandle)
			value = SplitMix64(value);
		return AssetHandle(value);
	}

	bool MeshImporter::Import(const std::filesystem::path& path, AssetHandle meshHandle, MeshImportResult& outResult, std::string& outError)
	{
		outResult = MeshImportResult();
		outError.clear();

		// The model file is read whole (a GLB carries its buffers, a .gltf may embed them as data URIs), so it gets the
		// same bound as the external buffers, checked before anything is read.
		std::error_code sizeError;
		const uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
		if (sizeError)
		{
			outError = fmt::format("cannot read '{}': {}", ToUtf8(path), sizeError.message());
			return false;
		}
		if (fileSize > MeshImporter::MaxBufferBytes)
		{
			outError = fmt::format("cannot read '{}': file is too large ({} bytes; at most {} can be read)", ToUtf8(path), fileSize, MeshImporter::MaxBufferBytes);
			return false;
		}

		auto data = fastgltf::GltfDataBuffer::FromPath(path);
		if (data.error() != fastgltf::Error::None)
		{
			outError = fmt::format("cannot read '{}': {}", ToUtf8(path), fastgltf::getErrorMessage(data.error()));
			return false;
		}

		// fastgltf only validates on export; structurally invalid documents are rejected before any data is touched.
		// External buffers are read by Parse (see ReadExternalBuffers), not by fastgltf, which would not confine them.
		const auto options = fastgltf::Options::GenerateMeshIndices;
		const auto extensions = fastgltf::Extensions::KHR_materials_emissive_strength | fastgltf::Extensions::KHR_texture_transform;
		DocumentExtras documentExtras;
		std::optional<fastgltf::Asset> parsed;
		ParseOutcome outcome = Parse(data.get(), path.parent_path(), extensions | fastgltf::Extensions::KHR_lights_punctual, options, documentExtras, parsed);
		if (outcome.Parse != fastgltf::Error::None || outcome.Validation != fastgltf::Error::None)
		{
			// fastgltf rejects a whole file over malformed light data (an unknown type, a zero range...), which the
			// meshes do not need: try once more without lights (the extras are read again).
			const ParseOutcome retry = Parse(data.get(), path.parent_path(), extensions, options, documentExtras, parsed);
			if (retry.Parse == fastgltf::Error::None && retry.Validation == fastgltf::Error::None)
			{
				outcome = retry;
				outResult.Warnings.push_back("the file's lights (KHR_lights_punctual) are malformed and were not imported");
			}
		}
		if (outcome.Parse != fastgltf::Error::None)
		{
			outError = fmt::format("cannot parse '{}': {}", ToUtf8(path), outcome.Threw ? "it is too large or malformed" : fastgltf::getErrorMessage(outcome.Parse));
			return false;
		}
		if (outcome.Validation != fastgltf::Error::None)
		{
			outError = fmt::format("'{}' is not a valid glTF file: {}", ToUtf8(path), fastgltf::getErrorMessage(outcome.Validation));
			return false;
		}
		if (!outcome.BufferProblem.empty())
		{
			outError = fmt::format("cannot load '{}': {}", ToUtf8(path), outcome.BufferProblem);
			return false;
		}
		const fastgltf::Asset& asset = *parsed;
		const std::filesystem::path directory = path.parent_path();

		// Textures: one TextureAsset per (glTF texture, color space) pair actually used by materials.
		std::unordered_map<uint64_t, AssetHandle> textureHandles; // key: textureIndex * 2 + srgb
		auto importTexture = [&](size_t textureIndex, bool srgb) -> AssetHandle {
			const uint64_t key = static_cast<uint64_t>(textureIndex) * 2 + (srgb ? 1 : 0);
			if (auto it = textureHandles.find(key); it != textureHandles.end())
				return it->second;

			AssetHandle handle = 0;
			if (textureIndex < asset.textures.size() && asset.textures[textureIndex].imageIndex.has_value() && *asset.textures[textureIndex].imageIndex < asset.images.size())
			{
				std::string problem;
				const std::vector<uint8_t> bytes = GetImageBytes(asset, asset.images[*asset.textures[textureIndex].imageIndex], directory, problem);
				Ref<TextureAsset> texture = bytes.empty() ? nullptr : TextureAsset::LoadFromMemory(bytes.data(), bytes.size(), srgb);
				if (texture)
				{
					handle = DeriveSubAssetHandle(meshHandle, SubAssetKind::Texture, key);
					texture->Handle = handle;
					outResult.Textures.push_back(texture);
				}
				else
				{
					outResult.Warnings.push_back(fmt::format("texture {}: {}", textureIndex, problem.empty() ? "could not be decoded" : problem));
				}
			}
			else
			{
				outResult.Warnings.push_back(fmt::format("texture {} has no image", textureIndex));
			}
			textureHandles[key] = handle;
			return handle;
		};

		// Parallax from a material's extras; each height map is loaded once (as linear data, keeping 16 bits), however
		// many materials share it.
		struct HeightTexture
		{
			AssetHandle Handle = 0;
			std::string Problem; // Why there is no texture, reported for every material that names it.
		};
		std::unordered_map<std::string, HeightTexture> heightTextures;
		auto ApplyParallax = [&](size_t materialIndex, const fastgltf::Material& source, MaterialAsset& material) {
			const MaterialExtras& extras = documentExtras.Materials[materialIndex];
			const double scale = extras.RepeatMeters > 0.0 ? extras.DepthMeters / extras.RepeatMeters : 0.0;
			if (extras.HeightMap.empty() || !std::isfinite(scale) || scale <= 0.0)
			{
				outResult.Warnings.push_back(fmt::format("material '{}': parallax needs height_map, height_depth_m and texture_repeat_m", source.name.c_str()));
				return;
			}
			auto found = heightTextures.find(extras.HeightMap);
			if (found == heightTextures.end())
			{
				// Beside the model, in its "textures" folder (where Blender's exporter puts external images), then beside the
				// material's base color image.
				std::vector<std::filesystem::path> candidates{FromUtf8(extras.HeightMap), std::filesystem::path("textures") / FromUtf8(extras.HeightMap)};
				if (source.pbrData.baseColorTexture && source.pbrData.baseColorTexture->textureIndex < asset.textures.size())
				{
					const auto& texture = asset.textures[source.pbrData.baseColorTexture->textureIndex];
					if (texture.imageIndex && *texture.imageIndex < asset.images.size())
					{
						if (const auto* uri = std::get_if<fastgltf::sources::URI>(&asset.images[*texture.imageIndex].data))
							if (const std::optional<std::filesystem::path> file = UriPath(uri->uri))
								candidates.push_back(file->parent_path() / FromUtf8(extras.HeightMap));
					}
				}
				AssetHandle handle = 0;
				std::string problem; // The first problem other than a missing file, if any.
				for (const std::filesystem::path& candidate : candidates)
				{
					std::string candidateProblem;
					const std::vector<uint8_t> bytes = ReadModelFile<uint8_t>(directory, candidate, TextureAsset::MaxEncodedBytes, candidateProblem);
					if (bytes.empty())
					{
						if (problem.empty() && candidateProblem.find("not found") == std::string::npos)
							problem = candidateProblem;
						continue;
					}
					if (Ref<TextureAsset> texture = TextureAsset::LoadFromMemory(bytes.data(), bytes.size(), false, true, AssetType::Texture, true))
					{
						handle = DeriveSubAssetHandle(meshHandle, SubAssetKind::HeightTexture, HashName(extras.HeightMap));
						texture->Handle = handle;
						outResult.Textures.push_back(texture);
						problem.clear();
					}
					else
					{
						problem = fmt::format("height map '{}' could not be decoded", ToUtf8(candidate));
					}
					break;
				}
				if (!handle.IsValid() && problem.empty())
					problem = fmt::format("height map '{}' not found beside the model, in its textures folder or beside its base color image", extras.HeightMap);
				found = heightTextures.emplace(extras.HeightMap, HeightTexture{handle, problem}).first;
			}
			if (!found->second.Handle.IsValid())
			{
				outResult.Warnings.push_back(fmt::format("material '{}': {}", source.name.c_str(), found->second.Problem));
				return;
			}
			material.HeightTexture = found->second.Handle;
			// The shader's height scale is a depth in UV units; one texture repeat is one UV unit.
			material.HeightScale = static_cast<float>(std::clamp(scale, 0.0, 0.5));
		};

		// Materials (one slot per glTF material, plus a default slot for primitives without one).
		for (size_t i = 0; i < asset.materials.size(); i++)
		{
			const fastgltf::Material& source = asset.materials[i];
			auto material = CreateRef<MaterialAsset>();
			material->Handle = DeriveSubAssetHandle(meshHandle, SubAssetKind::Material, i);

			const auto& pbr = source.pbrData;
			material->BaseColorFactor = glm::vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
			material->MetallicFactor = pbr.metallicFactor;
			material->RoughnessFactor = pbr.roughnessFactor;
			if (pbr.baseColorTexture)
				material->BaseColorTexture = importTexture(pbr.baseColorTexture->textureIndex, true);
			if (pbr.metallicRoughnessTexture)
				material->MetallicRoughnessTexture = importTexture(pbr.metallicRoughnessTexture->textureIndex, false);
			if (source.normalTexture)
			{
				material->NormalTexture = importTexture(source.normalTexture->textureIndex, false);
				material->NormalScale = source.normalTexture->scale;
			}
			if (source.occlusionTexture)
			{
				material->OcclusionTexture = importTexture(source.occlusionTexture->textureIndex, false);
				material->OcclusionStrength = source.occlusionTexture->strength;
			}
			if (source.emissiveTexture)
				material->EmissiveTexture = importTexture(source.emissiveTexture->textureIndex, true);
			material->EmissiveFactor = glm::vec3(source.emissiveFactor[0], source.emissiveFactor[1], source.emissiveFactor[2]);
			material->EmissiveStrength = source.emissiveStrength;
			material->Alpha = source.alphaMode == fastgltf::AlphaMode::Blend  ? AlphaMode::Blend
							  : source.alphaMode == fastgltf::AlphaMode::Mask ? AlphaMode::Mask
																			  : AlphaMode::Opaque;
			material->AlphaCutoff = source.alphaCutoff;
			material->DoubleSided = source.doubleSided;
			if (i < documentExtras.Materials.size() && documentExtras.Materials[i].Parallax)
				ApplyParallax(i, source, *material);
			outResult.Materials.push_back(material);
		}

		const size_t sceneIndex = asset.defaultScene.value_or(0);
		if (asset.scenes.empty() || sceneIndex >= asset.scenes.size())
		{
			outError = fmt::format("'{}' contains no usable scene", ToUtf8(path));
			return false;
		}
		if (!asset.skins.empty())
			outResult.Warnings.push_back(fmt::format("{} skin(s) are not imported; skinned meshes appear in their bind pose", asset.skins.size()));

		// Material slots, shared by every mesh: one per glTF material, then one for primitives without a material
		// (rendered with the engine's default material) if any mesh needs it.
		const uint32_t defaultMaterialSlot = static_cast<uint32_t>(outResult.Materials.size());
		std::vector<AssetHandle> materialSlots;
		for (const auto& material : outResult.Materials)
			materialSlots.push_back(material->Handle);
		bool usesDefaultMaterial = false;

		// Each glTF mesh the scene uses is built once, in its own space, on first use. Geometry built in all (unique
		// meshes, and the combined mesh) is bounded, since zero-filled accessors need no backing bytes.
		std::vector<Ref<MeshAsset>> builtMeshes(asset.meshes.size());
		std::vector<bool> attempted(asset.meshes.size(), false);
		size_t builtVertices = 0;
		size_t builtIndices = 0;

		// Per-node mesh handles follow the mesh's name, which survives re-exports that renumber meshes (as Blender's
		// do when objects are added or reordered); unnamed meshes, and repeated names, fall back to the index.
		std::unordered_map<std::string_view, size_t> nameCounts;
		for (const fastgltf::Mesh& sourceMesh : asset.meshes)
			nameCounts[std::string_view(sourceMesh.name.data(), sourceMesh.name.size())]++;
		std::unordered_set<uint64_t> usedMeshHandles;
		auto meshHandleFor = [&](size_t meshIndex) {
			const std::string_view name(asset.meshes[meshIndex].name.data(), asset.meshes[meshIndex].name.size());
			AssetHandle handle = 0;
			if (!name.empty() && nameCounts[name] == 1)
				handle = DeriveSubAssetHandle(meshHandle, SubAssetKind::Mesh, HashName(name));
			if (!handle.IsValid() || usedMeshHandles.contains(handle))
				handle = DeriveSubAssetHandle(meshHandle, SubAssetKind::MeshByIndex, meshIndex);
			usedMeshHandles.insert(handle);
			return handle;
		};
		auto buildMesh = [&](size_t meshIndex) -> Ref<MeshAsset> {
			if (attempted[meshIndex])
				return builtMeshes[meshIndex];
			attempted[meshIndex] = true;
			const fastgltf::Mesh& sourceMesh = asset.meshes[meshIndex];
			auto built = CreateRef<MeshAsset>();
			built->Handle = meshHandleFor(meshIndex);
			bool builtUsesDefault = false;

			for (size_t p = 0; p < sourceMesh.primitives.size(); p++)
			{
				const fastgltf::Primitive& primitive = sourceMesh.primitives[p];
				if (primitive.type != fastgltf::PrimitiveType::Triangles)
				{
					outResult.Warnings.push_back(fmt::format("mesh '{}' primitive {}: only triangle lists are supported", sourceMesh.name.c_str(), p));
					continue;
				}

				const auto* positionAttribute = primitive.findAttribute("POSITION");
				if (positionAttribute == primitive.attributes.end() || !primitive.indicesAccessor.has_value())
				{
					outResult.Warnings.push_back(fmt::format("mesh '{}' primitive {}: missing positions or indices", sourceMesh.name.c_str(), p));
					continue;
				}

				// Every accessor this primitive reads must stay within real data.
				std::string problem;
				bool accessorsValid = ValidateAccessor(asset, positionAttribute->accessorIndex, fastgltf::AccessorType::Vec3, problem) &&
									  ValidateAccessor(asset, *primitive.indicesAccessor, fastgltf::AccessorType::Scalar, problem) &&
									  ValidateIndexComponents(asset.accessors[*primitive.indicesAccessor], *primitive.indicesAccessor, problem);
				for (const AttributeType& attribute : s_OptionalAttributes)
				{
					if (const auto* found = primitive.findAttribute(attribute.Name); accessorsValid && found != primitive.attributes.end())
						accessorsValid = ValidateAccessor(asset, found->accessorIndex, attribute.Type, problem);
				}
				if (!accessorsValid)
				{
					outError = fmt::format("mesh '{}' primitive {}: {}", sourceMesh.name.c_str(), p, problem);
					return nullptr;
				}
				if (asset.accessors[*primitive.indicesAccessor].count % 3 != 0)
				{
					outResult.Warnings.push_back(fmt::format("mesh '{}' primitive {}: index count is not a multiple of 3; skipped", sourceMesh.name.c_str(), p));
					continue;
				}
				builtVertices += asset.accessors[positionAttribute->accessorIndex].count;
				builtIndices += asset.accessors[*primitive.indicesAccessor].count;
				if (builtVertices > s_MaxElements * 4 || builtIndices > s_MaxElements * 4)
				{
					outError = fmt::format("'{}' is too large to import", ToUtf8(path));
					return nullptr;
				}

				Submesh submesh;
				submesh.Name = sourceMesh.name.empty() ? fmt::format("Mesh{}", meshIndex) : std::string(sourceMesh.name.c_str());
				submesh.VertexOffset = static_cast<uint32_t>(built->Vertices.size());
				submesh.IndexOffset = static_cast<uint32_t>(built->Indices.size());

				const fastgltf::Accessor& positions = asset.accessors[positionAttribute->accessorIndex];
				submesh.VertexCount = static_cast<uint32_t>(positions.count);
				built->Vertices.resize(built->Vertices.size() + positions.count);
				MeshVertex* vertices = built->Vertices.data() + submesh.VertexOffset;

				fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, positions, [&](glm::vec3 value, size_t index) { vertices[index].Position = value; });

				bool hasNormals = false;
				if (const auto* attribute = primitive.findAttribute("NORMAL"); attribute != primitive.attributes.end())
				{
					hasNormals = asset.accessors[attribute->accessorIndex].count == positions.count;
					if (hasNormals)
						fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, asset.accessors[attribute->accessorIndex], [&](glm::vec3 value, size_t index) { vertices[index].Normal = value; });
				}
				bool hasTangents = false;
				if (const auto* attribute = primitive.findAttribute("TANGENT"); attribute != primitive.attributes.end())
				{
					hasTangents = asset.accessors[attribute->accessorIndex].count == positions.count;
					if (hasTangents)
						fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, asset.accessors[attribute->accessorIndex], [&](glm::vec4 value, size_t index) { vertices[index].Tangent = value; });
				}
				if (const auto* attribute = primitive.findAttribute("TEXCOORD_0"); attribute != primitive.attributes.end())
				{
					if (asset.accessors[attribute->accessorIndex].count == positions.count)
						fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, asset.accessors[attribute->accessorIndex], [&](glm::vec2 value, size_t index) { vertices[index].UV = value; });
				}

				const fastgltf::Accessor& indexAccessor = asset.accessors[*primitive.indicesAccessor];
				submesh.IndexCount = static_cast<uint32_t>(indexAccessor.count);
				built->Indices.resize(built->Indices.size() + indexAccessor.count);
				fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, built->Indices.data() + submesh.IndexOffset);

				for (uint32_t i = 0; i < submesh.IndexCount; i++)
				{
					if (built->Indices[submesh.IndexOffset + i] >= submesh.VertexCount)
					{
						outError = fmt::format("mesh '{}' primitive {} has an out-of-range index", submesh.Name, p);
						return nullptr;
					}
				}

				if (!hasNormals)
				{
					// Flat normals from face orientation (accumulated, so shared vertices are smoothed).
					for (uint32_t i = 0; i < submesh.VertexCount; i++)
						vertices[i].Normal = glm::vec3(0.0f);
					for (uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3)
					{
						const uint32_t* tri = &built->Indices[submesh.IndexOffset + i];
						const glm::vec3 n = glm::cross(vertices[tri[1]].Position - vertices[tri[0]].Position, vertices[tri[2]].Position - vertices[tri[0]].Position);
						vertices[tri[0]].Normal += n;
						vertices[tri[1]].Normal += n;
						vertices[tri[2]].Normal += n;
					}
					for (uint32_t i = 0; i < submesh.VertexCount; i++)
					{
						const float length = glm::length(vertices[i].Normal);
						vertices[i].Normal = length > 0.0f ? vertices[i].Normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
					}
				}
				if (!hasTangents)
					ComputeTangents(built->Vertices, built->Indices, submesh.IndexOffset, submesh.IndexCount, submesh.VertexOffset, submesh.VertexCount);

				if (primitive.materialIndex.has_value() && *primitive.materialIndex < outResult.Materials.size())
				{
					submesh.MaterialIndex = static_cast<uint32_t>(*primitive.materialIndex);
				}
				else
				{
					submesh.MaterialIndex = defaultMaterialSlot;
					builtUsesDefault = true;
				}
				built->Submeshes.push_back(std::move(submesh));
			}
			if (built->Submeshes.empty())
				return nullptr;

			built->Materials = materialSlots;
			if (builtUsesDefault)
			{
				built->Materials.push_back(0);
				usesDefaultMaterial = true;
			}
			built->ComputeBounds();
			std::string validationError;
			if (!built->Validate(validationError))
			{
				outError = fmt::format("mesh '{}' is invalid: {}", sourceMesh.name.c_str(), validationError);
				return nullptr;
			}
			builtMeshes[meshIndex] = built;
			outResult.NodeMeshes.push_back(built);
			return built;
		};

		// The combined mesh: each node's mesh appended with the node's world transform.
		auto mesh = CreateRef<MeshAsset>();
		mesh->Handle = meshHandle;
		auto appendToCombined = [&](const MeshAsset& source, const glm::mat4& transform) {
			if (mesh->Vertices.size() + source.Vertices.size() > s_MaxElements * 4 || mesh->Indices.size() + source.Indices.size() > s_MaxElements * 4)
			{
				outError = fmt::format("'{}' is too large to import", ToUtf8(path));
				return;
			}
			const uint32_t vertexBase = static_cast<uint32_t>(mesh->Vertices.size());
			const uint32_t indexBase = static_cast<uint32_t>(mesh->Indices.size());
			mesh->Vertices.insert(mesh->Vertices.end(), source.Vertices.begin(), source.Vertices.end());
			mesh->Indices.insert(mesh->Indices.end(), source.Indices.begin(), source.Indices.end());
			for (Submesh submesh : source.Submeshes)
			{
				submesh.VertexOffset += vertexBase;
				submesh.IndexOffset += indexBase;
				submesh.Transform = transform * submesh.Transform;
				mesh->Submeshes.push_back(std::move(submesh));
			}
		};

		// Iterative scene traversal with a visited set (glTF node graphs must be trees; cycles or shared nodes in
		// malformed files are ignored) and a depth limit. Nodes are recorded in pre-order, so parents come first.
		struct PendingNode
		{
			size_t Index;
			fastgltf::math::fmat4x4 Parent;
			size_t Depth;
			std::optional<size_t> ParentRecord;
		};
		std::vector<bool> visited(asset.nodes.size(), false);
		std::vector<std::optional<size_t>> recordOf(asset.nodes.size()); // glTF node -> MeshImportResult::Nodes.
		std::vector<PendingNode> stack;
		const auto& roots = asset.scenes[sceneIndex].nodeIndices;
		for (auto it = roots.rbegin(); it != roots.rend(); ++it)
			stack.push_back({*it, fastgltf::math::fmat4x4(), 0, std::nullopt});
		while (!stack.empty() && outError.empty())
		{
			const PendingNode pending = stack.back();
			stack.pop_back();
			if (pending.Index >= asset.nodes.size() || visited[pending.Index])
			{
				outResult.Warnings.push_back(fmt::format("node {} is invalid or referenced more than once; skipped", pending.Index));
				continue;
			}
			if (pending.Depth > s_MaxNodeDepth)
			{
				outError = fmt::format("'{}' has a node hierarchy deeper than {}", ToUtf8(path), s_MaxNodeDepth);
				break;
			}
			visited[pending.Index] = true;

			const fastgltf::Node& node = asset.nodes[pending.Index];
			const fastgltf::math::fmat4x4 matrix = fastgltf::getTransformMatrix(node, pending.Parent);

			ImportedNode record = MakeNode(asset, node, pending.Index, outResult.Warnings);
			record.Parent = pending.ParentRecord;
			if (pending.Index < documentExtras.Nodes.size())
				record.CastShadows = documentExtras.Nodes[pending.Index].CastShadows;
			if (node.meshIndex.has_value() && *node.meshIndex < asset.meshes.size())
			{
				if (Ref<MeshAsset> built = buildMesh(*node.meshIndex))
				{
					record.Mesh = built->Handle;
					glm::mat4 transform;
					std::memcpy(glm::value_ptr(transform), matrix.data(), sizeof(float) * 16);
					appendToCombined(*built, transform);
				}
			}
			const size_t recordIndex = outResult.Nodes.size();
			outResult.Nodes.push_back(std::move(record));
			recordOf[pending.Index] = recordIndex;

			for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
				stack.push_back({*it, matrix, pending.Depth + 1, recordIndex});
		}

		if (!outError.empty())
			return false;
		// Imported nodes whose extras.cast_shadow is not a boolean get one warning together: a file can hold many.
		size_t ignoredCastShadows = 0;
		size_t firstIgnoredCastShadow = 0;
		for (size_t i = 0; i < documentExtras.Nodes.size() && i < visited.size(); i++)
		{
			if (!visited[i] || !documentExtras.Nodes[i].InvalidCastShadows)
				continue;
			if (ignoredCastShadows == 0)
				firstIgnoredCastShadow = i;
			ignoredCastShadows++;
		}
		if (ignoredCastShadows > 0)
			outResult.Warnings.push_back(fmt::format("extras.cast_shadow is not true or false on {} node(s), the first being node {}; ignored", ignoredCastShadows, firstIgnoredCastShadow));
		ImportAnimations(asset, meshHandle, recordOf, outResult);
		if (mesh->Submeshes.empty())
		{
			outError = fmt::format("'{}' contains no triangle meshes", ToUtf8(path));
			return false;
		}
		mesh->Materials = materialSlots;
		if (usesDefaultMaterial)
		{
			// Every mesh's default slot sits at the same index; meshes that do not need it simply lack it.
			mesh->Materials.push_back(0);
		}

		mesh->ComputeBounds();
		std::string validationError;
		if (!mesh->Validate(validationError))
		{
			outError = fmt::format("'{}' produced an invalid mesh: {}", ToUtf8(path), validationError);
			return false;
		}

		outResult.Mesh = mesh;
		return true;
	}

}
