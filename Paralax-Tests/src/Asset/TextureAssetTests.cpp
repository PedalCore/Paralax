#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/Asset.h>
#include <Paralax/Asset/TextureAsset.h>
#include <Paralax/Renderer/Image.h>

#include <cmath>
#include <cstring>
#include <fstream>

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
	CHECK(AssetTypeFromExtension("theme.MP3") == AssetType::Audio);
	CHECK(AssetTypeFromExtension("hit.flac") == AssetType::Audio);
	CHECK(AssetTypeFromExtension("music.ogg") == AssetType::None); // Not decoded, so not importable.
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

TEST_CASE("TextureAsset: an empty image and a mip chain without mip 0 fail verification")
{
	const uint8_t pixel[] = {10, 20, 30, 255};
	Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(TextureAsset::CreateRGBA8(0, 1, pixel, false), Testing::AssertionFailedError);
	CHECK_THROWS_AS(TextureAsset::CreateRGBA8(1, 0, pixel, false), Testing::AssertionFailedError);
	CHECK_THROWS_AS(TextureAsset::CreateRGBA8(1, 1, nullptr, false), Testing::AssertionFailedError);
	CHECK_THROWS_AS(TextureAsset(AssetType::Texture).GenerateMips(), Testing::AssertionFailedError);
	CHECK_THROWS_AS(TextureAsset(AssetType::EnvironmentMap).GenerateMips(), Testing::AssertionFailedError);

	// The smallest real image is fine.
	Ref<TextureAsset> smallest = TextureAsset::CreateRGBA8(1, 1, pixel, false);
	REQUIRE(smallest);
	CHECK(smallest->GetMipCount() == 1);
	CHECK(smallest->Mips[0][2] == 30);
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

namespace {

	std::vector<uint8_t> HDRBytes(const std::string& resolution, std::initializer_list<uint8_t> pixels)
	{
		const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + resolution + "\n";
		std::vector<uint8_t> bytes(header.begin(), header.end());
		bytes.insert(bytes.end(), pixels);
		return bytes;
	}

	float FloatChannel(const TextureAsset& texture, size_t mip, size_t channel)
	{
		float value = 0;
		std::memcpy(&value, texture.Mips[mip].data() + channel * sizeof(float), sizeof(float));
		return value;
	}

}

TEST_CASE("TextureAsset: flat and scanline HDR preserve linear energy and explicit asset types")
{
	const auto flat = HDRBytes("-Y 1 +X 2", {128, 64, 32, 130, 128, 64, 32, 131});
	auto texture = TextureAsset::LoadFromMemory(flat.data(), flat.size(), true);
	REQUIRE(texture);
	CHECK(texture->GetType() == AssetType::Texture);
	CHECK(texture->Format == TextureFormat::RGBA32F);
	CHECK_FALSE(texture->SRGB);
	CHECK(texture->Width == 2);
	CHECK(texture->Height == 1);
	CHECK(FloatChannel(*texture, 0, 0) == 2.0f);
	CHECK(FloatChannel(*texture, 0, 1) == 1.0f);
	CHECK(FloatChannel(*texture, 0, 3) == 1.0f);
	CHECK(FloatChannel(*texture, 1, 0) == 3.0f);

	// Two scanlines exercise literal and repeated runs in each channel.
	const auto rle = HDRBytes("-Y 2 +X 8", {2, 2, 0, 8, 136, 128, 136, 64, 136, 32, 136, 130,
											2, 2, 0, 8, 8, 128, 128, 128, 128, 128, 128, 128, 128, 136, 64, 136, 32, 136, 131});
	auto environment = TextureAsset::LoadFromMemory(rle.data(), rle.size(), true, true, AssetType::EnvironmentMap);
	REQUIRE(environment);
	CHECK(environment->GetType() == AssetType::EnvironmentMap);
	CHECK(environment->Width == 8);
	CHECK(environment->Height == 2);
	CHECK(environment->GetMipCount() == 4);
	CHECK(FloatChannel(*environment, 0, 0) == 2.0f);
	CHECK(FloatChannel(*environment, 0, 8 * 4) == 4.0f);
	CHECK(FloatChannel(*environment, 3, 0) == 3.0f);

	const auto directory = Testing::CreateTestDirectory("HDRFile");
	{
		std::ofstream file(directory / "sky.hdr", std::ios::binary);
		file.write(reinterpret_cast<const char*>(rle.data()), static_cast<std::streamsize>(rle.size()));
	}
	auto loaded = TextureAsset::LoadFromFile(directory / "sky.hdr", false, false, AssetType::EnvironmentMap);
	REQUIRE(loaded);
	CHECK(loaded->GetType() == AssetType::EnvironmentMap);
	CHECK(loaded->GetMipCount() == 1);
	CHECK(loaded->Mips[0] == environment->Mips[0]);

	Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(CreateRef<TextureAsset>(AssetType::Mesh), Testing::AssertionFailedError);
	CHECK_THROWS_AS(TextureAsset::LoadFromMemory(flat.data(), flat.size(), false, true, AssetType::Mesh), Testing::AssertionFailedError);
	CHECK_THROWS_AS(TextureAsset::LoadFromFile(directory / "sky.hdr", false, true, AssetType::None), Testing::AssertionFailedError);
}

TEST_CASE("TextureAsset: every truncated HDR prefix is rejected before decoding")
{
	Testing::ScopedLogCapture log;
	for (const auto& bytes : {HDRBytes("-Y 1 +X 2", {128, 64, 32, 130, 128, 64, 32, 131}),
							  HDRBytes("-Y 2 +X 8", {2, 2, 0, 8, 136, 128, 136, 64, 136, 32, 136, 130,
													 2, 2, 0, 8, 136, 128, 136, 64, 136, 32, 136, 131})})
	{
		for (size_t size = 0; size < bytes.size(); size++)
		{
			CAPTURE(size);
			CHECK_FALSE(TextureAsset::LoadFromMemory(bytes.data(), size, false));
		}
	}
	CHECK(log.Contains(spdlog::level::err, "Failed to decode texture"));
}

TEST_CASE("TextureAsset: hostile HDR dimensions headers and scanline runs are rejected")
{
	Testing::ScopedLogCapture log;
	for (const char* resolution : {"-Y 0 +X 8", "-Y -1 +X 8", "-Y 1 +X 16385", "-Y 16385 +X 1",
								   "-Y 4097 +X 4096", "-Y 1 +X 9999999999999999999999", "-Y 1 +X 8 trailing", "+Y 1 +X 8", "-Y nan +X 8"})
	{
		CAPTURE(resolution);
		const auto bytes = HDRBytes(resolution, {2, 2, 0, 8, 136, 128, 136, 64, 136, 32, 136, 130});
		CHECK_FALSE(TextureAsset::LoadFromMemory(bytes.data(), bytes.size(), false));
	}
	for (const auto& bytes : {HDRBytes("-Y 1 +X 8", {2, 2, 0, 9, 136, 128, 136, 64, 136, 32, 136, 130}),
							  HDRBytes("-Y 1 +X 8", {2, 2, 0, 8, 0, 128}),
							  HDRBytes("-Y 1 +X 8", {2, 2, 0, 8, 137, 128}),
							  HDRBytes("-Y 1 +X 8", {2, 2, 0, 8, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9}),
							  HDRBytes("-Y 1 +X 8", {2, 2, 0, 8, 136})})
		CHECK_FALSE(TextureAsset::LoadFromMemory(bytes.data(), bytes.size(), false));

	std::string largeHeader = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n";
	while (largeHeader.size() < 65536)
		largeHeader += "# Bounded comment lines exercise the total header cap, not the per-line cap.\n";
	largeHeader += "\n-Y 1 +X 1\n";
	largeHeader.append(4, static_cast<char>(128));
	for (const std::string& header : {std::string("#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 1 +X 1\n"),
									  std::string("#?RADIANCE\n") + std::string(1024, 'x') + "\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n",
									  largeHeader})
		CHECK_FALSE(TextureAsset::LoadFromMemory(reinterpret_cast<const uint8_t*>(header.data()), header.size(), false));

	const uint8_t oneByte = 0;
	CHECK_FALSE(TextureAsset::LoadFromMemory(&oneByte, TextureAsset::MaxEncodedBytes + 1, false));
	const auto path = Testing::CreateTestDirectory("HDROversize") / "huge.hdr";
	{
		std::ofstream file(path, std::ios::binary);
		file.seekp(static_cast<std::streamoff>(TextureAsset::MaxEncodedBytes));
		file.put('\0');
	}
	CHECK_FALSE(TextureAsset::LoadFromFile(path, false));
	std::filesystem::remove(path);
}

TEST_CASE("TextureAsset: extreme finite HDR samples remain finite through every mip")
{
	const auto bytes = HDRBytes("-Y 2 +X 2", {255, 255, 255, 255, 255, 255, 255, 255,
											  255, 255, 255, 255, 255, 255, 255, 255});
	auto texture = TextureAsset::LoadFromMemory(bytes.data(), bytes.size(), false);
	REQUIRE(texture);
	REQUIRE(texture->GetMipCount() == 2);
	CHECK(std::isfinite(FloatChannel(*texture, 1, 0)));
	CHECK(FloatChannel(*texture, 1, 0) == FloatChannel(*texture, 0, 0));
}

TEST_CASE("TextureAsset: a PNG with an empty data chunk before its data still decodes")
{
	// stb_image copied zero bytes from a null buffer here (undefined behaviour, patched; see cmake/patches). Found by
	// the loader fuzz tests under UBSan.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("TextureEmptyIdat");
	Image image(4, 4);
	image.SetPixel(1, 2, {10, 20, 30, 255});
	REQUIRE(image.SavePNG(directory / "Sample.png"));
	std::ifstream file(directory / "Sample.png", std::ios::binary);
	std::vector<uint8_t> png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	// Chunks start after the 8-byte signature; insert a zero-length IDAT before the first one (stb skips CRCs).
	size_t at = 8;
	while (at + 8 <= png.size() && std::string(png.begin() + static_cast<std::ptrdiff_t>(at + 4), png.begin() + static_cast<std::ptrdiff_t>(at + 8)) != "IDAT")
	{
		const uint32_t length = (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | uint32_t(png[at + 3]);
		at += 12 + length;
	}
	REQUIRE(at + 8 <= png.size());
	const uint8_t empty[] = {0, 0, 0, 0, 'I', 'D', 'A', 'T', 0, 0, 0, 0};
	png.insert(png.begin() + static_cast<std::ptrdiff_t>(at), std::begin(empty), std::end(empty));

	Ref<TextureAsset> texture = TextureAsset::LoadFromMemory(png.data(), png.size(), false);
	REQUIRE(texture);
	CHECK(texture->Width == 4);
	CHECK(texture->Mips[0][(2 * 4 + 1) * 4 + 1] == 20);
}

TEST_CASE("TextureAsset: 16-bit images keep their precision as linear data only when asked")
{
	// Neighbouring 16-bit levels that 8 bits cannot tell apart.
	const std::vector<uint16_t> values{0, 1000, 1001, 65535};
	const std::vector<uint8_t> png = Paralax::Testing::MakePng16(2, 2, values);

	Ref<TextureAsset> precise = TextureAsset::LoadFromMemory(png.data(), png.size(), false, false, AssetType::Texture, true);
	REQUIRE(precise);
	CHECK(precise->Format == TextureFormat::RGBA32F);
	CHECK_FALSE(precise->SRGB);
	const auto* texels = reinterpret_cast<const float*>(precise->Mips[0].data());
	for (size_t i = 0; i < values.size(); i++)
	{
		CAPTURE(i);
		CHECK(texels[i * 4] == static_cast<float>(values[i]) / 65535.0f); // Grey in red...
		CHECK(texels[i * 4 + 1] == texels[i * 4]);                        // ...green and blue,
		CHECK(texels[i * 4 + 3] == 1.0f);                                 // opaque.
	}
	CHECK(texels[4] != texels[8]); // 1000 and 1001 stay apart.
	// A float texture gets float mips.
	Ref<TextureAsset> mipped = TextureAsset::LoadFromMemory(png.data(), png.size(), false, true, AssetType::Texture, true);
	REQUIRE(mipped);
	REQUIRE(mipped->Mips.size() == 2);
	CHECK(mipped->Mips[1].size() == 16);

	// Not asked for, or as colour: the usual RGBA8 image (16-bit normal maps stay small).
	for (bool srgb : {false, true})
	{
		CAPTURE(srgb);
		Ref<TextureAsset> usual = TextureAsset::LoadFromMemory(png.data(), png.size(), srgb, true, AssetType::Texture, srgb);
		REQUIRE(usual);
		CHECK(usual->Format == TextureFormat::RGBA8);
	}
	Ref<TextureAsset> byDefault = TextureAsset::LoadFromMemory(png.data(), png.size(), false);
	REQUIRE(byDefault);
	CHECK(byDefault->Format == TextureFormat::RGBA8);
}

TEST_CASE("TextureAsset: 16-bit grey-alpha, RGB and RGBA images keep every channel at full precision")
{
	struct Case
	{
		uint32_t Channels;
		std::vector<uint16_t> Samples;
		std::vector<float> Expected; // RGBA of the one pixel.
	};
	for (const Case& c : {Case{2, {1001, 32768}, {1001, 1001, 1001, 32768}}, Case{3, {1001, 2002, 3003}, {1001, 2002, 3003, 65535}},
						  Case{4, {1001, 2002, 3003, 4004}, {1001, 2002, 3003, 4004}}})
	{
		CAPTURE(c.Channels);
		const std::vector<uint8_t> png = Paralax::Testing::MakePng16(1, 1, c.Samples, c.Channels);
		Ref<TextureAsset> texture = TextureAsset::LoadFromMemory(png.data(), png.size(), false, false, AssetType::Texture, true);
		REQUIRE(texture);
		REQUIRE(texture->Format == TextureFormat::RGBA32F);
		const auto* texels = reinterpret_cast<const float*>(texture->Mips[0].data());
		for (size_t i = 0; i < 4; i++)
			CHECK(texels[i] == c.Expected[i] / 65535.0f);
	}
}
