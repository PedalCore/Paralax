#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/Asset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/Image.h>

#include <cstring>

using namespace Paralax;

TEST_CASE("Asset: types map to and from names and file extensions")
{
	for (AssetType type : {AssetType::Texture, AssetType::EnvironmentMap, AssetType::Material, AssetType::Mesh, AssetType::Scene, AssetType::Prefab, AssetType::Script, AssetType::Audio})
		CHECK(AssetTypeFromString(AssetTypeToString(type)) == type);
	CHECK(AssetTypeFromString("Nonsense") == AssetType::None);

	CHECK(AssetTypeFromExtension("a/b/Brick.PNG") == AssetType::Texture);
	CHECK(AssetTypeFromExtension("sky.hdr") == AssetType::EnvironmentMap);
	CHECK(AssetTypeFromExtension("ship.glb") == AssetType::Mesh);
	CHECK(AssetTypeFromExtension("ship.gltf") == AssetType::Mesh);
	CHECK(AssetTypeFromExtension("level.pxscene") == AssetType::Scene);
	CHECK(AssetTypeFromExtension("player.lua") == AssetType::Script);
	CHECK(AssetTypeFromExtension("jump.wav") == AssetType::Audio);
	CHECK(AssetTypeFromExtension("notes.txt") == AssetType::None);
	CHECK(AssetTypeFromExtension("no_extension") == AssetType::None);
}

TEST_CASE("TextureAsset: mip chains halve each level down to 1x1, including odd sizes")
{
	std::vector<uint8_t> pixels(5 * 3 * 4, 255);
	Ref<TextureAsset> texture = TextureAsset::CreateRGBA8(5, 3, pixels.data(), false);
	REQUIRE(texture->GetMipCount() == 3); // 5x3, 2x1, 1x1
	CHECK(texture->Mips[0].size() == 5 * 3 * 4);
	CHECK(texture->Mips[1].size() == 2 * 1 * 4);
	CHECK(texture->Mips[2].size() == 1 * 1 * 4);

	Ref<TextureAsset> noMips = TextureAsset::CreateRGBA8(5, 3, pixels.data(), false, false);
	CHECK(noMips->GetMipCount() == 1);
}

TEST_CASE("TextureAsset: sRGB textures are filtered in linear space, data textures are not")
{
	// A 2x1 image: black and white.
	const uint8_t pixels[] = {0, 0, 0, 255, 255, 255, 255, 255};

	Ref<TextureAsset> color = TextureAsset::CreateRGBA8(2, 1, pixels, true);
	// Linear average 0.5 encodes to sRGB 188.
	CHECK(color->Mips[1][0] == 188);
	CHECK(color->Mips[1][3] == 255); // Alpha is linear.

	Ref<TextureAsset> data = TextureAsset::CreateRGBA8(2, 1, pixels, false);
	CHECK(data->Mips[1][0] == 128);
}

TEST_CASE("TextureAsset: loads PNG files and reports failures")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("TextureLoad");
	Image image(4, 4);
	image.SetPixel(0, 0, {10, 20, 30, 255});
	REQUIRE(image.SavePNG(directory / "tex.png"));

	Ref<TextureAsset> texture = TextureAsset::LoadFromFile(directory / "tex.png", true);
	REQUIRE(texture);
	CHECK(texture->Width == 4);
	CHECK(texture->Height == 4);
	CHECK(texture->Format == TextureFormat::RGBA8);
	CHECK(texture->SRGB);
	CHECK(texture->GetMipCount() == 3);
	CHECK(texture->Mips[0][0] == 10);
	CHECK(texture->Mips[0][2] == 30);

	Paralax::Testing::ScopedLogCapture capture;
	CHECK(TextureAsset::LoadFromFile(directory / "missing.png", true) == nullptr);
	const uint8_t garbage[] = {1, 2, 3, 4};
	CHECK(TextureAsset::LoadFromMemory(garbage, sizeof(garbage), true) == nullptr);
	CHECK(TextureAsset::LoadFromMemory(nullptr, 10, true) == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "Failed to"));
}

TEST_CASE("TextureAsset: float mips average exactly")
{
	auto texture = CreateRef<TextureAsset>();
	texture->Width = 2;
	texture->Height = 2;
	texture->Format = TextureFormat::RGBA32F;
	texture->SRGB = false;
	std::vector<float> values = {0, 0, 0, 1, 2, 2, 2, 1, 4, 4, 4, 1, 6, 6, 6, 1};
	texture->Mips.emplace_back(reinterpret_cast<uint8_t*>(values.data()), reinterpret_cast<uint8_t*>(values.data()) + values.size() * sizeof(float));
	texture->GenerateMips();
	REQUIRE(texture->GetMipCount() == 2);
	float average = 0.0f;
	std::memcpy(&average, texture->Mips[1].data(), sizeof(float));
	CHECK(average == 3.0f);
}
