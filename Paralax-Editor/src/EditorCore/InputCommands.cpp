#include "EditorCore/EditorContext.h"

#include <Paralax/Core/Input.h>
#include <Paralax/Core/InputNames.h>
#include <Paralax/Core/JsonText.h>
#include <Paralax/Events/ApplicationEvent.h>
#include <Paralax/Events/KeyEvent.h>
#include <Paralax/Events/MouseEvent.h>

#include <cmath>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		std::string KeyNamesList()
		{
			std::string names;
			for (const InputName<KeyCode>& name : GetKeyNames())
				names += (names.empty() ? "" : ", ") + std::string(name.Name);
			return names;
		}

		std::optional<bool> ParseAction(const json& params, std::string& outError)
		{
			const std::string action = params["action"].get<std::string>();
			if (action == "press")
				return true;
			if (action == "release")
				return false;
			outError = "action must be press or release";
			return std::nullopt;
		}

		// Coordinates and scroll amounts from agents: finite and of a sane size (a float cannot hold every double).
		std::optional<glm::vec2> ParsePair(const json& params, const char* a, const char* b, double limit, std::string& outError)
		{
			const double x = params[a].get<double>();
			const double y = params[b].get<double>();
			if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > limit || std::abs(y) > limit)
			{
				outError = fmt::format("{} and {} must be finite and within +-{}", a, b, limit);
				return std::nullopt;
			}
			return glm::vec2(static_cast<float>(x), static_cast<float>(y));
		}

		// Synthetic input is for the running game: it is queued and delivered at the game's next update (so no
		// press or release is lost between agent requests), through the same path as real input.
		CommandResult Queue(EditorContext& context, Scope<Event> event)
		{
			if (!context.QueueInput(std::move(event)))
				return CommandResult::Fail("Not playing: input goes to a running game (play.start)");
			return CommandResult::Ok({{"queued", context.GetQueuedInputCount()}});
		}

	}

	void RegisterInputCommands(CommandRegistry& registry)
	{
		registry.Register({"input.key", "Presses or releases a key in the running game (names as in scripts: A-Z, D0-D9, Space, Enter, Left, Right, Up, Down, LeftShift, F1...). The game's next update sees it pressed (or released); a pressed key stays held until released.", {{"key", "string", "Key name, e.g. Space or Left.", true}, {"action", "string", "press or release.", true}}, false, [](EditorContext& context, const json& params) {
							   const std::string name = params["key"].get<std::string>();
							   std::optional<KeyCode> key = KeyFromName(name);
							   if (!key)
								   return CommandResult::Fail(fmt::format("Unknown key {}; keys are: {}", QuoteText(name), KeyNamesList()));
							   std::string error;
							   std::optional<bool> press = ParseAction(params, error);
							   if (!press)
								   return CommandResult::Fail(error);
							   if (*press)
								   return Queue(context, CreateScope<KeyPressedEvent>(*key));
							   return Queue(context, CreateScope<KeyReleasedEvent>(*key));
						   }});

		registry.Register({"input.mouse_button", "Presses or releases a mouse button in the running game.", {{"button", "string", "Left, Right or Middle.", true}, {"action", "string", "press or release.", true}}, false, [](EditorContext& context, const json& params) {
							   std::optional<MouseCode> button = MouseButtonFromName(params["button"].get<std::string>());
							   if (!button)
								   return CommandResult::Fail("button must be Left, Right or Middle");
							   std::string error;
							   std::optional<bool> press = ParseAction(params, error);
							   if (!press)
								   return CommandResult::Fail(error);
							   if (*press)
								   return Queue(context, CreateScope<MouseButtonPressedEvent>(*button));
							   return Queue(context, CreateScope<MouseButtonReleasedEvent>(*button));
						   }});

		registry.Register({"input.mouse_move", "Moves the cursor in the running game (window coordinates, origin at the top-left).", {{"x", "number", "", true}, {"y", "number", "", true}}, false, [](EditorContext& context, const json& params) {
							   std::string error;
							   std::optional<glm::vec2> position = ParsePair(params, "x", "y", 1e6, error);
							   if (!position)
								   return CommandResult::Fail(error);
							   return Queue(context, CreateScope<MouseMovedEvent>(position->x, position->y));
						   }});

		registry.Register({"input.scroll", "Scrolls the mouse wheel in the running game; its next update sees the scroll.", {{"x", "number", "Horizontal scroll.", true}, {"y", "number", "Vertical scroll (positive is away from the user).", true}}, false, [](EditorContext& context, const json& params) {
							   std::string error;
							   std::optional<glm::vec2> amount = ParsePair(params, "x", "y", 1e4, error);
							   if (!amount)
								   return CommandResult::Fail(error);
							   return Queue(context, CreateScope<MouseScrolledEvent>(amount->x, amount->y));
						   }});

		registry.Register({"input.release_all", "Releases every key and mouse button: at the game's next update while playing (it sees the releases), otherwise at once.", {}, false, [](EditorContext& context, const json&) {
							   if (context.IsPlaying())
								   return Queue(context, CreateScope<WindowLostFocusEvent>());
							   Input::OnEvent(WindowLostFocusEvent());
							   return CommandResult::Ok({{"queued", 0}});
						   }});

		registry.Register({"input.state", "Keys and mouse buttons held, the cursor position, and how many injected events wait for the game's next update.", {}, false, [](EditorContext& context, const json&) {
							   json keys = json::array();
							   for (const InputName<KeyCode>& name : GetKeyNames())
							   {
								   if (Input::IsKeyDown(name.Code))
									   keys.push_back(name.Name);
							   }
							   json buttons = json::array();
							   for (const InputName<MouseCode>& name : GetMouseButtonNames())
							   {
								   if (Input::IsMouseButtonDown(name.Code))
									   buttons.push_back(name.Name);
							   }
							   const glm::vec2 mouse = Input::GetMousePosition();
							   return CommandResult::Ok({{"keys", keys}, {"buttons", buttons}, {"mouse", {mouse.x, mouse.y}}, {"queued", context.GetQueuedInputCount()}});
						   }});
	}

}
