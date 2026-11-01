#pragma once

#include "Paralax/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Paralax {

	enum class TextAlignment : int; // Scene/Components.h

	// One glyph's placement: a quad in layout units (line height 1; x to the right and y down from the top of the
	// first line) and its rectangle in the atlas (UV, top-left origin).
	struct GlyphQuad
	{
		glm::vec2 Min{0.0f};
		glm::vec2 Max{0.0f};
		glm::vec2 UVMin{0.0f};
		glm::vec2 UVMax{0.0f};
	};

	struct TextLayout
	{
		std::vector<GlyphQuad> Quads;
		glm::vec2 Size{0.0f}; // Widest line, and the number of lines (layout units).
		uint32_t Lines = 0;
		uint32_t Replacements = 0; // Characters drawn as the replacement box.
	};

	// A signed-distance-field glyph atlas built from the engine's embedded font, and text layout on it. It covers ASCII
	// and Latin-1 (U+0020 to U+007E and U+00A0 to U+00FF); any other character is drawn as a replacement box, never
	// dropped silently. Building is deterministic for a given build (stb_truetype's distance fields use floating-point
	// math, so pixels may differ in the last bit between compilers or CPU architectures). Main-thread-only.
	class FontAtlas
	{
	public:
		static constexpr uint32_t Width = 1024;
		static constexpr uint32_t Height = 1024;
		// Glyphs are rasterised at this height; the distance field reaches Padding pixels out from every edge.
		static constexpr float GlyphPixelHeight = 48.0f;
		static constexpr int Padding = 6;
		// Text beyond this many bytes is cut before layout.
		static constexpr size_t MaxTextBytes = 4096;

		// The engine's text font (Inter SemiBold, embedded). Returns nullptr (with an error logged) if the font is
		// missing from the build or its glyphs do not fit.
		static Ref<FontAtlas> CreateDefault();

		// Width x Height bytes, rows top to bottom: 128 on a glyph's edge, more inside.
		const std::vector<uint8_t>& GetPixels() const { return m_Pixels; }
		// Distance from a line's top to its baseline, in layout units.
		float GetAscent() const { return m_Ascent; }

		// Lays out UTF-8 text. A newline starts a new line and a tab advances four spaces; other control characters
		// are skipped; invalid UTF-8 bytes and characters outside the atlas become replacement boxes. Lines are
		// aligned within the widest one.
		TextLayout Layout(std::string_view text, TextAlignment alignment) const;

	private:
		struct Glyph
		{
			glm::vec2 UVMin{0.0f};
			glm::vec2 UVMax{0.0f};
			glm::vec2 Offset{0.0f}; // Top-left of the quad from the pen, in layout units (y down).
			glm::vec2 Size{0.0f};   // Layout units; zero for glyphs without a shape (space).
			float Advance = 0.0f;   // Layout units.
		};

		FontAtlas() = default;
		// stb_truetype does not range-check font data, so only the embedded (trusted) font is ever passed here.
		static Ref<FontAtlas> Create(std::span<const uint8_t> ttf);
		const Glyph& Find(uint32_t codepoint) const;

		std::vector<uint8_t> m_Pixels;
		std::vector<Glyph> m_Glyphs;         // [0] is the replacement box.
		std::array<uint16_t, 256> m_Slots{}; // Codepoints below 256 to glyph indices (0: replacement).
		std::vector<float> m_Kerning;        // 256 x 256, layout units, for pairs of codepoints below 256.
		float m_Ascent = 0.0f;
	};

}
