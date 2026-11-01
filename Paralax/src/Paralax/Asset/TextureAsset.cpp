#include "pxpch.h"
#include "Paralax/Asset/TextureAsset.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string_view>

namespace Paralax {

	namespace {

		void VerifyTextureType(AssetType type)
		{
			PX_CORE_VERIFY(type == AssetType::Texture || type == AssetType::EnvironmentMap, "TextureAsset requires a texture or environment type");
		}

		bool ValidDimensions(int width, int height, bool hdr)
		{
			return width > 0 && height > 0 && static_cast<uint32_t>(width) <= TextureAsset::MaxDimension &&
				   static_cast<uint32_t>(height) <= TextureAsset::MaxDimension &&
				   static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * (hdr ? 16u : 4u) <= TextureAsset::MaxDecodedBytes;
		}

		// stb tolerates EOF in flat RGBE pixels. Validate the complete payload before it allocates or decodes.
		bool ValidateHDR(const uint8_t* data, size_t size, int& width, int& height)
		{
			const std::string_view bytes(reinterpret_cast<const char*>(data), size);
			size_t offset = 0;
			auto readLine = [&](std::string_view& line) {
				const size_t end = bytes.find('\n', offset);
				if (end == std::string_view::npos || end >= 65536 || end - offset > 1023)
					return false;
				line = bytes.substr(offset, end - offset);
				offset = end + 1;
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1);
				return line.find('\0') == std::string_view::npos;
			};
			std::string_view line;
			if (!readLine(line) || (line != "#?RADIANCE" && line != "#?RGBE"))
				return false;
			bool format = false;
			for (;;)
			{
				if (!readLine(line))
					return false;
				if (line.empty())
					break;
				if (line.starts_with("FORMAT="))
				{
					if (line != "FORMAT=32-bit_rle_rgbe")
						return false;
					format = true;
				}
			}
			if (!format || !readLine(line) || !line.starts_with("-Y "))
				return false;
			line.remove_prefix(3);
			const auto y = std::from_chars(line.data(), line.data() + line.size(), height);
			if (y.ec != std::errc())
				return false;
			line.remove_prefix(static_cast<size_t>(y.ptr - line.data()));
			if (!line.starts_with(" +X "))
				return false;
			line.remove_prefix(4);
			const auto x = std::from_chars(line.data(), line.data() + line.size(), width);
			if (x.ec != std::errc() || x.ptr != line.data() + line.size() || !ValidDimensions(width, height, true))
				return false;

			if (size - offset < 4)
				return false;
			const bool rle = width >= 8 && data[offset] == 2 && data[offset + 1] == 2 && (data[offset + 2] & 128) == 0;
			if (!rle)
				return static_cast<size_t>(width) * static_cast<size_t>(height) * 4 <= size - offset;

			for (int row = 0; row < height; row++)
			{
				if (size - offset < 4 || data[offset] != 2 || data[offset + 1] != 2 ||
					((static_cast<int>(data[offset + 2]) << 8) | data[offset + 3]) != width)
					return false;
				offset += 4;
				for (int channel = 0; channel < 4; channel++)
				{
					int written = 0;
					while (written < width)
					{
						if (offset == size)
							return false;
						const uint8_t code = data[offset++];
						const int count = code > 128 ? code - 128 : code;
						const size_t encoded = code > 128 ? 1 : static_cast<size_t>(count);
						if (count == 0 || count > width - written || encoded > size - offset)
							return false;
						offset += encoded;
						written += count;
					}
				}
			}
			return true;
		}

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

		Ref<TextureAsset> FromDecoded(uint32_t width, uint32_t height, bool hdr, void* pixels, bool srgb, bool generateMips, AssetType type)
		{
			auto texture = CreateRef<TextureAsset>(type);
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

	TextureAsset::TextureAsset(AssetType type)
		: m_Type(type)
	{
		VerifyTextureType(type);
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

	Ref<TextureAsset> TextureAsset::LoadFromFile(const std::filesystem::path& path, bool srgb, bool generateMips, AssetType type)
	{
		VerifyTextureType(type);
		// Read once: header validation and decoding must see the same bytes, including if the file changes.
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		const auto size = file ? file.tellg() : std::streampos(-1);
		if (size <= 0 || size > static_cast<std::streamoff>(MaxEncodedBytes))
		{
			PX_CORE_ERROR("Failed to load texture '{}': unreadable, empty or exceeds encoded size limit", path.string());
			return nullptr;
		}
		std::vector<uint8_t> data(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
		{
			PX_CORE_ERROR("Failed to load texture '{}': incomplete read", path.string());
			return nullptr;
		}
		return LoadFromMemory(data.data(), data.size(), srgb, generateMips, type);
	}

	Ref<TextureAsset> TextureAsset::LoadFromMemory(const uint8_t* data, size_t size, bool srgb, bool generateMips, AssetType type, bool keepFullPrecision)
	{
		VerifyTextureType(type);
		if (!data || size == 0 || size > MaxEncodedBytes)
		{
			PX_CORE_ERROR("Failed to decode texture: invalid buffer or encoded size limit exceeded");
			return nullptr;
		}

		const int length = static_cast<int>(size);
		int width = 0, height = 0, channels = 0;
		const bool hdr = stbi_is_hdr_from_memory(data, length) != 0;
		// 16-bit data (height maps) can keep its precision as linear floats when asked for.
		const bool wide = keepFullPrecision && !hdr && !srgb && stbi_is_16_bit_from_memory(data, length) != 0;
		if ((hdr && !ValidateHDR(data, size, width, height)) ||
			(!hdr && !stbi_info_from_memory(data, length, &width, &height, &channels)) || !ValidDimensions(width, height, hdr || wide))
		{
			PX_CORE_ERROR("Failed to decode texture: malformed image or decoded size limit exceeded");
			return nullptr;
		}
		if (wide)
		{
			stbi_us* wordPixels = stbi_load_16_from_memory(data, length, &width, &height, &channels, 4);
			if (!wordPixels)
			{
				PX_CORE_ERROR("Failed to decode texture: {}", stbi_failure_reason());
				return nullptr;
			}
			std::vector<float> values(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
			for (size_t i = 0; i < values.size(); i++)
				values[i] = static_cast<float>(wordPixels[i]) / 65535.0f;
			stbi_image_free(wordPixels);
			return FromDecoded(static_cast<uint32_t>(width), static_cast<uint32_t>(height), true, values.data(), false, generateMips, type);
		}
		void* pixels = hdr ? static_cast<void*>(stbi_loadf_from_memory(data, length, &width, &height, &channels, 4))
						   : static_cast<void*>(stbi_load_from_memory(data, length, &width, &height, &channels, 4));
		if (!pixels)
		{
			PX_CORE_ERROR("Failed to decode texture: {}", stbi_failure_reason());
			return nullptr;
		}
		if (hdr)
		{
			const auto* values = static_cast<const float*>(pixels);
			const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
			if (!std::all_of(values, values + count, [](float value) { return std::isfinite(value); }))
			{
				stbi_image_free(pixels);
				PX_CORE_ERROR("Failed to decode texture: non-finite HDR values");
				return nullptr;
			}
		}

		Ref<TextureAsset> texture = FromDecoded(static_cast<uint32_t>(width), static_cast<uint32_t>(height), hdr, pixels, srgb, generateMips, type);
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
								// Weight before summing: finite HDR samples can overflow an unweighted sum.
								sum += value * 0.25f;
							}
							const float average = sum;
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
