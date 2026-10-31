#pragma once

#include "Paralax/Core/Timestep.h"
#include "Paralax/Events/Event.h"

#include <string>
#include <utility>

namespace Paralax {

	// A unit of per-frame application logic (game layer, editor layer, debug overlay, ...).
	// Layers are updated in stack order and receive events in reverse stack order (overlays first).
	class Layer
	{
	public:
		explicit Layer(std::string name = "Layer")
			: m_DebugName(std::move(name))
		{
		}
		virtual ~Layer() = default;

		Layer(const Layer&) = delete;
		Layer& operator=(const Layer&) = delete;

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate([[maybe_unused]] Timestep ts) {}
		// Records rendering for this frame. Only called when graphics is enabled and a back buffer was acquired.
		virtual void OnRender() {}
		virtual void OnEvent([[maybe_unused]] Event& event) {}

		const std::string& GetName() const { return m_DebugName; }

	protected:
		std::string m_DebugName;
	};

}
