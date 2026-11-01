#include "pxpch.h"
#include "Paralax/Core/InputNames.h"

namespace Paralax {

	namespace {

		constexpr InputName<KeyCode> s_KeyNames[] = {{"Space", Key::Space}, {"Apostrophe", Key::Apostrophe}, {"Comma", Key::Comma}, {"Minus", Key::Minus}, {"Period", Key::Period}, {"Slash", Key::Slash}, {"D0", Key::D0}, {"D1", Key::D1}, {"D2", Key::D2}, {"D3", Key::D3}, {"D4", Key::D4}, {"D5", Key::D5}, {"D6", Key::D6}, {"D7", Key::D7}, {"D8", Key::D8}, {"D9", Key::D9}, {"Semicolon", Key::Semicolon}, {"Equal", Key::Equal}, {"A", Key::A}, {"B", Key::B}, {"C", Key::C}, {"D", Key::D}, {"E", Key::E}, {"F", Key::F}, {"G", Key::G}, {"H", Key::H}, {"I", Key::I}, {"J", Key::J}, {"K", Key::K}, {"L", Key::L}, {"M", Key::M}, {"N", Key::N}, {"O", Key::O}, {"P", Key::P}, {"Q", Key::Q}, {"R", Key::R}, {"S", Key::S}, {"T", Key::T}, {"U", Key::U}, {"V", Key::V}, {"W", Key::W}, {"X", Key::X}, {"Y", Key::Y}, {"Z", Key::Z}, {"LeftBracket", Key::LeftBracket}, {"Backslash", Key::Backslash}, {"RightBracket", Key::RightBracket}, {"GraveAccent", Key::GraveAccent}, {"Escape", Key::Escape}, {"Enter", Key::Enter}, {"Tab", Key::Tab}, {"Backspace", Key::Backspace}, {"Insert", Key::Insert}, {"Delete", Key::Delete}, {"Right", Key::Right}, {"Left", Key::Left}, {"Down", Key::Down}, {"Up", Key::Up}, {"PageUp", Key::PageUp}, {"PageDown", Key::PageDown}, {"Home", Key::Home}, {"End", Key::End}, {"CapsLock", Key::CapsLock}, {"F1", Key::F1}, {"F2", Key::F2}, {"F3", Key::F3}, {"F4", Key::F4}, {"F5", Key::F5}, {"F6", Key::F6}, {"F7", Key::F7}, {"F8", Key::F8}, {"F9", Key::F9}, {"F10", Key::F10}, {"F11", Key::F11}, {"F12", Key::F12}, {"KP0", Key::KP0}, {"KP1", Key::KP1}, {"KP2", Key::KP2}, {"KP3", Key::KP3}, {"KP4", Key::KP4}, {"KP5", Key::KP5}, {"KP6", Key::KP6}, {"KP7", Key::KP7}, {"KP8", Key::KP8}, {"KP9", Key::KP9}, {"KPEnter", Key::KPEnter}, {"LeftShift", Key::LeftShift}, {"LeftControl", Key::LeftControl}, {"LeftAlt", Key::LeftAlt}, {"LeftSuper", Key::LeftSuper}, {"RightShift", Key::RightShift}, {"RightControl", Key::RightControl}, {"RightAlt", Key::RightAlt}, {"RightSuper", Key::RightSuper}};

		constexpr InputName<MouseCode> s_MouseButtonNames[] = {{"Left", Mouse::ButtonLeft}, {"Right", Mouse::ButtonRight}, {"Middle", Mouse::ButtonMiddle}};

		template<typename CodeType>
		std::optional<CodeType> Find(std::span<const InputName<CodeType>> names, std::string_view name)
		{
			for (const InputName<CodeType>& entry : names)
			{
				if (name == entry.Name)
					return entry.Code;
			}
			return std::nullopt;
		}

	}

	std::span<const InputName<KeyCode>> GetKeyNames()
	{
		return s_KeyNames;
	}

	std::span<const InputName<MouseCode>> GetMouseButtonNames()
	{
		return s_MouseButtonNames;
	}

	std::optional<KeyCode> KeyFromName(std::string_view name)
	{
		return Find(GetKeyNames(), name);
	}

	std::optional<MouseCode> MouseButtonFromName(std::string_view name)
	{
		return Find(GetMouseButtonNames(), name);
	}

}
