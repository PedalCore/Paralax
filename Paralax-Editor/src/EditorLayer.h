#pragma once

#include "EditorCore/EditorCamera.h"
#include "EditorCore/ConsoleSink.h"
#include "EditorCore/EditorContext.h"
#include "Panels/AssetBrowserPanel.h"
#include "Panels/ConsolePanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/SceneHierarchyPanel.h"

#include <Paralax/Core/Layer.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Scene/Entity.h>

#include <memory>
#include <string>

namespace Paralax::Editor {

	// The editor UI: docked panels around a scene viewport. All state lives in the EditorContext; this layer draws it,
	// renders the viewport and turns input into editor commands.
	class EditorLayer : public Layer
	{
	public:
		EditorLayer(EditorContext& context, std::shared_ptr<ConsoleSink> consoleSink);
		~EditorLayer() override;

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep ts) override;
		void OnRender() override;
		void OnImGuiRender() override;
		void OnEvent(Event& event) override;

	private:
		void EnsureViewportTarget(uint32_t width, uint32_t height);
		CameraView GetViewportCamera(float aspect);
		bool IsViewingThroughSceneCamera();
		void RegisterCommands();
		CommandResult Capture(const nlohmann::json& params);

		void DrawMenuBar();
		void DrawToolbar();
		void DrawViewport();
		void DrawRenderSettings();
		void DrawStatistics();
		void SetupDefaultLayout(unsigned int dockspace);
		void HandleShortcuts();
		void PickEntity(float u, float v);
		void FocusSelection();
		void FocusEntity(Entity entity);

		// Commands this layer registers while attached (they need the renderer, camera or gizmo).
		static constexpr const char* s_LayerCommands[] = {"editor.camera.get", "editor.camera.set", "editor.camera.focus", "editor.gizmo.get", "editor.gizmo.set",
														  "render.settings.get", "render.settings.set", "render.stats", "render.aa.state", "render.aa.reset", "render.gi.state", "render.gi.reset", "render.gi.step", "render.gi.inspect"};

	private:
		EditorContext& m_Context;
		std::shared_ptr<ConsoleSink> m_ConsoleSink;
		Scope<SceneRenderer> m_Renderer;
		LocalShadowHistory m_ShadowHistory;
		EditorCamera m_Camera;

		SceneHierarchyPanel m_Hierarchy;
		InspectorPanel m_Inspector;
		AssetBrowserPanel m_AssetBrowser;
		ConsolePanel m_Console;

		nvrhi::TextureHandle m_ViewportTexture;
		nvrhi::FramebufferHandle m_ViewportFramebuffer;
		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;
		bool m_ViewportHovered = false;
		bool m_ViewportFocused = false;
		int m_GizmoOperation = 0; // ImGuizmo::OPERATION; 0 = none
		bool m_GizmoWorldSpace = false;
		bool m_GizmoInUse = false;
		bool m_LayoutInitialized = false;
		bool m_ShowRenderSettings = true;
		bool m_ShowStatistics = false;
		bool m_ShowImGuiDemo = false;
		std::string m_ScenePathInput;
		float m_FrameTime = 0.0f;
	};

}
