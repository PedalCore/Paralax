#include "pxpch.h"
#include "Paralax/Core/JsonText.h"

#include "Paralax/Core/Assert.h"
#include "Paralax/Core/PathEncoding.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace Paralax {

	namespace {

		using json = nlohmann::json;

		constexpr std::string_view Ellipsis = "...";
		constexpr std::string_view ReplacementCharacter = "\xEF\xBF\xBD"; // U+FFFD

		// Appends token whole, or returns false and leaves out unchanged if it would grow past limit.
		bool AppendToken(std::string& out, std::string_view token, size_t limit)
		{
			if (out.size() + token.size() > limit)
				return false;
			out += token;
			return true;
		}

		// Characters that would let quoted text change how a log line reads rather than what it says: control
		// characters (C0, DEL and C1, where a terminal may read U+009B as the start of an escape sequence), the line
		// and paragraph separators, which could break a line or fake the start of another, and the invisible format
		// characters (zero-width spaces and joiners, the bidirectional marks, embeddings, overrides and isolates, and
		// the byte order mark), with which a name such as U+202E + "...txt" would display reversed.
		bool NeedsUnicodeEscape(char32_t code)
		{
			return code < U' ' || (code >= U'\x7F' && code <= U'\x9F') ||         // C0, DEL, C1
				   (code >= U'\x200B' && code <= U'\x200F') ||                    // ZWSP, ZWNJ, ZWJ, LRM, RLM
				   code == U'\x2028' || code == U'\x2029' ||                      // Line and paragraph separators
				   (code >= U'\x202A' && code <= U'\x202E') ||                    // LRE, RLE, PDF, LRO, RLO
				   (code >= U'\x2060' && code <= U'\x2069') || code == U'\xFEFF'; // Word joiner to PDI; BOM
		}

		// Appends text escaped as inside a JSON string, one character at a time, until the next character would grow
		// out past limit. Returns false if it stopped before the end of the text.
		bool AppendEscaped(std::string& out, std::string_view text, size_t limit)
		{
			char escape[6];
			for (size_t index = 0; index < text.size();)
			{
				const size_t start = index;
				const std::optional<char32_t> code = DecodeUtf8(text, index);
				std::string_view character = text.substr(start, index - start);
				if (!code)
					character = ReplacementCharacter;
				else if (*code == U'"')
					character = "\\\"";
				else if (*code == U'\\')
					character = "\\\\";
				else if (*code == U'\b')
					character = "\\b";
				else if (*code == U'\f')
					character = "\\f";
				else if (*code == U'\n')
					character = "\\n";
				else if (*code == U'\r')
					character = "\\r";
				else if (*code == U'\t')
					character = "\\t";
				else if (NeedsUnicodeEscape(*code))
				{
					// Every such character is below U+10000, so four hex digits hold it.
					constexpr char Hex[] = "0123456789abcdef";
					escape[0] = '\\';
					escape[1] = 'u';
					escape[2] = Hex[(*code >> 12) & 0xF];
					escape[3] = Hex[(*code >> 8) & 0xF];
					escape[4] = Hex[(*code >> 4) & 0xF];
					escape[5] = Hex[*code & 0xF];
					character = std::string_view(escape, sizeof(escape));
				}
				if (!AppendToken(out, character, limit))
					return false;
			}
			return true;
		}

		std::string Count(size_t count, std::string_view singular, std::string_view plural)
		{
			return fmt::format("{} {}", count, count == 1 ? singular : plural);
		}

		bool AppendNumber(std::string& out, const json& value, size_t limit)
		{
			if (value.is_number_float())
			{
				const double number = value.get<double>();
				// dump() would write these as null, which reads as a different mistake.
				if (std::isnan(number))
					return AppendToken(out, "NaN", limit);
				if (std::isinf(number))
					return AppendToken(out, number > 0.0 ? "Infinity" : "-Infinity", limit);
			}
			// A number's dump() never recurses and never meets a string, so it cannot throw.
			return AppendToken(out, value.dump(), limit);
		}

		// Appends value as compact JSON until out would grow past limit; returns false if it stopped short. The walk
		// keeps its own stack instead of recursing, and every level it enters appends a bracket first, so the stack
		// never grows deeper than limit.
		bool AppendJsonPreview(std::string& out, const json& root, size_t limit)
		{
			struct Level
			{
				const json* Container;
				json::const_iterator Next;
				bool First = true;
			};
			std::vector<Level> levels;

			// Appends a scalar whole, or opens a container (pushing a level for one that is not empty).
			auto enter = [&](const json& value) {
				switch (value.type())
				{
					case json::value_t::null: return AppendToken(out, "null", limit);
					case json::value_t::boolean: return AppendToken(out, value.get<bool>() ? "true" : "false", limit);
					case json::value_t::number_integer:
					case json::value_t::number_unsigned:
					case json::value_t::number_float: return AppendNumber(out, value, limit);
					case json::value_t::string:
						return AppendToken(out, "\"", limit) && AppendEscaped(out, value.get_ref<const std::string&>(), limit) && AppendToken(out, "\"", limit);
					case json::value_t::binary: return AppendToken(out, fmt::format("<binary {}>", Count(value.get_binary().size(), "byte", "bytes")), limit);
					case json::value_t::discarded: return AppendToken(out, "<discarded>", limit);
					case json::value_t::array:
					case json::value_t::object:
					{
						const bool isArray = value.is_array();
						if (value.empty())
							return AppendToken(out, isArray ? "[]" : "{}", limit);
						if (!AppendToken(out, isArray ? "[" : "{", limit))
							return false;
						levels.push_back({&value, value.cbegin()});
						return true;
					}
				}
				return AppendToken(out, "?", limit);
			};

			if (!enter(root))
				return false;
			while (!levels.empty())
			{
				Level& level = levels.back();
				const bool isArray = level.Container->is_array();
				if (level.Next == level.Container->cend())
				{
					if (!AppendToken(out, isArray ? "]" : "}", limit))
						return false;
					levels.pop_back();
					continue;
				}
				if (!level.First && !AppendToken(out, ",", limit))
					return false;
				level.First = false;
				const json::const_iterator element = level.Next++;
				if (!isArray && !(AppendToken(out, "\"", limit) && AppendEscaped(out, element.key(), limit) && AppendToken(out, "\":", limit)))
					return false;
				// May push a level, which invalidates the reference to this one; it is not used after this.
				if (!enter(element.value()))
					return false;
			}
			return true;
		}

	}

	std::string PreviewText(std::string_view text, bool* outCut)
	{
		std::string preview;
		const bool complete = AppendEscaped(preview, text, MaxPreviewBytes);
		if (!complete)
			preview += Ellipsis;
		if (outCut)
			*outCut = !complete;
		return preview;
	}

	std::string QuoteText(std::string_view text)
	{
		bool cut = false;
		const std::string preview = PreviewText(text, &cut);
		if (!cut)
			return fmt::format("'{}'", preview);
		return fmt::format("'{}' ({})", preview, Count(text.size(), "byte", "bytes"));
	}

	std::string CapMessage(std::string_view text, size_t maxBytes, size_t fullBytes)
	{
		fullBytes = std::max(fullBytes, text.size());
		if (fullBytes <= maxBytes)
			return std::string(text);
		// Back up over continuation bytes to the start of the character the limit falls in, so it is left out whole.
		// A character has at most three of them; a longer run is not UTF-8, and cutting inside it splits nothing.
		size_t end = std::min(maxBytes, text.size());
		for (int step = 0; step < 3 && end > 0 && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80; step++)
			end--;
		return fmt::format("{}{} ({})", text.substr(0, end), Ellipsis, Count(fullBytes, "byte", "bytes"));
	}

	std::string DescribeJsonValue(const nlohmann::json& value)
	{
		if (value.is_null())
			return "null";
		if (value.is_binary())
			return fmt::format("binary data ({})", Count(value.get_binary().size(), "byte", "bytes"));
		if (value.is_discarded())
			return "a discarded value";

		std::string preview;
		if (AppendJsonPreview(preview, value, MaxPreviewBytes))
			return fmt::format("{} ({})", preview, value.type_name());

		// Only strings and containers are ever cut: no number or boolean is longer than the limit.
		std::string size;
		if (value.is_string())
			size = Count(value.get_ref<const std::string&>().size(), "byte", "bytes");
		else if (value.is_array())
			size = Count(value.size(), "element", "elements");
		else
			size = Count(value.size(), "member", "members");
		return fmt::format("{}{} ({} of {})", preview, Ellipsis, value.type_name(), size);
	}

	std::string FormatJsonDocument(const nlohmann::json& document)
	{
		return document.dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
	}

	bool JsonExceedsDepth(std::string_view text, int maxDepth)
	{
		PX_CORE_ASSERT(maxDepth >= 0, "The JSON depth limit cannot be negative");
		int depth = 0;
		bool inString = false;
		for (size_t i = 0; i < text.size(); i++)
		{
			const char c = text[i];
			if (inString)
			{
				// Whatever follows a backslash is part of the string, so it can neither end it nor nest.
				if (c == '\\')
					i++;
				else if (c == '"')
					inString = false;
			}
			else if (c == '"')
			{
				inString = true;
			}
			else if (c == '[' || c == '{')
			{
				if (++depth > maxDepth)
					return true;
			}
			else if ((c == ']' || c == '}') && depth > 0)
			{
				depth--;
			}
		}
		return false;
	}

	std::optional<int> ReadDocumentVersion(const nlohmann::json& header, int currentVersion, std::string& outDescription)
	{
		PX_CORE_ASSERT(currentVersion >= 1, "A reader supports at least version 1");
		// Where the assert is compiled out, a range that is empty stays empty, rather than converting to a huge one.
		const uint64_t newest = currentVersion > 0 ? static_cast<uint64_t>(currentVersion) : 0;
		outDescription.clear();
		if (!header.is_object() || !header.contains("Version"))
		{
			outDescription = "none";
			return std::nullopt;
		}
		const nlohmann::json& value = header["Version"];
		// Parsed non-negative integers are unsigned and negative ones signed (a document built in memory may hold
		// either); get<int>() would truncate both, reading 4294967297 as 1.
		if (value.is_number_unsigned())
		{
			const uint64_t version = value.get<uint64_t>();
			if (version >= 1 && version <= newest)
				return static_cast<int>(version);
		}
		else if (value.is_number_integer())
		{
			const int64_t version = value.get<int64_t>();
			if (version >= 1 && static_cast<uint64_t>(version) <= newest)
				return static_cast<int>(version);
		}
		// At most 20 digits and a sign, an integer is always shown in full; anything else is bounded.
		outDescription = DescribeJsonValue(value);
		return std::nullopt;
	}

}
