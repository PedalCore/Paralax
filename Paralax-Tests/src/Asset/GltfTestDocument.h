#pragma once

#include <Paralax/Renderer/Image.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// A small, valid glTF document (text and binary) for importer tests and the loader fuzz tests.
namespace Paralax::Testing {

	using json = nlohmann::json;

	inline std::string Base64(const std::vector<uint8_t>& bytes)
	{
		static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		for (size_t i = 0; i < bytes.size(); i += 3)
		{
			const uint32_t a = bytes[i];
			const uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
			const uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
			const uint32_t triple = (a << 16) | (b << 8) | c;
			out += table[(triple >> 18) & 63];
			out += table[(triple >> 12) & 63];
			out += i + 1 < bytes.size() ? table[(triple >> 6) & 63] : '=';
			out += i + 2 < bytes.size() ? table[triple & 63] : '=';
		}
		return out;
	}

	template<typename T>
	inline void Append(std::vector<uint8_t>& buffer, const T& value)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
	}

	// The triangle's vertex and index data: positions, normals, UVs, then 16-bit indices (104 bytes).
	inline std::vector<uint8_t> MakeTriangleBuffer()
	{
		std::vector<uint8_t> buffer;
		const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
		const float normals[] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
		const float uvs[] = {0, 0, 1, 0, 0, 1};
		const uint16_t indices[] = {0, 1, 2, 0};
		for (float v : positions)
			Append(buffer, v);
		for (float v : normals)
			Append(buffer, v);
		for (float v : uvs)
			Append(buffer, v);
		for (uint16_t v : indices)
			Append(buffer, v);
		return buffer;
	}

	// Writes a glTF with one triangle (positions, normals, UVs, 16-bit indices) used by two nodes: a root translated
	// by (10, 0, 0) and its child translated by (0, 5, 0). The material has a base color factor and an external
	// base color texture. trailingBytes unused zero bytes end the buffer (to vary the base64 padding).
	inline json MakeTriangleDocument(const std::filesystem::path& directory, bool withNormals = true, size_t trailingBytes = 0)
	{
		Image texture(2, 2);
		texture.SetPixel(0, 0, {255, 0, 0, 255});
		REQUIRE(texture.SavePNG(directory / "albedo.png"));

		std::vector<uint8_t> buffer = MakeTriangleBuffer();
		buffer.resize(buffer.size() + trailingBytes, 0);

		json attributes = {{"POSITION", 0}, {"TEXCOORD_0", 2}};
		if (withNormals)
			attributes["NORMAL"] = 1;

		json document = {
			{"asset", {{"version", "2.0"}}},
			{"scene", 0},
			{"scenes", {{{"nodes", {0}}}}},
			{"nodes", {{{"mesh", 0}, {"translation", {10, 0, 0}}, {"children", {1}}}, {{"mesh", 0}, {"translation", {0, 5, 0}}}}},
			{"meshes", {{{"name", "Tri"}, {"primitives", {{{"attributes", attributes}, {"indices", 3}, {"material", 0}}}}}}},
			{"materials", {{{"pbrMetallicRoughness", {{"baseColorFactor", {0.5, 0.25, 1.0, 1.0}}, {"metallicFactor", 0.75}, {"roughnessFactor", 0.2}, {"baseColorTexture", {{"index", 0}}}}}, {"emissiveFactor", {1, 0, 0}}, {"alphaMode", "MASK"}, {"alphaCutoff", 0.3}, {"doubleSided", true}}}},
			{"textures", {{{"source", 0}}}},
			{"images", {{{"uri", "albedo.png"}}}},
			{"buffers", {{{"byteLength", buffer.size()}, {"uri", "data:application/octet-stream;base64," + Base64(buffer)}}}},
			{"bufferViews", {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}}, {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}}, {{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 24}}, {{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 6}}}},
			{"accessors",
			 {{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
			  {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
			  {{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
			  {{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}}},
		};

		return document;
	}

	// The same triangle as binary glTF (GLB): the document as the JSON chunk, its buffer as the BIN chunk.
	inline std::vector<uint8_t> MakeTriangleGlb(const std::filesystem::path& directory)
	{
		json document = MakeTriangleDocument(directory);
		document["buffers"][0].erase("uri");

		std::string text = document.dump();
		text.resize((text.size() + 3) / 4 * 4, ' '); // Chunks are 4-byte aligned; JSON pads with spaces, BIN with zeros.
		std::vector<uint8_t> bin = MakeTriangleBuffer();
		bin.resize((bin.size() + 3) / 4 * 4, 0);

		std::vector<uint8_t> glb;
		Append(glb, uint32_t(0x46546C67)); // "glTF"
		Append(glb, uint32_t(2));
		Append(glb, static_cast<uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
		Append(glb, static_cast<uint32_t>(text.size()));
		Append(glb, uint32_t(0x4E4F534A)); // "JSON"
		glb.insert(glb.end(), text.begin(), text.end());
		Append(glb, static_cast<uint32_t>(bin.size()));
		Append(glb, uint32_t(0x004E4942)); // "BIN\0"
		glb.insert(glb.end(), bin.begin(), bin.end());
		return glb;
	}

	inline std::filesystem::path WriteGltf(const std::filesystem::path& directory, const json& document, const char* name = "triangle.gltf")
	{
		const std::filesystem::path path = directory / name;
		std::ofstream(path) << document.dump();
		return path;
	}

}
