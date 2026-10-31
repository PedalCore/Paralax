#include "pxpch.h"
#include "Paralax/ImGui/ImGuiLayer.h"

#include "Paralax/Core/Application.h"
#include "Paralax/Core/EmbeddedFile.h"
#include "Paralax/Core/Input.h"
#include "Paralax/Events/ApplicationEvent.h"
#include "Paralax/Events/KeyEvent.h"
#include "Paralax/Events/MouseEvent.h"
#include "Paralax/ImGui/ImGuiRenderer.h"
#include "Paralax/Renderer/GraphicsDevice.h"
#include "Paralax/Renderer/Swapchain.h"

#include <imgui.h>
// ImGuizmo.h relies on imgui.h being included first.
#include <ImGuizmo.h>

namespace Paralax::EmbeddedFonts {

	extern const EmbeddedFile* const g_Entries;
	extern const size_t g_EntryCount;

}

namespace Paralax {

	namespace {

		// Backing store for the text returned to ImGui by the clipboard callback (one ImGui context per process).
		std::string s_ClipboardText;

		ImFont* AddEmbeddedFont(ImGuiIO& io, const char* name, float size)
		{
			std::span<const uint8_t> data = FindEmbeddedFile(EmbeddedFonts::g_Entries, EmbeddedFonts::g_EntryCount, name);
			if (data.empty())
			{
				PX_CORE_WARN("Embedded font '{}' is missing; using ImGui's default font", name);
				return nullptr;
			}
			ImFontConfig config;
			// The data lives in the binary for the whole program: the atlas must not try to free it.
			config.FontDataOwnedByAtlas = false;
			return io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(data.data()), static_cast<int>(data.size()), size, &config);
		}

		ImGuiKey ToImGuiKey(KeyCode key)
		{
			// Contiguous ranges first; both enumerations order these alike.
			if (key >= Key::D0 && key <= Key::D9)
				return static_cast<ImGuiKey>(ImGuiKey_0 + (key - Key::D0));
			if (key >= Key::A && key <= Key::Z)
				return static_cast<ImGuiKey>(ImGuiKey_A + (key - Key::A));
			if (key >= Key::F1 && key <= Key::F24)
				return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - Key::F1));
			if (key >= Key::KP0 && key <= Key::KP9)
				return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (key - Key::KP0));
			switch (key)
			{
				case Key::Space: return ImGuiKey_Space;
				case Key::Apostrophe: return ImGuiKey_Apostrophe;
				case Key::Comma: return ImGuiKey_Comma;
				case Key::Minus: return ImGuiKey_Minus;
				case Key::Period: return ImGuiKey_Period;
				case Key::Slash: return ImGuiKey_Slash;
				case Key::Semicolon: return ImGuiKey_Semicolon;
				case Key::Equal: return ImGuiKey_Equal;
				case Key::LeftBracket: return ImGuiKey_LeftBracket;
				case Key::Backslash: return ImGuiKey_Backslash;
				case Key::RightBracket: return ImGuiKey_RightBracket;
				case Key::GraveAccent: return ImGuiKey_GraveAccent;
				case Key::Escape: return ImGuiKey_Escape;
				case Key::Enter: return ImGuiKey_Enter;
				case Key::Tab: return ImGuiKey_Tab;
				case Key::Backspace: return ImGuiKey_Backspace;
				case Key::Insert: return ImGuiKey_Insert;
				case Key::Delete: return ImGuiKey_Delete;
				case Key::Right: return ImGuiKey_RightArrow;
				case Key::Left: return ImGuiKey_LeftArrow;
				case Key::Down: return ImGuiKey_DownArrow;
				case Key::Up: return ImGuiKey_UpArrow;
				case Key::PageUp: return ImGuiKey_PageUp;
				case Key::PageDown: return ImGuiKey_PageDown;
				case Key::Home: return ImGuiKey_Home;
				case Key::End: return ImGuiKey_End;
				case Key::CapsLock: return ImGuiKey_CapsLock;
				case Key::ScrollLock: return ImGuiKey_ScrollLock;
				case Key::NumLock: return ImGuiKey_NumLock;
				case Key::PrintScreen: return ImGuiKey_PrintScreen;
				case Key::Pause: return ImGuiKey_Pause;
				case Key::KPDecimal: return ImGuiKey_KeypadDecimal;
				case Key::KPDivide: return ImGuiKey_KeypadDivide;
				case Key::KPMultiply: return ImGuiKey_KeypadMultiply;
				case Key::KPSubtract: return ImGuiKey_KeypadSubtract;
				case Key::KPAdd: return ImGuiKey_KeypadAdd;
				case Key::KPEnter: return ImGuiKey_KeypadEnter;
				case Key::KPEqual: return ImGuiKey_KeypadEqual;
				case Key::LeftShift: return ImGuiKey_LeftShift;
				case Key::LeftControl: return ImGuiKey_LeftCtrl;
				case Key::LeftAlt: return ImGuiKey_LeftAlt;
				case Key::LeftSuper: return ImGuiKey_LeftSuper;
				case Key::RightShift: return ImGuiKey_RightShift;
				case Key::RightControl: return ImGuiKey_RightCtrl;
				case Key::RightAlt: return ImGuiKey_RightAlt;
				case Key::RightSuper: return ImGuiKey_RightSuper;
				case Key::Menu: return ImGuiKey_Menu;
				default: return ImGuiKey_None;
			}
		}

		void UpdateModifiers(ImGuiIO& io)
		{
			io.AddKeyEvent(ImGuiMod_Ctrl, Input::IsKeyDown(Key::LeftControl) || Input::IsKeyDown(Key::RightControl));
			io.AddKeyEvent(ImGuiMod_Shift, Input::IsKeyDown(Key::LeftShift) || Input::IsKeyDown(Key::RightShift));
			io.AddKeyEvent(ImGuiMod_Alt, Input::IsKeyDown(Key::LeftAlt) || Input::IsKeyDown(Key::RightAlt));
			io.AddKeyEvent(ImGuiMod_Super, Input::IsKeyDown(Key::LeftSuper) || Input::IsKeyDown(Key::RightSuper));
		}

	}

	ImGuiLayer::ImGuiLayer(ImGuiLayerSpecification specification)
		: Layer("ImGuiLayer")
		, m_Specification(std::move(specification))
	{
	}

	ImGuiLayer::~ImGuiLayer() = default;

	void ImGuiLayer::OnAttach()
	{
		Application& app = Application::Get();
		if (!app.HasGraphics())
		{
			PX_CORE_ERROR("ImGuiLayer requires graphics; ImGui is disabled");
			return;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		if (m_Specification.Docking)
			io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		m_IniFile = m_Specification.IniFile.string();
		io.IniFilename = m_IniFile.empty() ? nullptr : m_IniFile.c_str();
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		io.BackendRendererName = "Paralax NVRHI";

		ImFont* regular = AddEmbeddedFont(io, "Inter-Regular", m_Specification.FontSize);
		m_BoldFont = AddEmbeddedFont(io, "Inter-SemiBold", m_Specification.FontSize);
		if (regular)
			io.FontDefault = regular;

		ApplyTheme();

		// No ImGui platform backend: input arrives as Paralax events (OnEvent), so synthetic events from agents, tests
		// and replays drive the UI exactly like real ones; frame timing comes from the application's Timestep so UI
		// behaviour is reproducible under a fixed timestep; the clipboard goes through the Window abstraction.
		io.BackendPlatformName = "Paralax";
		ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
		platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
			// ImGui copies the text before the next call, so one buffer suffices.
			s_ClipboardText = Application::Get().GetWindow().GetClipboardText();
			return s_ClipboardText.c_str();
		};
		platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { Application::Get().GetWindow().SetClipboardText(text ? text : ""); };

		m_Renderer = CreateScope<ImGuiRenderer>(app.GetGraphicsDevice().GetDevice());
		if (!m_Renderer->Initialize())
		{
			PX_CORE_ERROR("Failed to initialise the ImGui renderer");
			m_Renderer.reset();
			ImGui::DestroyContext();
			return;
		}
		m_Initialized = true;
	}

	void ImGuiLayer::OnDetach()
	{
		if (!m_Initialized)
			return;
		m_Renderer->Shutdown();
		m_Renderer.reset();
		ImGui::DestroyContext();
		m_Initialized = false;
	}

	void ImGuiLayer::OnEvent(Event& event)
	{
		if (!m_Initialized)
			return;
		ImGuiIO& io = ImGui::GetIO();
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent& e) {
			io.AddMousePosEvent(e.GetX(), e.GetY());
			return false;
		});
		dispatcher.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
			if (e.GetMouseButton() < ImGuiMouseButton_COUNT)
				io.AddMouseButtonEvent(static_cast<int>(e.GetMouseButton()), true);
			return false;
		});
		dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& e) {
			if (e.GetMouseButton() < ImGuiMouseButton_COUNT)
				io.AddMouseButtonEvent(static_cast<int>(e.GetMouseButton()), false);
			return false;
		});
		dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent& e) {
			io.AddMouseWheelEvent(e.GetXOffset(), e.GetYOffset());
			return false;
		});
		dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
			UpdateModifiers(io);
			io.AddKeyEvent(ToImGuiKey(e.GetKeyCode()), true);
			return false;
		});
		dispatcher.Dispatch<KeyReleasedEvent>([&](KeyReleasedEvent& e) {
			UpdateModifiers(io);
			io.AddKeyEvent(ToImGuiKey(e.GetKeyCode()), false);
			return false;
		});
		dispatcher.Dispatch<KeyTypedEvent>([&](KeyTypedEvent& e) {
			io.AddInputCharacter(e.GetCodepoint());
			return false;
		});
		dispatcher.Dispatch<WindowFocusEvent>([&](WindowFocusEvent&) {
			io.AddFocusEvent(true);
			return false;
		});
		dispatcher.Dispatch<WindowLostFocusEvent>([&](WindowLostFocusEvent&) {
			io.AddFocusEvent(false);
			return false;
		});

		if (!m_BlockEvents)
			return;
		event.Handled |= event.IsInCategory(EventCategoryMouse) && io.WantCaptureMouse;
		event.Handled |= event.IsInCategory(EventCategoryKeyboard) && io.WantCaptureKeyboard;
	}

	void ImGuiLayer::Begin(Timestep timestep)
	{
		if (!m_Initialized)
			return;
		// Layout is in window (screen) coordinates, which mouse events use; the framebuffer may be larger on high-DPI
		// displays, and FramebufferScale maps between the two when rendering.
		Window& window = Application::Get().GetWindow();
		uint32_t framebufferWidth = 0;
		uint32_t framebufferHeight = 0;
		window.GetFramebufferSize(framebufferWidth, framebufferHeight);
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(window.GetWidth()), static_cast<float>(window.GetHeight()));
		if (window.GetWidth() > 0 && window.GetHeight() > 0)
			io.DisplayFramebufferScale = ImVec2(static_cast<float>(framebufferWidth) / io.DisplaySize.x, static_cast<float>(framebufferHeight) / io.DisplaySize.y);
		// ImGui requires a positive delta.
		io.DeltaTime = std::max(timestep.GetSeconds(), 1e-6f);
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
		m_FrameBegun = true;
	}

	void ImGuiLayer::End()
	{
		if (!m_Initialized || !m_FrameBegun)
			return;
		m_FrameBegun = false;

		Swapchain& swapchain = Application::Get().GetSwapchain();
		ImGui::Render();
		m_Renderer->Render(ImGui::GetDrawData(), swapchain.GetCurrentFramebuffer());
	}

	void ImGuiLayer::ApplyTheme()
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 6.0f;
		style.ChildRounding = 4.0f;
		style.FrameRounding = 4.0f;
		style.PopupRounding = 4.0f;
		style.ScrollbarRounding = 6.0f;
		style.GrabRounding = 3.0f;
		style.TabRounding = 4.0f;
		style.WindowPadding = ImVec2(10.0f, 10.0f);
		style.FramePadding = ImVec2(8.0f, 5.0f);
		style.ItemSpacing = ImVec2(8.0f, 6.0f);
		style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
		style.IndentSpacing = 16.0f;
		style.ScrollbarSize = 12.0f;
		style.WindowBorderSize = 1.0f;
		style.FrameBorderSize = 0.0f;
		style.TabBorderSize = 0.0f;
		style.WindowMenuButtonPosition = ImGuiDir_None;

		// Deep slate background with a cool cyan accent.
		const ImVec4 background(0.090f, 0.098f, 0.118f, 1.0f);
		const ImVec4 panel(0.118f, 0.129f, 0.153f, 1.0f);
		const ImVec4 frame(0.161f, 0.173f, 0.204f, 1.0f);
		const ImVec4 frameHover(0.208f, 0.224f, 0.263f, 1.0f);
		const ImVec4 accent(0.231f, 0.627f, 0.875f, 1.0f);
		const ImVec4 accentHover(0.322f, 0.698f, 0.925f, 1.0f);
		const ImVec4 accentActive(0.180f, 0.529f, 0.769f, 1.0f);
		const ImVec4 text(0.902f, 0.914f, 0.933f, 1.0f);
		const ImVec4 textDim(0.533f, 0.561f, 0.612f, 1.0f);
		const ImVec4 border(0.200f, 0.212f, 0.247f, 1.0f);

		ImVec4* colors = style.Colors;
		colors[ImGuiCol_Text] = text;
		colors[ImGuiCol_TextDisabled] = textDim;
		colors[ImGuiCol_WindowBg] = panel;
		colors[ImGuiCol_ChildBg] = panel;
		colors[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.118f, 0.141f, 0.98f);
		colors[ImGuiCol_Border] = border;
		colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_FrameBg] = frame;
		colors[ImGuiCol_FrameBgHovered] = frameHover;
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.239f, 0.255f, 0.298f, 1.0f);
		colors[ImGuiCol_TitleBg] = background;
		colors[ImGuiCol_TitleBgActive] = background;
		colors[ImGuiCol_TitleBgCollapsed] = background;
		colors[ImGuiCol_MenuBarBg] = background;
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_ScrollbarGrab] = frameHover;
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.290f, 0.306f, 0.353f, 1.0f);
		colors[ImGuiCol_ScrollbarGrabActive] = accent;
		colors[ImGuiCol_CheckMark] = accent;
		colors[ImGuiCol_SliderGrab] = accent;
		colors[ImGuiCol_SliderGrabActive] = accentActive;
		colors[ImGuiCol_Button] = frame;
		colors[ImGuiCol_ButtonHovered] = frameHover;
		colors[ImGuiCol_ButtonActive] = accentActive;
		colors[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.22f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.32f);
		colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.42f);
		colors[ImGuiCol_Separator] = border;
		colors[ImGuiCol_SeparatorHovered] = accentHover;
		colors[ImGuiCol_SeparatorActive] = accent;
		colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_ResizeGripHovered] = accentHover;
		colors[ImGuiCol_ResizeGripActive] = accent;
		colors[ImGuiCol_Tab] = background;
		colors[ImGuiCol_TabHovered] = frameHover;
		colors[ImGuiCol_TabSelected] = panel;
		colors[ImGuiCol_TabSelectedOverline] = accent;
		colors[ImGuiCol_TabDimmed] = background;
		colors[ImGuiCol_TabDimmedSelected] = panel;
		colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.5f);
		colors[ImGuiCol_DockingEmptyBg] = background;
		colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
		colors[ImGuiCol_NavCursor] = accent;
		colors[ImGuiCol_PlotHistogram] = accent;
		colors[ImGuiCol_PlotLines] = accent;
	}

}
