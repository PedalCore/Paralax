#pragma once

#include "Paralax/Core/KeyCodes.h"
#include "Paralax/Core/MouseCodes.h"

#include <optional>
#include <span>
#include <string_view>

namespace Paralax {

	// The names of keys and mouse buttons, shared by scripts (Key.Space, Mouse.Left) and agent commands
	// (input.key { key = "Space" }), so both speak the same vocabulary. Immutable; safe on any thread.
	template<typename CodeType>
	struct InputName
	{
		const char* Name;
		CodeType Code;
	};

	std::span<const InputName<KeyCode>> GetKeyNames();
	std::span<const InputName<MouseCode>> GetMouseButtonNames();

	// Case-sensitive lookups; nullopt for unknown names.
	std::optional<KeyCode> KeyFromName(std::string_view name);
	std::optional<MouseCode> MouseButtonFromName(std::string_view name);

}
