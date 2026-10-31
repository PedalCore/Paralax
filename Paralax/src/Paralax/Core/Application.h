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

		struct GraphicsOptions
		{
			// Creates a GraphicsDevice and a Swapchain for the main window. Off by default so non-rendering
			// applications and unit tests need no GPU.
			bool Enabled = false;
			bool Validation = false;
			// Exit with a non-zero code if any graphics validation error was reported (automated runs).
			bool FailOnValidationErrors = false;
			std::string PreferredAdapter;
		} Graphics;
	};

	// Owns the main window and the layer stack and drives the main loop. Exactly one may exist at a time.
	// All member functions are main-thread-only except SubmitToMainThread.
	//
	// Construction can fail at runtime (e.g. no display server or GPU); check IsInitialized() before Run(). A failed
	// application logs the reason and Run() returns immediately.
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
		// Process exit code requested by the application (0 unless something failed, see GraphicsOptions).
		int GetExitCode() const { return m_ExitCode; }

		// Runs the main loop until Close() is called, the window is closed, or MaxFrames is reached.
		// Returns immediately if the application failed to initialise.
		void Run();
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
		// updates or events in the meantime. Changes requested from OnDetach during destruction are discarded.
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

		// Queues a function to run on the main thread at the start of the next frame. Thread-safe.
		void SubmitToMainThread(std::function<void()> function);

		Window& GetWindow();
		bool HasGraphics() const { return m_Swapchain != nullptr; }
		// Verified to exist; only call when HasGraphics().
		GraphicsDevice& GetGraphicsDevice();
		Swapchain& GetSwapchain();
		const ApplicationSpecification& GetSpecification() const { return m_Specification; }
		uint64_t GetFrameCount() const { return m_FrameCount; }
		// Frames for which a back buffer was acquired, rendered and presented.
		uint64_t GetRenderedFrameCount() const { return m_RenderedFrameCount; }
		bool IsRunning() const { return m_Running; }

		// Returns the current application. Verifies that one exists.
		static Application& Get();
		static bool Exists() { return s_Instance != nullptr; }

	private:
		bool InitWindowAndGraphics();
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
		bool OnWindowMinimize(WindowMinimizeEvent& e);

		void ExecuteMainThreadQueue();

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
		bool m_Initialized = false;
		int m_ExitCode = 0;
		LayerStack m_LayerStack;
		std::vector<LayerOperation> m_PendingLayerOperations;
		uint32_t m_LayerIterationDepth = 0;
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
	// Returns false (and logs an error) if an option is malformed; the specification is then left unchanged.
	bool ApplyCommandLineOptions(ApplicationSpecification& specification);

	// Options that never consume a following argument as their value (e.g. "--headless project.pxproj").
	// Pass to CommandLine when parsing the process arguments.
	const std::vector<std::string>& GetStandardFlagOptions();

	// Implemented by the client (editor, runtime, game); called by the entry point.
	Scope<Application> CreateApplication(const CommandLine& args);

}
