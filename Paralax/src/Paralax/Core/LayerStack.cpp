#include "pxpch.h"
#include "Paralax/Core/LayerStack.h"

#include <algorithm>
#include <iterator>

namespace Paralax {

	LayerStack::~LayerStack()
	{
		Clear();
	}

	Layer* LayerStack::PushLayer(Scope<Layer> layer)
	{
		PX_CORE_VERIFY(layer, "Cannot push a null layer");
		Layer* raw = layer.get();
		m_Layers.emplace(m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex), std::move(layer));
		m_LayerInsertIndex++;
		return raw;
	}

	Layer* LayerStack::PushOverlay(Scope<Layer> overlay)
	{
		PX_CORE_VERIFY(overlay, "Cannot push a null overlay");
		Layer* raw = overlay.get();
		m_Layers.emplace_back(std::move(overlay));
		return raw;
	}

	Scope<Layer> LayerStack::PopLayer(Layer* layer)
	{
		const auto layersEnd = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex);
		auto it = std::find_if(m_Layers.begin(), layersEnd, [layer](const Scope<Layer>& l) { return l.get() == layer; });
		if (it == layersEnd)
			return nullptr;

		Scope<Layer> result = std::move(*it);
		m_Layers.erase(it);
		m_LayerInsertIndex--;
		return result;
	}

	Scope<Layer> LayerStack::PopOverlay(Layer* overlay)
	{
		const auto overlaysBegin = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex);
		auto it = std::find_if(overlaysBegin, m_Layers.end(), [overlay](const Scope<Layer>& l) { return l.get() == overlay; });
		if (it == m_Layers.end())
			return nullptr;

		Scope<Layer> result = std::move(*it);
		m_Layers.erase(it);
		return result;
	}

	void LayerStack::Clear()
	{
		while (!m_Layers.empty())
			m_Layers.pop_back();
		m_LayerInsertIndex = 0;
	}

}
