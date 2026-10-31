#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Core/Layer.h"

#include <vector>

namespace Paralax {

	// Ordered collection of layers that owns them. Regular layers always come before overlays.
	// The stack does not call OnAttach/OnDetach; Application does.
	class LayerStack
	{
	public:
		LayerStack() = default;
		~LayerStack();

		LayerStack(const LayerStack&) = delete;
		LayerStack& operator=(const LayerStack&) = delete;

		// Takes ownership and returns a non-owning pointer to the inserted layer. Verifies the layer is non-null.
		Layer* PushLayer(Scope<Layer> layer);
		Layer* PushOverlay(Scope<Layer> overlay);

		// Removes the layer and returns ownership to the caller, or nullptr if it is not in this stack.
		Scope<Layer> PopLayer(Layer* layer);
		Scope<Layer> PopOverlay(Layer* overlay);

		// Detaching is the owner's responsibility; Clear() only destroys layers, last pushed first.
		void Clear();

		size_t Size() const { return m_Layers.size(); }
		bool Empty() const { return m_Layers.empty(); }

		std::vector<Scope<Layer>>::iterator begin() { return m_Layers.begin(); }
		std::vector<Scope<Layer>>::iterator end() { return m_Layers.end(); }
		std::vector<Scope<Layer>>::reverse_iterator rbegin() { return m_Layers.rbegin(); }
		std::vector<Scope<Layer>>::reverse_iterator rend() { return m_Layers.rend(); }

		std::vector<Scope<Layer>>::const_iterator begin() const { return m_Layers.begin(); }
		std::vector<Scope<Layer>>::const_iterator end() const { return m_Layers.end(); }

	private:
		std::vector<Scope<Layer>> m_Layers;
		size_t m_LayerInsertIndex = 0;
	};

}
