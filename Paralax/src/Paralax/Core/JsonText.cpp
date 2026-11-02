#include "pxpch.h"
#include "Paralax/Core/JsonText.h"

#include "Paralax/Core/Assert.h"

#include <nlohmann/json.hpp>

namespace Paralax {

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

}
