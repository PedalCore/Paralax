#include "pxpch.h"
#include "Paralax/Renderer/Image.h"

#include <glm/vec4.hpp>

// stb is compiled into exactly this translation unit.
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wold-style-cast"
	#pragma GCC diagnostic ignored "-Wcast-align"
	#pragma GCC diagnostic ignored "-Wdouble-promotion"
	#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
	#pragma GCC diagnostic ignored "-Wshadow"
	#pragma GCC diagnostic ignored "-Wsign-compare"
	#pragma GCC diagnostic ignored "-Wunused-function"
	#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#elif defined(_MSC_VER)
	#pragma warning(push, 0)
#endif
#if defined(__SANITIZE_ADDRESS__)
	// GCC cannot compile stb_image's SSE2 intrinsics at -O0 with AddressSanitizer (non-immediate shift operands).
	#define STBI_NO_SIMD
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
	#pragma warning(pop)
#endif

#include <algorithm>
#include <cstdlib>

namespace Paralax {

	Image::Image(uint32_t width, uint32_t height)
		: m_Width(width)
		, m_Height(height)
		, m_Pixels(static_cast<size_t>(width) * height * 4, 0)
	{
	}

	glm::u8vec4 Image::GetPixel(uint32_t x, uint32_t y) const
	{
		PX_CORE_VERIFY(x < m_Width && y < m_Height, "Pixel ({}, {}) out of range for {}x{} image", x, y, m_Width, m_Height);
		const size_t offset = (static_cast<size_t>(y) * m_Width + x) * 4;
		return {m_Pixels[offset], m_Pixels[offset + 1], m_Pixels[offset + 2], m_Pixels[offset + 3]};
	}

	void Image::SetPixel(uint32_t x, uint32_t y, glm::u8vec4 color)
	{
		PX_CORE_VERIFY(x < m_Width && y < m_Height, "Pixel ({}, {}) out of range for {}x{} image", x, y, m_Width, m_Height);
		const size_t offset = (static_cast<size_t>(y) * m_Width + x) * 4;
		m_Pixels[offset] = color.r;
		m_Pixels[offset + 1] = color.g;
		m_Pixels[offset + 2] = color.b;
		m_Pixels[offset + 3] = color.a;
	}

	bool Image::SavePNG(const std::filesystem::path& path) const
	{
		if (IsEmpty())
		{
			PX_CORE_ERROR("Cannot save an empty image to '{}'", path.string());
			return false;
		}

		std::error_code error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), error);

		const std::string utf8Path = reinterpret_cast<const char*>(path.u8string().c_str());
		const int stride = static_cast<int>(m_Width * 4);
		if (!stbi_write_png(utf8Path.c_str(), static_cast<int>(m_Width), static_cast<int>(m_Height), 4, m_Pixels.data(), stride))
		{
			PX_CORE_ERROR("Failed to write PNG '{}'", utf8Path);
			return false;
		}
		return true;
	}

	Image Image::LoadFromFile(const std::filesystem::path& path)
	{
		const std::string utf8Path = reinterpret_cast<const char*>(path.u8string().c_str());
		int width = 0, height = 0, channels = 0;
		stbi_uc* data = stbi_load(utf8Path.c_str(), &width, &height, &channels, 4);
		if (!data)
		{
			PX_CORE_ERROR("Failed to load image '{}': {}", utf8Path, stbi_failure_reason());
			return {};
		}

		Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		std::copy(data, data + image.m_Pixels.size(), image.m_Pixels.begin());
		stbi_image_free(data);
		return image;
	}

	uint32_t Image::MaxChannelDifference(const Image& a, const Image& b)
	{
		if (a.m_Width != b.m_Width || a.m_Height != b.m_Height)
			return 256;

		uint32_t maxDifference = 0;
		for (size_t i = 0; i < a.m_Pixels.size(); i++)
			maxDifference = std::max(maxDifference, static_cast<uint32_t>(std::abs(int(a.m_Pixels[i]) - int(b.m_Pixels[i]))));
		return maxDifference;
	}

}
