#pragma once

#include <glm/vec4.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Paralax {

	// An 8-bit RGBA image in CPU memory, rows top to bottom. Used for captures, golden images and tests.
	class Image
	{
	public:
		Image() = default;
		Image(uint32_t width, uint32_t height);

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		bool IsEmpty() const { return m_Width == 0 || m_Height == 0; }

		std::vector<uint8_t>& GetPixels() { return m_Pixels; }
		const std::vector<uint8_t>& GetPixels() const { return m_Pixels; }

		// Pixel access; coordinates are verified to be in range.
		glm::u8vec4 GetPixel(uint32_t x, uint32_t y) const;
		void SetPixel(uint32_t x, uint32_t y, glm::u8vec4 color);

		// Writes a PNG, creating parent directories. Returns false (with an error logged) on failure.
		bool SavePNG(const std::filesystem::path& path) const;
		// Loads a PNG (or any format stb_image reads) as RGBA8. Returns an empty image on failure.
		static Image LoadFromFile(const std::filesystem::path& path);

		// Largest per-channel absolute difference between two images of equal size (256 if sizes differ).
		static uint32_t MaxChannelDifference(const Image& a, const Image& b);

	private:
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		std::vector<uint8_t> m_Pixels;
	};

}
