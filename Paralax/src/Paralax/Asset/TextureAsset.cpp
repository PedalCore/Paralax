#include "pxpch.h"
#include "Paralax/Asset/TextureAsset.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Paralax {

	namespace {

		// sRGB <-> linear lookup for 8-bit values; the inverse uses the exact formula.
		const std::array<float, 256>& SRGBToLinearTable()
		{
			static const std::array<float, 256> s_Table = [] {
				std::array<float, 256> table{};
				for (int i = 0; i < 256; i++)
				{
					const float c = static_cast<float>(i) / 255.0f;
					table[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
				}
				return table;
			}();
			return s_Table;
		}

		uint8_t LinearToSRGB8(float linear)
		{
			linear = std::clamp(linear, 0.0f, 1.0f);
			const float c = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(c * 255.0f));
		}

		Ref<TextureAsset> FromDecoded(uint32_t width, uint32_t height, bool hdr, void* pixels, bool srgb, bool generateMips)
		{
			auto texture = CreateRef<TextureAsset>();
			texture->Width = width;
			texture->Height = height;
			texture->Format = hdr ? TextureFormat::RGBA32F : TextureFormat::RGBA8;
			texture->SRGB = hdr ? false : srgb;
			const size_t bytes = static_cast<size_t>(width) * height * texture->GetBytesPerPixel();
			texture->Mips.emplace_back(static_cast<uint8_t*>(pixels), static_cast<uint8_t*>(pixels) + bytes);
			if (generateMips)
				texture->GenerateMips();
			return texture;
		}

	}

	Ref<TextureAsset> TextureAsset::CreateRGBA8(uint32_t width, uint32_t height, const uint8_t* pixels, bool srgb, bool generateMips)
	{
		PX_CORE_VERIFY(width > 0 && height > 0 && pixels, "CreateRGBA8 requires a non-empty image");
		auto texture = CreateRef<TextureAsset>();
		texture->Width = width;
		texture->Height = height;
		texture->Format = TextureFormat::RGBA8;
		texture->SRGB = srgb;
		texture->Mips.emplace_back(pixels, pixels + static_cast<size_t>(width) * height * 4);
		if (generateMips)
			texture->GenerateMips();
		return texture;
	}

	Ref<TextureAsset> TextureAsset::LoadFromFile(const std::filesystem::path& path, bool srgb, bool generateMips)
	{
		const std::string utf8Path = reinterpret_cast<const char*>(path.u8string().c_str());
		int width = 0, height = 0, channels = 0;
		const bool hdr = stbi_is_hdr(utf8Path.c_str()) != 0;
		void* pixels = hdr ? static_cast<void*>(stbi_loadf(utf8Path.c_str(), &width, &height, &channels, 4))
						   : static_cast<void*>(stbi_load(utf8Path.c_str(), &width, &height, &channels, 4));
		if (!pixels)
		{
			PX_CORE_ERROR("Failed to load texture '{}': {}", utf8Path, stbi_failure_reason());
			return nullptr;
		}

		Ref<TextureAsset> texture = FromDecoded(static_cast<uint32_t>(width), static_cast<uint32_t>(height), hdr, pixels, srgb, generateMips);
		stbi_image_free(pixels);
		return texture;
	}

	Ref<TextureAsset> TextureAsset::LoadFromMemory(const uint8_t* data, size_t size, bool srgb, bool generateMips)
	{
		if (!data || size == 0 || size > static_cast<size_t>(INT32_MAX))
		{
			PX_CORE_ERROR("Failed to decode texture: invalid buffer");
			return nullptr;
		}

		const int length = static_cast<int>(size);
		int width = 0, height = 0, channels = 0;
		const bool hdr = stbi_is_hdr_from_memory(data, length) != 0;
		void* pixels = hdr ? static_cast<void*>(stbi_loadf_from_memory(data, length, &width, &height, &channels, 4))
						   : static_cast<void*>(stbi_load_from_memory(data, length, &width, &height, &channels, 4));
		if (!pixels)
		{
			PX_CORE_ERROR("Failed to decode texture: {}", stbi_failure_reason());
			return nullptr;
		}

		Ref<TextureAsset> texture = FromDecoded(static_cast<uint32_t>(width), static_cast<uint32_t>(height), hdr, pixels, srgb, generateMips);
		stbi_image_free(pixels);
		return texture;
	}

	void TextureAsset::GenerateMips()
	{
		PX_CORE_VERIFY(!Mips.empty(), "GenerateMips requires mip 0");
		Mips.resize(1);

		uint32_t width = Width;
		uint32_t height = Height;
		const auto& toLinear = SRGBToLinearTable();

		while (width > 1 || height > 1)
		{
			const uint32_t nextWidth = std::max(width / 2, 1u);
			const uint32_t nextHeight = std::max(height / 2, 1u);
			const std::vector<uint8_t>& source = Mips.back();
			std::vector<uint8_t> destination(static_cast<size_t>(nextWidth) * nextHeight * GetBytesPerPixel());

			for (uint32_t y = 0; y < nextHeight; y++)
			{
				for (uint32_t x = 0; x < nextWidth; x++)
				{
					// Sample the 2x2 footprint, clamped at the edges for odd sizes.
					const uint32_t x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
					const uint32_t y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
					const uint32_t xs[4] = {x0, x1, x0, x1};
					const uint32_t ys[4] = {y0, y0, y1, y1};

					for (uint32_t channel = 0; channel < 4; channel++)
					{
						if (Format == TextureFormat::RGBA32F)
						{
							float sum = 0.0f;
							for (int i = 0; i < 4; i++)
							{
								float value;
								std::memcpy(&value, &source[((static_cast<size_t>(ys[i]) * width + xs[i]) * 4 + channel) * 4], sizeof(float));
								sum += value;
							}
							const float average = sum * 0.25f;
							std::memcpy(&destination[((static_cast<size_t>(y) * nextWidth + x) * 4 + channel) * 4], &average, sizeof(float));
						}
						else
						{
							const bool linearize = SRGB && channel < 3; // Alpha is always linear.
							float sum = 0.0f;
							for (int i = 0; i < 4; i++)
							{
								const uint8_t value = source[(static_cast<size_t>(ys[i]) * width + xs[i]) * 4 + channel];
								sum += linearize ? toLinear[value] : static_cast<float>(value) / 255.0f;
							}
							const float average = sum * 0.25f;
							destination[(static_cast<size_t>(y) * nextWidth + x) * 4 + channel] =
								linearize ? LinearToSRGB8(average) : static_cast<uint8_t>(std::lround(std::clamp(average, 0.0f, 1.0f) * 255.0f));
						}
					}
				}
			}

			Mips.push_back(std::move(destination));
			width = nextWidth;
			height = nextHeight;
		}
	}

}
