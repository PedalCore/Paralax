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
	// the size (rounding down, minimum 1). Rows are top to bottom, tightly packed. Owns its pixels; main-thread-only.
	class TextureAsset : public Asset
	{
	public:
		// The registered use determines the type, independently of pixel format. Verifies a texture/environment type.
		explicit TextureAsset(AssetType type = AssetType::Texture);
		AssetType GetType() const override { return m_Type; }

		static constexpr size_t MaxEncodedBytes = 128u * 1024 * 1024;
		static constexpr size_t MaxDecodedBytes = 256u * 1024 * 1024;
		static constexpr uint32_t MaxDimension = 16384;

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
		// Loads PNG/JPEG/TGA/BMP (as RGBA8) or Radiance HDR (as RGBA32F). Returns an owned image or nullptr with an error.
		// Factories verify that type is Texture or EnvironmentMap before reading or allocating pixels.
		// Limits: 128 MiB encoded, 256 MiB decoded base level, 16384 per axis. HDR supports top-to-bottom RGBE,
		// flat pixels or scanline RLE, with a 64 KiB header and 1023-byte lines. Truncated/non-finite HDR is rejected.
		static Ref<TextureAsset> LoadFromFile(const std::filesystem::path& path, bool srgb, bool generateMips = true, AssetType type = AssetType::Texture);
		// Decodes an image already in memory (e.g. embedded in a glTF file). With keepFullPrecision, a 16-bit image (PNG,
		// PSD or PNM) loaded as linear data (srgb false) keeps its precision as RGBA32F, values 0..1 (for height maps,
		// where 8 bits show as steps); otherwise it is reduced to RGBA8 like any other image. RGBA32F takes four times the
		// memory, and its size limit is reached at a quarter of the pixels.
		static Ref<TextureAsset> LoadFromMemory(const uint8_t* data, size_t size, bool srgb, bool generateMips = true, AssetType type = AssetType::Texture, bool keepFullPrecision = false);

		// Rebuilds mips 1..N from mip 0 with a 2x2 box filter (in linear space for sRGB textures).
		void GenerateMips();

	private:
		AssetType m_Type;
	};

}
