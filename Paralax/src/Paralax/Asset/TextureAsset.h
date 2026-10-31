#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Paralax {

	enum class TextureFormat
	{
		RGBA8,  // 8 bits per channel; color data is sRGB-encoded when TextureAsset::SRGB is set
		RGBA32F // linear HDR (environment maps)
	};

	// A 2D image with its full mip chain in CPU memory. Mip 0 is the full-resolution image; each further level halves
	// the size (rounding down, minimum 1). Rows are top to bottom, tightly packed.
	class TextureAsset : public Asset
	{
	public:
		AssetType GetType() const override { return AssetType::Texture; }

		uint32_t Width = 0;
		uint32_t Height = 0;
		TextureFormat Format = TextureFormat::RGBA8;
		// True for color textures (base color, emissive); false for data (normals, roughness/metallic, height).
		bool SRGB = true;
		std::vector<std::vector<uint8_t>> Mips;

		uint32_t GetMipCount() const { return static_cast<uint32_t>(Mips.size()); }
		uint32_t GetBytesPerPixel() const { return Format == TextureFormat::RGBA8 ? 4 : 16; }

		// Creates an RGBA8 texture from pixels (width * height * 4 bytes) and generates mips. Verifies the size.
		static Ref<TextureAsset> CreateRGBA8(uint32_t width, uint32_t height, const uint8_t* pixels, bool srgb, bool generateMips = true);
		// Loads PNG/JPEG/TGA/BMP (as RGBA8) or Radiance HDR (as RGBA32F). Returns nullptr (with an error) on failure.
		static Ref<TextureAsset> LoadFromFile(const std::filesystem::path& path, bool srgb, bool generateMips = true);
		// Decodes an image already in memory (e.g. embedded in a glTF file).
		static Ref<TextureAsset> LoadFromMemory(const uint8_t* data, size_t size, bool srgb, bool generateMips = true);

		// Rebuilds mips 1..N from mip 0 with a 2x2 box filter (in linear space for sRGB textures).
		void GenerateMips();
	};

}
