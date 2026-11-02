#include "pxpch.h"
#include "Paralax/Core/PathEncoding.h"

#include <algorithm>
#include <optional>

namespace Paralax {

	namespace {

		constexpr char32_t ReplacementCharacter = 0xFFFD;

		void AppendUtf8(std::string& out, char32_t code)
		{
			if (code < 0x80)
			{
				out += static_cast<char>(code);
			}
			else if (code < 0x800)
			{
				out += static_cast<char>(0xC0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
			else if (code < 0x10000)
			{
				out += static_cast<char>(0xE0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | (code >> 18));
				out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
		}

		// Decodes the sequence starting at index (which must be inside text) and advances index past it. A malformed
		// sequence yields nullopt, and index stops at the first byte that does not belong to it (Unicode's "maximal
		// subpart" practice), so a valid character that follows is kept. The table is Unicode's well-formed byte
		// sequences (Table 3-7): no overlong forms, no encoded surrogates, nothing beyond U+10FFFF.
		std::optional<char32_t> DecodeUtf8(std::string_view text, size_t& index)
		{
			const auto lead = static_cast<uint8_t>(text[index]);
			index++;
			if (lead < 0x80)
				return lead;

			size_t continuations = 0;
			char32_t code = 0;
			uint8_t lower = 0x80, upper = 0xBF; // The range the next byte must fall in.
			if (lead >= 0xC2 && lead <= 0xDF)
			{
				continuations = 1;
				code = lead & 0x1Fu;
			}
			else if (lead >= 0xE0 && lead <= 0xEF)
			{
				continuations = 2;
				code = lead & 0x0Fu;
				if (lead == 0xE0)
					lower = 0xA0;
				else if (lead == 0xED)
					upper = 0x9F;
			}
			else if (lead >= 0xF0 && lead <= 0xF4)
			{
				continuations = 3;
				code = lead & 0x07u;
				if (lead == 0xF0)
					lower = 0x90;
				else if (lead == 0xF4)
					upper = 0x8F;
			}
			else
			{
				return std::nullopt;
			}

			for (size_t i = 0; i < continuations; i++)
			{
				if (index >= text.size())
					return std::nullopt;
				const auto byte = static_cast<uint8_t>(text[index]);
				if (byte < lower || byte > upper)
					return std::nullopt;
				code = (code << 6) | (byte & 0x3Fu);
				lower = 0x80;
				upper = 0xBF;
				index++;
			}
			return code;
		}

		template<typename Char>
		std::string NativeToUtf8(std::basic_string_view<Char> native)
		{
			if constexpr (sizeof(Char) == 1)
			{
				return std::string(native.begin(), native.end());
			}
			else
			{
				static_assert(sizeof(Char) == sizeof(char16_t), "Native paths are bytes (POSIX) or UTF-16 (Windows)");
				std::u16string units(native.size(), u'\0');
				std::transform(native.begin(), native.end(), units.begin(), [](Char unit) { return static_cast<char16_t>(unit); });
				return Utf16ToUtf8(units);
			}
		}

		template<typename Char>
		std::basic_string<Char> Utf8ToNative(std::string_view text)
		{
			if constexpr (sizeof(Char) == 1)
			{
				return std::basic_string<Char>(text.begin(), text.end());
			}
			else
			{
				static_assert(sizeof(Char) == sizeof(char16_t), "Native paths are bytes (POSIX) or UTF-16 (Windows)");
				const std::u16string units = Utf8ToUtf16(text);
				std::basic_string<Char> native(units.size(), Char());
				std::transform(units.begin(), units.end(), native.begin(), [](char16_t unit) { return static_cast<Char>(unit); });
				return native;
			}
		}

	}

	std::string Utf16ToUtf8(std::u16string_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (size_t i = 0; i < text.size(); i++)
		{
			char32_t code = text[i];
			if (code >= 0xD800 && code <= 0xDBFF)
			{
				const char32_t low = i + 1 < text.size() ? text[i + 1] : u'\0';
				if (low >= 0xDC00 && low <= 0xDFFF)
				{
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
					i++;
				}
				else
				{
					code = ReplacementCharacter;
				}
			}
			else if (code >= 0xDC00 && code <= 0xDFFF)
			{
				code = ReplacementCharacter;
			}
			AppendUtf8(out, code);
		}
		return out;
	}

	std::u16string Utf8ToUtf16(std::string_view text)
	{
		std::u16string out;
		out.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			const char32_t code = DecodeUtf8(text, i).value_or(ReplacementCharacter);
			if (code >= 0x10000)
			{
				out += static_cast<char16_t>(0xD800 + ((code - 0x10000) >> 10));
				out += static_cast<char16_t>(0xDC00 + ((code - 0x10000) & 0x3FF));
			}
			else
			{
				out += static_cast<char16_t>(code);
			}
		}
		return out;
	}

	bool IsWellFormedUtf8(std::string_view text)
	{
		for (size_t i = 0; i < text.size();)
		{
			if (!DecodeUtf8(text, i))
				return false;
		}
		return true;
	}

	std::string ToUtf8(const std::filesystem::path& path)
	{
		using Path = std::filesystem::path;
		std::string text = NativeToUtf8(std::basic_string_view<Path::value_type>(path.native()));
		// Generic form: the platform's preferred separator becomes '/', as path::generic_u8string() does.
		if constexpr (Path::preferred_separator != static_cast<Path::value_type>('/'))
			std::replace(text.begin(), text.end(), static_cast<char>(Path::preferred_separator), '/');
		return text;
	}

	std::filesystem::path FromUtf8(std::string_view text)
	{
		return std::filesystem::path(Utf8ToNative<std::filesystem::path::value_type>(text));
	}

}
