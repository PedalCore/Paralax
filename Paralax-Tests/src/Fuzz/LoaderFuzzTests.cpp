#include <doctest/doctest.h>

#include "Asset/GltfTestDocument.h"
#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MaterialSerializer.h>
#include <Paralax/Asset/MeshImporter.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Audio/AudioClip.h>
#include <Paralax/Project/Project.h>
#include <Paralax/Renderer/Image.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Prefab.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Deterministic mutation fuzzing of every loader that reads untrusted files. Each test takes a valid input, mutates it
// thousands of times with a fixed seed, and feeds every variant to the loader, which must reject or accept it without
// crashing, throwing, or (under ASan and UBSan, which also instrument the third-party loaders) touching memory it
// should not. Failures report the iteration, and reproduce exactly on every platform: the inputs contain no random
// UUIDs, and mutations come straight from mt19937_64, whose output the standard fixes (unlike the distributions').

using namespace Paralax;
using json = nlohmann::json;

namespace {

	class Mutator
	{
	public:
		explicit Mutator(uint64_t seed)
			: m_Random(seed)
		{
		}

		// Modulo bias is irrelevant for fuzzing; what matters is the same sequence on every standard library.
		size_t Below(size_t bound) { return bound == 0 ? 0 : static_cast<size_t>(m_Random() % bound); }

		// One to four byte-level edits: bit flips, boundary values, truncation, deleted, duplicated or inserted runs.
		std::vector<uint8_t> Bytes(std::vector<uint8_t> data)
		{
			static constexpr uint8_t Interesting[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x20, 0x0A};
			static constexpr uint32_t InterestingWords[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x10000u, 65535u};
			const size_t edits = 1 + Below(4);
			for (size_t edit = 0; edit < edits && !data.empty(); edit++)
			{
				const size_t at = Below(data.size());
				switch (Below(7))
				{
					case 0: data[at] ^= static_cast<uint8_t>(1u << Below(8)); break;
					case 1: data[at] = Interesting[Below(std::size(Interesting))]; break;
					case 2: data.resize(at); break;
					case 3: data.erase(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), at + 1 + Below(64)))); break;
					case 4:
					{
						const std::vector<uint8_t> run(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), at + 1 + Below(64))));
						data.insert(data.begin() + static_cast<std::ptrdiff_t>(Below(data.size())), run.begin(), run.end());
						break;
					}
					case 5:
					{
						std::vector<uint8_t> noise(1 + Below(16));
						for (uint8_t& byte : noise)
							byte = static_cast<uint8_t>(Below(256));
						data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), noise.begin(), noise.end());
						break;
					}
					default:
						if (data.size() >= 4)
						{
							const uint32_t word = InterestingWords[Below(std::size(InterestingWords))];
							std::memcpy(data.data() + std::min(at, data.size() - 4), &word, sizeof(word));
						}
						break;
				}
			}
			return data;
		}

		// One to three structural edits to a JSON document: a node replaced by a value of another type (huge numbers,
		// deep nesting, long strings, empty containers), removed, or duplicated into a sibling key.
		json Json(json document)
		{
			const size_t edits = 1 + Below(3);
			for (size_t edit = 0; edit < edits; edit++)
			{
				std::vector<json::json_pointer> nodes;
				Collect(document, json::json_pointer(), nodes);
				const json::json_pointer& target = nodes[Below(nodes.size())];
				const size_t kind = Below(4);
				if (kind == 0 && !target.empty())
				{
					json& parent = document[target.parent_pointer()];
					if (parent.is_object())
						parent.erase(target.back());
					else if (parent.is_array() && !parent.empty())
						parent.erase(parent.begin() + static_cast<std::ptrdiff_t>(Below(parent.size())));
				}
				else if (kind == 1 && !target.empty() && document[target.parent_pointer()].is_object())
					document[target.parent_pointer()]["Fuzz" + std::to_string(Below(4))] = document[target];
				else
					document[target] = Replacement();
			}
			return document;
		}

	private:
		static void Collect(const json& node, const json::json_pointer& at, std::vector<json::json_pointer>& out)
		{
			out.push_back(at);
			if (out.size() > 4096)
				return;
			if (node.is_object())
			{
				for (auto it = node.begin(); it != node.end(); ++it)
					Collect(it.value(), at / it.key(), out);
			}
			else if (node.is_array())
			{
				for (size_t i = 0; i < node.size(); i++)
					Collect(node[i], at / i, out);
			}
		}

		json Replacement()
		{
			switch (Below(14))
			{
				case 0: return nullptr;
				case 1: return Below(2) == 0;
				case 2: return -1;
				case 3: return 0;
				case 4: return 1e308;
				case 5: return -1e308;
				case 6: return std::numeric_limits<int64_t>::min();
				case 7: return std::numeric_limits<uint64_t>::max();
				case 8: return "";
				case 9: return std::string(300 + Below(2000), 'x');
				case 10: return json::array();
				case 11: return json::object();
				case 12:
				{
					json deep = json::array();
					for (int i = 0; i < 200; i++)
						deep = json::array({deep});
					return deep;
				}
				default: return json::array({1, "two", nullptr, json::object({{"three", 3}})});
			}
		}

		std::mt19937_64 m_Random;
	};

	// Iterations for a fuzz test: the base count, times PARALAX_FUZZ_SCALE (1 to 1000) for a deeper search by hand,
	// e.g. PARALAX_FUZZ_SCALE=50 Paralax-Tests -tc="Fuzz*" under the asan preset.
	int Iterations(int base)
	{
		const char* scale = std::getenv("PARALAX_FUZZ_SCALE");
		const int factor = scale ? std::atoi(scale) : 1;
		return base * std::clamp(factor, 1, 1000);
	}

	std::vector<uint8_t> ReadBytes(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

	void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	std::vector<uint8_t> Text(const std::string& text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// Mutated JSON, as an edited document or as text that may no longer parse; nullopt when it does not.
	std::optional<json> MutatedDocument(Mutator& mutator, const json& document)
	{
		if (mutator.Below(3) != 0)
			return mutator.Json(document);
		const std::vector<uint8_t> bytes = mutator.Bytes(Text(document.dump()));
		json parsed = json::parse(bytes.begin(), bytes.end(), nullptr, false);
		if (parsed.is_discarded())
			return std::nullopt;
		return parsed;
	}

	// A scene using every registered component, with a little hierarchy.
	json MakeSceneDocument()
	{
		Scene scene;
		Entity root = scene.CreateEntityWithUUID(UUID(1001), "Root");
		Entity child = scene.CreateEntityWithUUID(UUID(1002), "Child");
		scene.SetParent(child, root, false);
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			if (!info.Has(scene.GetRegistry(), child.GetHandle()))
				info.Add(scene.GetRegistry(), child.GetHandle());
		}
		return SceneSerializer::Serialize(scene);
	}

}

TEST_CASE("Fuzz: mutated scene and prefab documents never crash the loaders")
{
	Paralax::Testing::ScopedLogLevel quiet(spdlog::level::off);
	Mutator mutator(0x5CE7E);
	const json scene = MakeSceneDocument();
	json prefab = scene;
	prefab["Type"] = "Prefab";
	for (int iteration = 0; iteration < Iterations(1500); iteration++)
	{
		CAPTURE(iteration);
		const bool asPrefab = iteration % 3 == 2;
		std::optional<json> document = MutatedDocument(mutator, asPrefab ? prefab : scene);
		if (!document)
			continue;
		std::string error;
		if (asPrefab)
		{
			CHECK_NOTHROW(PrefabAsset::FromJson(*document, error));
		}
		else
		{
			Scene loaded;
			CHECK_NOTHROW(SceneSerializer::Deserialize(loaded, *document, error));
		}
	}
}

TEST_CASE("Fuzz: mutated material documents never crash the loader")
{
	Paralax::Testing::ScopedLogLevel quiet(spdlog::level::off);
	Mutator mutator(0x3A7E41A1);
	MaterialAsset original;
	const json document = MaterialSerializer::Serialize(original);
	for (int iteration = 0; iteration < Iterations(1500); iteration++)
	{
		CAPTURE(iteration);
		std::optional<json> mutated = MutatedDocument(mutator, document);
		if (!mutated)
			continue;
		MaterialAsset material;
		std::string error;
		CHECK_NOTHROW(MaterialSerializer::Deserialize(material, *mutated, error));
	}
}

TEST_CASE("Fuzz: mutated images never crash the texture loader")
{
	Paralax::Testing::ScopedLogLevel quiet(spdlog::level::off);
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("FuzzImages");
	Image image(16, 12);
	for (uint32_t y = 0; y < 12; y++)
		for (uint32_t x = 0; x < 16; x++)
			image.SetPixel(x, y, {static_cast<uint8_t>(x * 16), static_cast<uint8_t>(y * 20), 128, 255});
	REQUIRE(image.SavePNG(directory / "Sample.png"));
	const std::vector<uint8_t> png = ReadBytes(directory / "Sample.png");
	// A Radiance HDR file: header, then flat RGBE pixels.
	std::vector<uint8_t> hdr = Text("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n");
	for (int i = 0; i < 32; i++)
		hdr.insert(hdr.end(), {static_cast<uint8_t>(64 + i), 96, 128, 130});

	Mutator mutator(0x1A6E5);
	for (int iteration = 0; iteration < Iterations(3000); iteration++)
	{
		CAPTURE(iteration);
		const std::vector<uint8_t> bytes = mutator.Bytes(iteration % 2 == 0 ? png : hdr);
		if (bytes.empty())
			continue;
		CHECK_NOTHROW(TextureAsset::LoadFromMemory(bytes.data(), bytes.size(), iteration % 4 == 0));
	}
}

TEST_CASE("Fuzz: mutated audio files never crash the decoder")
{
	Paralax::Testing::ScopedLogLevel quiet(spdlog::level::off);
	std::vector<float> tone(2000);
	for (size_t i = 0; i < tone.size(); i++)
		tone[i] = 0.5f * std::sin(static_cast<float>(i) * 0.05f);
	const std::vector<uint8_t> wav16 = Paralax::Testing::MakeWav(2, 22050, tone);
	const std::vector<uint8_t> wavFloat = Paralax::Testing::MakeWav(1, 48000, tone, 32);
	std::vector<uint8_t> mp3;
	for (int frame = 0; frame < 8; frame++)
	{
		mp3.insert(mp3.end(), {0xFF, 0xFB, 0x90, 0xC0});
		mp3.insert(mp3.end(), 417 - 4, 0);
	}
	// The head of a real FLAC file (stream info and the first frames).
	std::vector<uint8_t> flac = ReadBytes(PX_TEST_FLAC_FILE);
	REQUIRE(flac.size() > 32768);
	flac.resize(32768);
	const std::vector<uint8_t>* samples[] = {&wav16, &wavFloat, &mp3, &flac};

	Mutator mutator(0xA0D10);
	for (int iteration = 0; iteration < Iterations(2400); iteration++)
	{
		CAPTURE(iteration);
		const std::vector<uint8_t> bytes = mutator.Bytes(*samples[iteration % 4]);
		if (bytes.empty())
			continue;
		// A small decoded-size limit keeps lying headers from making this slow.
		CHECK_NOTHROW(AudioClip::LoadFromMemory(bytes.data(), bytes.size(), 4u * 1024 * 1024));
	}
}

TEST_CASE("Fuzz: mutated glTF files never crash the importer")
{
	Paralax::Testing::ScopedLogLevel quiet(spdlog::level::off);
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("FuzzGltf");
	const json document = Paralax::Testing::MakeTriangleDocument(directory);
	const std::vector<uint8_t> glb = Paralax::Testing::MakeTriangleGlb(directory);
	Mutator mutator(0x6177F);
	for (int iteration = 0; iteration < Iterations(800); iteration++)
	{
		CAPTURE(iteration);
		// Structural edits mostly (the importer sees a well-formed but hostile document), raw bytes of the text and
		// binary forms sometimes (the binary form's chunk headers are a classic source of out-of-bounds reads).
		const std::filesystem::path path = directory / "Fuzz.gltf";
		if (iteration % 4 == 3)
			WriteBytes(path, mutator.Bytes(Text(document.dump())));
		else if (iteration % 4 == 2)
			WriteBytes(path, mutator.Bytes(glb));
		else
			WriteBytes(path, Text(mutator.Json(document).dump()));
		MeshImportResult result;
		std::string error;
		CHECK_NOTHROW(MeshImporter::Import(path, UUID(7), result, error));
	}
}

TEST_CASE("Fuzz: mutated project files and asset registries never crash loading")
{
	Paralax::Testing::ScopedLogLevel quiet(spdlog::level::off);
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("FuzzProject");
	REQUIRE(Project::Create(root / "Game", "Game"));
	std::ofstream(root / "Game" / "Assets" / "Notes.lua") << "return {}";
	{
		AssetManager assets(root / "Game" / "Assets");
		REQUIRE(assets.ImportAsset("Notes.lua").IsValid());
	}
	const std::vector<uint8_t> projectFile = ReadBytes(root / "Game" / "Game.pxproj");
	const std::vector<uint8_t> registry = ReadBytes(root / "Game" / "Assets" / AssetManager::RegistryFileName);
	const json projectDocument = json::parse(projectFile.begin(), projectFile.end());
	json registryDocument = json::parse(registry.begin(), registry.end());
	REQUIRE(registryDocument["Assets"].size() == 1);
	registryDocument["Assets"][0]["Handle"] = "4242"; // Imported under a random handle; fix it so runs reproduce.

	Mutator mutator(0x940EC7);
	for (int iteration = 0; iteration < Iterations(600); iteration++)
	{
		CAPTURE(iteration);
		const bool project = iteration % 2 == 0;
		const json& base = project ? projectDocument : registryDocument;
		const std::vector<uint8_t> bytes = mutator.Below(3) == 0 ? mutator.Bytes(Text(base.dump())) : Text(mutator.Json(base).dump());
		if (project)
		{
			WriteBytes(root / "Game" / "Game.pxproj", bytes);
			CHECK_NOTHROW(Project::Load(root / "Game"));
		}
		else
		{
			WriteBytes(root / "Game" / "Assets" / AssetManager::RegistryFileName, bytes);
			AssetManager assets(root / "Game" / "Assets");
			CHECK_NOTHROW(assets.LoadRegistry());
		}
	}
}
