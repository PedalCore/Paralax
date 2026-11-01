#include "pxpch.h"
#include "Paralax/Renderer/FontAtlas.h"

#include "Paralax/Core/EmbeddedFile.h"
#include "Paralax/Scene/Components.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Paralax::EmbeddedFonts {

	extern const EmbeddedFile* const g_Entries;
	extern const size_t g_EntryCount;

}

namespace Paralax {

	namespace {

		constexpr uint32_t s_Replacement = 0xFFFD;
		constexpr uint32_t s_Gutter = 1; // Pixels between packed glyphs, so bilinear sampling never bleeds.

		// The codepoints the atlas holds, in order: ASCII, then Latin-1.
		std::vector<uint32_t> AtlasCodepoints()
		{
			std::vector<uint32_t> codepoints;
			for (uint32_t c = 0x20; c <= 0x7E; c++)
				codepoints.push_back(c);
			for (uint32_t c = 0xA0; c <= 0xFF; c++)
				codepoints.push_back(c);
			return codepoints;
		}

		// A rasterised glyph awaiting packing.
		struct Bitmap
		{
			uint32_t Codepoint = 0;
			std::vector<uint8_t> Pixels;
			int Width = 0;
			int Height = 0;
			int OffsetX = 0; // Top-left of the bitmap from the pen, pixels (y down).
			int OffsetY = 0;
			float Advance = 0.0f; // Pixels.
			uint32_t X = 0;       // Packed position.
			uint32_t Y = 0;
		};

		// The replacement box: a hollow rectangle as a distance field, so it anti-aliases like the font's glyphs.
		Bitmap MakeReplacementBox(float ascentPixels)
		{
			const float boxWidth = std::round(FontAtlas::GlyphPixelHeight * 0.5f);
			const float boxHeight = std::round(ascentPixels * 0.85f);
			const float thickness = 3.0f;
			Bitmap box;
			box.Codepoint = s_Replacement;
			box.Width = static_cast<int>(boxWidth) + 2 * FontAtlas::Padding;
			box.Height = static_cast<int>(boxHeight) + 2 * FontAtlas::Padding;
			box.OffsetX = 0;
			box.OffsetY = -static_cast<int>(boxHeight) - FontAtlas::Padding;
			box.Advance = boxWidth + 2.0f * FontAtlas::Padding;
			box.Pixels.resize(static_cast<size_t>(box.Width) * static_cast<size_t>(box.Height));
			const glm::vec2 half(boxWidth * 0.5f, boxHeight * 0.5f);
			const glm::vec2 centre(FontAtlas::Padding + half.x, FontAtlas::Padding + half.y);
			const float scale = 128.0f / FontAtlas::Padding; // Atlas units per pixel, as stb's distance fields use.
			for (int y = 0; y < box.Height; y++)
			{
				for (int x = 0; x < box.Width; x++)
				{
					// Signed distance to the rectangle's outline, then to the ring of the given thickness.
					const glm::vec2 p = glm::abs(glm::vec2(x + 0.5f, y + 0.5f) - centre) - half;
					const float outside = glm::length(glm::max(p, glm::vec2(0.0f))) + std::min(std::max(p.x, p.y), 0.0f);
					const float ring = std::abs(outside) - thickness * 0.5f;
					const float value = std::clamp(128.0f - ring * scale, 0.0f, 255.0f);
					box.Pixels[static_cast<size_t>(y) * box.Width + x] = static_cast<uint8_t>(std::lround(value));
				}
			}
			return box;
		}

		// Decodes one UTF-8 sequence at offset. Invalid or truncated sequences decode as the replacement character,
		// consuming one byte, so every byte of hostile input is accounted for.
		uint32_t DecodeUTF8(std::string_view text, size_t& offset)
		{
			const auto byte = [&](size_t i) { return static_cast<uint8_t>(text[i]); };
			const uint8_t lead = byte(offset);
			size_t length = 0;
			uint32_t codepoint = 0;
			if (lead < 0x80)
			{
				offset++;
				return lead;
			}
			if ((lead & 0xE0) == 0xC0)
			{
				length = 2;
				codepoint = lead & 0x1F;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				length = 3;
				codepoint = lead & 0x0F;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				length = 4;
				codepoint = lead & 0x07;
			}
			else
			{
				offset++;
				return s_Replacement;
			}
			if (offset + length > text.size())
			{
				offset++;
				return s_Replacement;
			}
			for (size_t i = 1; i < length; i++)
			{
				const uint8_t continuation = byte(offset + i);
				if ((continuation & 0xC0) != 0x80)
				{
					offset++;
					return s_Replacement;
				}
				codepoint = (codepoint << 6) | (continuation & 0x3F);
			}
			// Overlong encodings, surrogates and values beyond Unicode are invalid.
			const uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800
																	  : 0x10000;
			if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
			{
				offset++;
				return s_Replacement;
			}
			offset += length;
			return codepoint;
		}

	}

	Ref<FontAtlas> FontAtlas::Create(std::span<const uint8_t> ttf)
	{
		stbtt_fontinfo info;
		if (ttf.empty() || ttf.size() > static_cast<size_t>(std::numeric_limits<int>::max()) || !stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0)))
		{
			PX_CORE_ERROR("FontAtlas: the font data cannot be read");
			return nullptr;
		}
		const float scale = stbtt_ScaleForPixelHeight(&info, GlyphPixelHeight);
		int ascent = 0, descent = 0, lineGap = 0;
		stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
		const float lineHeightPixels = static_cast<float>(ascent - descent + lineGap) * scale;
		if (!(lineHeightPixels > 0.0f))
		{
			PX_CORE_ERROR("FontAtlas: the font has no usable line height");
			return nullptr;
		}
		const float ascentPixels = static_cast<float>(ascent) * scale;

		// Rasterise every glyph as a distance field: 128 on the edge, Padding pixels of range either side.
		std::vector<Bitmap> bitmaps;
		bitmaps.push_back(MakeReplacementBox(ascentPixels));
		for (uint32_t codepoint : AtlasCodepoints())
		{
			Bitmap bitmap;
			bitmap.Codepoint = codepoint;
			int advance = 0, bearing = 0;
			stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance, &bearing);
			bitmap.Advance = static_cast<float>(advance) * scale;
			if (unsigned char* pixels = stbtt_GetCodepointSDF(&info, scale, static_cast<int>(codepoint), Padding, 128, 128.0f / Padding, &bitmap.Width, &bitmap.Height, &bitmap.OffsetX, &bitmap.OffsetY))
			{
				bitmap.Pixels.assign(pixels, pixels + static_cast<size_t>(bitmap.Width) * static_cast<size_t>(bitmap.Height));
				stbtt_FreeSDF(pixels, nullptr);
			}
			else
			{
				bitmap.Width = bitmap.Height = 0; // No shape (a space).
			}
			bitmaps.push_back(std::move(bitmap));
		}

		// Shelf packing, tallest first (ties by codepoint, so the layout is deterministic).
		std::vector<size_t> order(bitmaps.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			return bitmaps[a].Height != bitmaps[b].Height ? bitmaps[a].Height > bitmaps[b].Height : bitmaps[a].Codepoint < bitmaps[b].Codepoint;
		});
		uint32_t shelfX = 0, shelfY = 0, shelfHeight = 0;
		for (size_t index : order)
		{
			Bitmap& bitmap = bitmaps[index];
			if (bitmap.Width == 0)
				continue;
			const uint32_t width = static_cast<uint32_t>(bitmap.Width) + s_Gutter;
			const uint32_t height = static_cast<uint32_t>(bitmap.Height) + s_Gutter;
			if (shelfX + width > Width)
			{
				shelfY += shelfHeight;
				shelfX = 0;
				shelfHeight = 0;
			}
			if (shelfY + height > Height || width > Width)
			{
				PX_CORE_ERROR("FontAtlas: the glyphs do not fit a {}x{} atlas", Width, Height);
				return nullptr;
			}
			bitmap.X = shelfX;
			bitmap.Y = shelfY;
			shelfX += width;
			shelfHeight = std::max(shelfHeight, height);
		}

		auto atlas = Ref<FontAtlas>(new FontAtlas());
		atlas->m_Pixels.assign(static_cast<size_t>(Width) * Height, 0);
		atlas->m_Glyphs.reserve(bitmaps.size());
		atlas->m_Ascent = ascentPixels / lineHeightPixels;
		for (const Bitmap& bitmap : bitmaps)
		{
			for (int y = 0; y < bitmap.Height; y++)
				std::copy_n(bitmap.Pixels.data() + static_cast<size_t>(y) * bitmap.Width, bitmap.Width, atlas->m_Pixels.data() + static_cast<size_t>(bitmap.Y + y) * Width + bitmap.X);
			Glyph glyph;
			glyph.UVMin = glm::vec2(static_cast<float>(bitmap.X) / Width, static_cast<float>(bitmap.Y) / Height);
			glyph.UVMax = glm::vec2(static_cast<float>(bitmap.X + bitmap.Width) / Width, static_cast<float>(bitmap.Y + bitmap.Height) / Height);
			glyph.Offset = glm::vec2(bitmap.OffsetX, bitmap.OffsetY) / lineHeightPixels;
			glyph.Size = glm::vec2(bitmap.Width, bitmap.Height) / lineHeightPixels;
			glyph.Advance = bitmap.Advance / lineHeightPixels;
			if (bitmap.Codepoint < 256)
				atlas->m_Slots[bitmap.Codepoint] = static_cast<uint16_t>(atlas->m_Glyphs.size());
			atlas->m_Glyphs.push_back(glyph);
		}

		// Kerning for every pair the atlas can draw, looked up once here rather than per character per frame.
		atlas->m_Kerning.assign(256 * 256, 0.0f);
		const std::vector<uint32_t> codepoints = AtlasCodepoints();
		for (uint32_t first : codepoints)
			for (uint32_t second : codepoints)
			{
				const int kern = stbtt_GetCodepointKernAdvance(&info, static_cast<int>(first), static_cast<int>(second));
				if (kern != 0)
					atlas->m_Kerning[first * 256 + second] = static_cast<float>(kern) * scale / lineHeightPixels;
			}
		return atlas;
	}

	Ref<FontAtlas> FontAtlas::CreateDefault()
	{
		const std::span<const uint8_t> data = FindEmbeddedFile(EmbeddedFonts::g_Entries, EmbeddedFonts::g_EntryCount, "Inter-SemiBold");
		if (data.empty())
		{
			PX_CORE_ERROR("FontAtlas: the embedded font 'Inter-SemiBold' is missing");
			return nullptr;
		}
		return Create(data);
	}

	const FontAtlas::Glyph& FontAtlas::Find(uint32_t codepoint) const
	{
		return m_Glyphs[codepoint < 256 ? m_Slots[codepoint] : 0];
	}

	TextLayout FontAtlas::Layout(std::string_view text, TextAlignment alignment) const
	{
		TextLayout layout;
		if (text.size() > MaxTextBytes)
			text = text.substr(0, MaxTextBytes);

		struct Line
		{
			size_t FirstQuad = 0;
			float Width = 0.0f;
		};
		std::vector<Line> lines{Line{}};
		const float spaceAdvance = Find(' ').Advance;
		float penX = 0.0f;
		uint32_t previous = 0;
		size_t offset = 0;
		while (offset < text.size())
		{
			const uint32_t codepoint = DecodeUTF8(text, offset);
			if (codepoint == '\n')
			{
				lines.back().Width = penX;
				lines.push_back(Line{layout.Quads.size(), 0.0f});
				penX = 0.0f;
				previous = 0;
				continue;
			}
			if (codepoint == '\t')
			{
				penX += 4.0f * spaceAdvance;
				previous = 0;
				continue;
			}
			if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0))
				continue; // Other control characters have no appearance.

			const bool known = codepoint < 256 && m_Slots[codepoint] != 0;
			const Glyph& glyph = known ? m_Glyphs[m_Slots[codepoint]] : m_Glyphs[0];
			if (!known)
				layout.Replacements++;
			if (known && previous != 0)
				penX += m_Kerning[previous * 256 + codepoint];
			if (glyph.Size.x > 0.0f)
			{
				GlyphQuad quad;
				quad.Min = glm::vec2(penX, static_cast<float>(lines.size() - 1) + m_Ascent) + glyph.Offset;
				quad.Max = quad.Min + glyph.Size;
				quad.UVMin = glyph.UVMin;
				quad.UVMax = glyph.UVMax;
				layout.Quads.push_back(quad);
			}
			penX += glyph.Advance;
			previous = known ? codepoint : 0;
		}
		lines.back().Width = penX;

		float widest = 0.0f;
		for (const Line& line : lines)
			widest = std::max(widest, line.Width);
		for (size_t i = 0; i < lines.size(); i++)
		{
			const float shift = alignment == TextAlignment::Center ? (widest - lines[i].Width) * 0.5f : alignment == TextAlignment::Right ? widest - lines[i].Width
																																		  : 0.0f;
			const size_t end = i + 1 < lines.size() ? lines[i + 1].FirstQuad : layout.Quads.size();
			for (size_t q = lines[i].FirstQuad; q < end; q++)
			{
				layout.Quads[q].Min.x += shift;
				layout.Quads[q].Max.x += shift;
			}
		}
		layout.Lines = static_cast<uint32_t>(lines.size());
		layout.Size = glm::vec2(widest, static_cast<float>(lines.size()));
		return layout;
	}

}
