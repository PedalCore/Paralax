#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Core/CommandLine.h"
#include "Paralax/Core/LayerStack.h"
#include "Paralax/Core/Timestep.h"
#include "Paralax/Core/Window.h"
#include "Paralax/Renderer/GraphicsDevice.h"
#include "Paralax/Renderer/Swapchain.h"

#include "Paralax/Events/ApplicationEvent.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Paralax {

	class ImGuiLayer;

	struct ApplicationSpecification
	{
		std::string Name = "Paralax Application";
		// When non-empty, the process working directory is changed to this path on startup.
		std::filesystem::path WorkingDirectory;
		CommandLine CommandLineArgs;
		WindowSpecification Window;

		// Stop automatically after this many frames (0 = run until closed). Used by automated runs.
		uint64_t MaxFrames = 0;
		// When greater than zero, every frame advances by exactly this many seconds instead of wall-clock time,
		// making simulation deterministic (tests, golden images, recordings).
		float FixedTimestep = 0.0f;
		// When non-empty, log output is additionally written to this file (relative to WorkingDirectory) for the
		// lifetime of the application.
		std::filesystem::path LogFile;
		// When set (with graphics enabled and MaxFrames set), the last rendered frame of the run is saved here as a
		// PNG. An error is logged at startup if it cannot be honoured.
		std::filesystem::path ScreenshotPath;

		struct GraphicsOptions
		{
			// Creates a GraphicsDevice and a Swapchain for the main window. Off by default so non-rendering
			// applications and unit tests need no GPU.
			bool Enabled = false;
			bool Validation = false;
			// Exit with code 3 if any graphics validation error was reported (automated runs). Checked by
			// Application::Shutdown() once every layer has detached, so errors raised during teardown count too; an exit
			// code requested earlier wins (see Application::SetExitCode).
			bool FailOnValidationErrors = false;
			std::string PreferredAdapter;
		} Graphics;

		struct ImGuiOptions
		{
			// Creates an ImGuiLayer overlay (requires Graphics.Enabled); Layer::OnImGuiRender is then called each frame.
			bool Enabled = false;
			// Layout persistence file; empty disables it.
			std::filesystem::path IniFile;
		} ImGui;
	};

	// Owns the main window and the layer stack and drives the main loop. Exactly one may exist at a time.
	// All member functions are main-thread-only except SubmitToMainThread.
	//
	// Construction can fail at runtime (e.g. no display server or GPU); check IsInitialized() before Run(). A failed
	// application logs the reason and Run() returns immediately.
	//
	// Lifetime: construct, Run(), Shutdown() (which detaches the layers and settles the exit code), then destroy. The
	// entry point does all of this; the destructor shuts down if Shutdown() was never called.
	//
	// Frame order: main-thread queue, OS events, Layer::OnUpdate, then (when graphics is enabled and a back
	// buffer was acquired) Layer::OnRender and present.
	class Application
	{
	public:
		explicit Application(const ApplicationSpecification& specification);
		virtual ~Application();

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		bool IsInitialized() const { return m_Initialized; }
		// Process exit code requested so far (0 unless something failed, see SetExitCode). It is final only once
		// Shutdown() has run: layers detaching, and the graphics validation check, can still set it.
		int GetExitCode() const { return m_ExitCode; }

		// Runs the main loop until Close() is called, the window is closed, or MaxFrames is reached.
		// Returns immediately if the application failed to initialise or has been shut down.
		void Run();
		// Ends the application ahead of its destruction, so everything that can fail the run has reported before the
		// exit code is read. Waits for the GPU, calls OnShutdown(), then detaches every remaining layer in reverse
		// stack order (overlays first, top-down), destroys them, and finally checks graphics validation (see
		// GraphicsOptions::FailOnValidationErrors). Layer changes requested from OnDetach meanwhile are discarded.
		// The window and graphics device stay until destruction; afterwards Run() returns immediately and pushing a
		// layer is refused.
		//
		// Main thread only. Idempotent: once it has started, further calls (from OnShutdown, a detaching layer or the
		// destructor) do nothing. Verified not to be called from inside Run() (a layer callback or a function queued
		// with SubmitToMainThread), nor from a layer's OnAttach, OnUpdate, OnEvent, OnRender or OnImGuiRender: the
		// layers it destroys would still be in use. Use Close() to stop the main loop from there.
		void Shutdown();
		// Requests the main loop to stop after the current frame. Main thread only.
		void Close();

		// Dispatches an event through the application: input state, then layers top-down, then the application
		// itself. A layer can veto WindowCloseEvent by marking it handled. Main thread only.
		// Also used to inject synthetic events (agents, tests).
		void OnEvent(Event& event);

		// Layer stack mutation. Push takes ownership and returns a non-owning pointer to the layer (nullptr if
		// given nullptr); pop detaches and destroys the layer. When called while layers are being updated or
		// receiving an event (e.g. from inside OnUpdate/OnEvent), the change is deferred until that iteration
		// finishes; OnAttach/OnDetach run when the change is applied, and a popped layer receives no further
		// updates or events in the meantime. Changes requested from OnDetach during Shutdown() are discarded, and a
		// push after Shutdown() is refused (an error is logged, the layer is destroyed unattached and nullptr returned).
		Layer* PushLayer(Scope<Layer> layer);
		Layer* PushOverlay(Scope<Layer> overlay);
		void PopLayer(Layer* layer);
		void PopOverlay(Layer* overlay);
		template<typename T, typename... Args>
		T* PushLayer(Args&&... args)
		{
			return static_cast<T*>(PushLayer(CreateScope<T>(std::forward<Args>(args)...)));
		}
		template<typename T, typename... Args>
		T* PushOverlay(Args&&... args)
		{
			return static_cast<T*>(PushOverlay(CreateScope<T>(std::forward<Args>(args)...)));
		}

		// Saves the next fully rendered frame (scene and UI, as presented) as a PNG. Main thread only; requires graphics.
		void CaptureNextFrame(std::filesystem::path path) { m_PendingScreenshot = std::move(path); }

		// Queues a function to run on the main thread at the start of the next frame. Thread-safe.
		void SubmitToMainThread(std::function<void()> function);

		Window& GetWindow();
		bool HasGraphics() const { return m_Swapchain != nullptr; }
		// Verified to exist; only call when HasGraphics().
		GraphicsDevice& GetGraphicsDevice();
		Swapchain& GetSwapchain();
		// The ImGui overlay, or nullptr when ImGui is disabled or failed to initialise.
		ImGuiLayer* GetImGuiLayer() const { return m_ImGuiLayer; }
		const ApplicationSpecification& GetSpecification() const { return m_Specification; }
		uint64_t GetFrameCount() const { return m_FrameCount; }
		// Frames for which a back buffer was acquired, rendered and presented.
		uint64_t GetRenderedFrameCount() const { return m_RenderedFrameCount; }
		bool IsRunning() const { return m_Running; }

		// Returns the current application. Verifies that one exists.
		static Application& Get();
		static bool Exists() { return s_Instance != nullptr; }

		// Requests a process exit code (e.g. a runtime reporting failed scripts). The first non-zero request wins:
		// once the code is non-zero, later requests (including 0) are ignored. Requests count in the order they are
		// made, not the order the failures happened: those made while running come before those made by layers
		// detaching in Shutdown(), and the graphics validation check (exit code 3) requests last, after every layer
		// has detached. Main thread only.
		void SetExitCode(int code)
		{
			if (m_ExitCode == 0)
				m_ExitCode = code;
		}

	protected:
		// Called once by Shutdown(), before the remaining layers are detached, while every layer, the window and the
		// graphics device still exist and the GPU is idle. Layer changes made here apply immediately, so a derived
		// application pops here the layers that must detach in a particular order or that use its own members.
		// The destructor's Shutdown() call can no longer reach an override (the derived part is already destroyed),
		// so a derived application that overrides this calls Shutdown() from its own destructor. Main thread only.
		virtual void OnShutdown() {}

	private:
		bool InitWindowAndGraphics();
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
		bool OnWindowMinimize(WindowMinimizeEvent& e);

		void ExecuteMainThreadQueue();

		enum class ShutdownState
		{
			NotStarted,
			InProgress,
			Complete
		};

		enum class LayerOperationType
		{
			PushLayer,
			PushOverlay,
			PopLayer,
			PopOverlay
		};

		struct LayerOperation
		{
			LayerOperationType Type;
			Scope<Layer> Owned; // Push operations
			Layer* Target = nullptr;
		};

		void ApplyLayerOperation(LayerOperation operation);
		void ApplyPendingLayerOperations();
		bool IsPendingRemoval(const Layer* layer) const;
		Layer* QueueOrApply(LayerOperationType type, Scope<Layer> owned, Layer* target);

		// RAII marker for "layers are being iterated"; mutations during iteration are deferred.
		class LayerIterationScope
		{
		public:
			explicit LayerIterationScope(Application& app);
			~LayerIterationScope();

			LayerIterationScope(const LayerIterationScope&) = delete;
			LayerIterationScope& operator=(const LayerIterationScope&) = delete;

		private:
			Application& m_App;
		};

	private:
		ApplicationSpecification m_Specification;
		Scope<Window> m_Window;
		Scope<GraphicsDevice> m_GraphicsDevice;
		Scope<Swapchain> m_Swapchain;
		ImGuiLayer* m_ImGuiLayer = nullptr;
		std::filesystem::path m_PendingScreenshot;
		bool m_Initialized = false;
		int m_ExitCode = 0;
		ShutdownState m_ShutdownState = ShutdownState::NotStarted;
		LayerStack m_LayerStack;
		std::vector<LayerOperation> m_PendingLayerOperations;
		uint32_t m_LayerIterationDepth = 0;
		// Shutdown() is refused while either is non-zero (see there).
		uint32_t m_RunDepth = 0;
		uint32_t m_AttachDepth = 0;
		spdlog::sink_ptr m_LogFileSink;
		bool m_Running = true;
		bool m_Minimized = false;
		uint64_t m_FrameCount = 0;
		uint64_t m_RenderedFrameCount = 0;
		double m_LastFrameTime = 0.0;

		std::vector<std::function<void()>> m_MainThreadQueue;
		std::mutex m_MainThreadQueueMutex;

		static Application* s_Instance;
	};

	// Applies the standard engine options from specification.CommandLineArgs:
	//   --headless[=true|false]  run without a visible window or display server
	//   --frames <n>             exit after n frames (0 = run until closed, the default)
	//   --fixed-timestep <sec>   advance every frame by a fixed, positive number of seconds
	//   --validation             enable graphics validation; validation errors make the process exit non-zero
	//   --gpu <name>             prefer the graphics adapter whose name contains <name>
	//   --screenshot <file.png>  save the final frame of the run (use with --frames)
	// Values are in the engine's path encoding, as GetProcessArguments gives them: paths are decoded with FromUtf8.
	// Returns false (and logs an error) if an option is malformed; the specification is then left unchanged.
	bool ApplyCommandLineOptions(ApplicationSpecification& specification);

	// Options that never consume a following argument as their value (e.g. "--headless project.pxproj").
	// Pass to CommandLine when parsing the process arguments.
	const std::vector<std::string>& GetStandardFlagOptions();

	// Implemented by the client (editor, runtime, game); called by the entry point.
	Scope<Application> CreateApplication(const CommandLine& args);

}
