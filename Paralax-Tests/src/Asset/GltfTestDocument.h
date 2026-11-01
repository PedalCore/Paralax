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

	// Adds a small scene around the triangle's nodes (0, the root, and its child 1, both without names):
	//   node 2 "Camera" (perspective, a root) with child node 3 "TopView" (orthographic);
	//   node 4 "Sun" (directional light, a root) with children node 5 "Lamp" (point light, no range) and node 6
	//   "Spot" (spot light, given as a matrix: translate (1, 2, 3), rotate 90 degrees about Y, scale 2).
	// Light intensities are photometric, 683 times the strengths they should import as (3, 80 and 1).
	inline void AddCamerasAndLights(json& document)
	{
		document["cameras"] = {{{"type", "perspective"}, {"perspective", {{"yfov", 0.8}, {"znear", 0.05}, {"zfar", 500.0}}}},
							   {{"type", "orthographic"}, {"orthographic", {{"xmag", 4.0}, {"ymag", 3.0}, {"znear", 0.1}, {"zfar", 50.0}}}}};
		document["extensionsUsed"] = {"KHR_lights_punctual"};
		document["extensions"]["KHR_lights_punctual"]["lights"] = {
			{{"type", "directional"}, {"color", {1.0, 0.9, 0.8}}, {"intensity", 3.0 * 683.0}},
			{{"type", "point"}, {"intensity", 80.0 * 683.0}},
			{{"type", "spot"}, {"intensity", 683.0}, {"range", 7.0}, {"spot", {{"innerConeAngle", 0.2}, {"outerConeAngle", 0.5}}}}};
		auto light = [](int index) { return json{{"KHR_lights_punctual", {{"light", index}}}}; };
		// translate(1, 2, 3) * rotate(90 degrees about Y) * scale(2), column-major.
		const json spotMatrix = {0.0, 0.0, -2.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 1.0, 2.0, 3.0, 1.0};
		json& nodes = document["nodes"];
		nodes.push_back({{"name", "Camera"}, {"camera", 0}, {"translation", {0.0, 1.0, 5.0}}, {"children", {3}}});
		nodes.push_back({{"name", "TopView"}, {"camera", 1}, {"rotation", {-0.70710678, 0.0, 0.0, 0.70710678}}});
		nodes.push_back({{"name", "Sun"}, {"extensions", light(0)}, {"children", {5, 6}}});
		nodes.push_back({{"name", "Lamp"}, {"extensions", light(1)}, {"translation", {0.0, 3.0, 0.0}}});
		nodes.push_back({{"name", "Spot"}, {"extensions", light(2)}, {"matrix", spotMatrix}});
		document["scenes"][0]["nodes"] = {0, 2, 4};
	}

	// Adds animations to a document that has had AddCamerasAndLights, with their keys appended to its buffer:
	//   "LampBob"    Lamp (node 5) translation, LINEAR, keys 0, 1, 2 s: y 3 -> 4 -> 3.
	//   "CameraMove" Camera (node 2) translation, LINEAR, 0 -> 2 s: x 0 -> 4; and TopView (node 3, its child) rotation,
	//                STEP, keys 0 and 1 s: identity, then 90 degrees about Y. Played by Camera.
	//   "Mixed"      Node0 (node 0) and Lamp (node 5) scale, CUBICSPLINE with zero tangents, 0 -> 1 s: 1 -> 2. They
	//                share no node above them, so the model's root plays it.
	inline void AddAnimations(json& document)
	{
		std::vector<uint8_t> buffer = MakeTriangleBuffer(); // 104 bytes, already 4-byte aligned.
		auto addAccessor = [&](const std::vector<float>& floats, const char* type, size_t count) {
			const size_t offset = buffer.size();
			for (float value : floats)
				Append(buffer, value);
			document["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", floats.size() * 4}});
			json accessor = {{"bufferView", document["bufferViews"].size() - 1}, {"componentType", 5126}, {"count", count}, {"type", type}};
			if (std::string(type) == "SCALAR")
			{
				accessor["min"] = {floats.front()};
				accessor["max"] = {floats.back()};
			}
			document["accessors"].push_back(accessor);
			return document["accessors"].size() - 1;
		};
		const float s = 0.70710678f;
		const size_t bobTimes = addAccessor({0.0f, 1.0f, 2.0f}, "SCALAR", 3);
		const size_t bobValues = addAccessor({0, 3, 0, 0, 4, 0, 0, 3, 0}, "VEC3", 3);
		const size_t moveTimes = addAccessor({0.0f, 2.0f}, "SCALAR", 2);
		const size_t moveValues = addAccessor({0, 1, 5, 4, 1, 5}, "VEC3", 2);
		const size_t turnTimes = addAccessor({0.0f, 1.0f}, "SCALAR", 2);
		const size_t turnValues = addAccessor({0, 0, 0, 1, 0, s, 0, s}, "VEC4", 2);
		const size_t growValues = addAccessor({0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0}, "VEC3", 6);
		document["animations"] = {
			{{"name", "LampBob"}, {"samplers", {{{"input", bobTimes}, {"output", bobValues}}}}, {"channels", {{{"sampler", 0}, {"target", {{"node", 5}, {"path", "translation"}}}}}}},
			{{"name", "CameraMove"},
			 {"samplers", {{{"input", moveTimes}, {"output", moveValues}}, {{"input", turnTimes}, {"output", turnValues}, {"interpolation", "STEP"}}}},
			 {"channels", {{{"sampler", 0}, {"target", {{"node", 2}, {"path", "translation"}}}}, {{"sampler", 1}, {"target", {{"node", 3}, {"path", "rotation"}}}}}}},
			{{"name", "Mixed"},
			 {"samplers", {{{"input", turnTimes}, {"output", growValues}, {"interpolation", "CUBICSPLINE"}}}},
			 {"channels", {{{"sampler", 0}, {"target", {{"node", 0}, {"path", "scale"}}}}, {{"sampler", 0}, {"target", {{"node", 5}, {"path", "scale"}}}}}}}};
		document["buffers"][0] = {{"byteLength", buffer.size()}, {"uri", "data:application/octet-stream;base64," + Base64(buffer)}};
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
