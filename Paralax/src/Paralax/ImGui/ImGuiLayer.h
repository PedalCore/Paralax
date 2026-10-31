#pragma once

#include "Paralax/Core/Layer.h"
#include "Paralax/Core/Timestep.h"

#include <filesystem>

struct ImFont;

namespace Paralax {

	class ImGuiRenderer;

	struct ImGuiLayerSpecification
	{
		// Where ImGui stores window/docking layout; empty disables persistence (tests, headless runs).
		std::filesystem::path IniFile;
		bool Docking = true;
		float FontSize = 16.0f;
	};

	// Owns the Dear ImGui context and its NVRHI renderer. Application drives it: Begin() before
	// Layer::OnImGuiRender, End() after (which renders into the current back buffer). Input reaches ImGui only as
	// Paralax events through OnEvent (so injected events work), and frame time comes from the Timestep.
	// While BlockEvents is set, mouse/keyboard events ImGui wants are not passed to the layers below. Main-thread-only.
	class ImGuiLayer : public Layer
	{
	public:
		explicit ImGuiLayer(ImGuiLayerSpecification specification = {});
		~ImGuiLayer() override;

		void OnAttach() override;
		void OnDetach() override;
		void OnEvent(Event& event) override;

		void Begin(Timestep timestep);
		void End();

		void SetBlockEvents(bool block) { m_BlockEvents = block; }
		bool IsInitialized() const { return m_Initialized; }

		ImFont* GetBoldFont() const { return m_BoldFont; }

		static void ApplyTheme();

	private:
		ImGuiLayerSpecification m_Specification;
		Scope<ImGuiRenderer> m_Renderer;
		std::string m_IniFile;
		ImFont* m_BoldFont = nullptr;
		bool m_BlockEvents = true;
		bool m_Initialized = false;
		bool m_FrameBegun = false;
	};

}
