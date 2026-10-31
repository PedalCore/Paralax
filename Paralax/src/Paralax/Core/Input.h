#pragma once

#include "Paralax/Core/KeyCodes.h"
#include "Paralax/Core/MouseCodes.h"

#include <glm/vec2.hpp>

namespace Paralax {

	class Event;

	// Polled input state, built from the input events the application receives.
	//
	// Because state is derived from events rather than queried from the OS, synthetic events (agents,
	// automated tests, input replay) behave exactly like real input. All functions must be called on the
	// main thread.
	//
	// "Down" means held; "Pressed"/"Released" mean the transition happened during the current frame.
	class Input
	{
	public:
		static bool IsKeyDown(KeyCode key);
		static bool WasKeyPressed(KeyCode key);
		static bool WasKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button);
		static bool WasMouseButtonPressed(MouseCode button);
		static bool WasMouseButtonReleased(MouseCode button);

		// Cursor position in window coordinates (origin at the top-left of the content area).
		static glm::vec2 GetMousePosition();
		static float GetMouseX() { return GetMousePosition().x; }
		static float GetMouseY() { return GetMousePosition().y; }

		// Scroll accumulated during the current frame.
		static glm::vec2 GetMouseScrollDelta();

		// Updates state from an input event. Called by Application for every event; never marks it handled.
		static void OnEvent(const Event& event);
		// Clears per-frame transitions. Called by Application at the end of every frame.
		static void EndFrame();
		// Releases all keys and buttons and clears all state (e.g. when the window loses focus).
		static void Reset();
	};

}
