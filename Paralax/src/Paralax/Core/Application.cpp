#include "pxpch.h"
#include "Paralax/Core/Application.h"

#include "Paralax/Core/Input.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace Paralax {

	Application* Application::s_Instance = nullptr;

	namespace {

		double GetTimeSeconds()
		{
			using Clock = std::chrono::steady_clock;
			static const Clock::time_point s_Start = Clock::now();
			return std::chrono::duration<double>(Clock::now() - s_Start).count();
		}

	}

	Application::LayerIterationScope::LayerIterationScope(Application& app)
		: m_App(app)
	{
		m_App.m_LayerIterationDepth++;
	}

	Application::LayerIterationScope::~LayerIterationScope()
	{
		if (--m_App.m_LayerIterationDepth == 0)
			m_App.ApplyPendingLayerOperations();
	}

	Application::Application(const ApplicationSpecification& specification)
		: m_Specification(specification)
	{
		PX_CORE_VERIFY(!s_Instance, "Application already exists!");

		if (!m_Specification.WorkingDirectory.empty())
		{
			std::error_code error;
			std::filesystem::current_path(m_Specification.WorkingDirectory, error);
			if (error)
				PX_CORE_ERROR("Failed to set working directory to '{}': {}", m_Specification.WorkingDirectory.string(), error.message());
		}

		if (!m_Specification.LogFile.empty())
			m_LogFileSink = Log::AddFileSink(m_Specification.LogFile);

		Input::Reset();

		m_Window = Window::Create(m_Specification.Window);
		if (!m_Window)
		{
			PX_CORE_ERROR("Application '{}' failed to initialise: could not create the main window", m_Specification.Name);
			m_Running = false;
		}
		else
		{
			m_Window->SetEventCallback(PX_BIND_EVENT_FN(OnEvent));
		}

		m_LastFrameTime = GetTimeSeconds();

		// Registered last so a constructor that exits early never leaves a dangling instance behind.
		s_Instance = this;
	}

	Application::~Application()
	{
		// Treat teardown as an iteration so layer changes requested from OnDetach are deferred (and then discarded)
		// instead of mutating the stack mid-loop.
		m_LayerIterationDepth++;
		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
			(*it)->OnDetach();
		m_PendingLayerOperations.clear();
		m_LayerStack.Clear();
		m_LayerIterationDepth--;

		m_Window.reset();
		Input::Reset();

		if (m_LogFileSink)
			Log::RemoveSink(m_LogFileSink);

		s_Instance = nullptr;
	}

	Application& Application::Get()
	{
		PX_CORE_VERIFY(s_Instance, "No Application exists");
		return *s_Instance;
	}

	Window& Application::GetWindow()
	{
		PX_CORE_VERIFY(m_Window, "Application has no window (initialisation failed)");
		return *m_Window;
	}

	Layer* Application::PushLayer(Scope<Layer> layer)
	{
		return QueueOrApply(LayerOperationType::PushLayer, std::move(layer), nullptr);
	}

	Layer* Application::PushOverlay(Scope<Layer> overlay)
	{
		return QueueOrApply(LayerOperationType::PushOverlay, std::move(overlay), nullptr);
	}

	void Application::PopLayer(Layer* layer)
	{
		QueueOrApply(LayerOperationType::PopLayer, nullptr, layer);
	}

	void Application::PopOverlay(Layer* overlay)
	{
		QueueOrApply(LayerOperationType::PopOverlay, nullptr, overlay);
	}

	Layer* Application::QueueOrApply(LayerOperationType type, Scope<Layer> owned, Layer* target)
	{
		const bool isPush = type == LayerOperationType::PushLayer || type == LayerOperationType::PushOverlay;
		if (isPush)
		{
			if (!owned)
			{
				PX_CORE_ERROR("Cannot push a null layer");
				return nullptr;
			}
			target = owned.get();
		}

		LayerOperation operation{type, std::move(owned), target};
		if (m_LayerIterationDepth > 0)
			m_PendingLayerOperations.push_back(std::move(operation));
		else
			ApplyLayerOperation(std::move(operation));

		return isPush ? target : nullptr;
	}

	void Application::ApplyLayerOperation(LayerOperation operation)
	{
		switch (operation.Type)
		{
			case LayerOperationType::PushLayer:
				m_LayerStack.PushLayer(std::move(operation.Owned))->OnAttach();
				break;
			case LayerOperationType::PushOverlay:
				m_LayerStack.PushOverlay(std::move(operation.Owned))->OnAttach();
				break;
			case LayerOperationType::PopLayer:
			case LayerOperationType::PopOverlay:
			{
				const bool isLayer = operation.Type == LayerOperationType::PopLayer;
				Scope<Layer> removed = isLayer ? m_LayerStack.PopLayer(operation.Target) : m_LayerStack.PopOverlay(operation.Target);
				if (removed)
					removed->OnDetach();
				else
					PX_CORE_WARN("Pop{} ignored: the {} is not in the layer stack", isLayer ? "Layer" : "Overlay", isLayer ? "layer" : "overlay");
				break;
			}
		}
	}

	bool Application::IsPendingRemoval(const Layer* layer) const
	{
		for (const LayerOperation& operation : m_PendingLayerOperations)
		{
			const bool isPop = operation.Type == LayerOperationType::PopLayer || operation.Type == LayerOperationType::PopOverlay;
			if (isPop && operation.Target == layer)
				return true;
		}
		return false;
	}

	void Application::ApplyPendingLayerOperations()
	{
		// OnAttach/OnDetach may queue further operations; apply until none remain.
		while (!m_PendingLayerOperations.empty())
		{
			std::vector<LayerOperation> operations;
			operations.swap(m_PendingLayerOperations);
			for (LayerOperation& operation : operations)
				ApplyLayerOperation(std::move(operation));
		}
	}

	void Application::Close()
	{
		m_Running = false;
	}

	void Application::SubmitToMainThread(std::function<void()> function)
	{
		std::scoped_lock lock(m_MainThreadQueueMutex);
		m_MainThreadQueue.emplace_back(std::move(function));
	}

	void Application::ExecuteMainThreadQueue()
	{
		std::vector<std::function<void()>> queue;
		{
			std::scoped_lock lock(m_MainThreadQueueMutex);
			queue.swap(m_MainThreadQueue);
		}

		// Run outside the lock so queued functions may themselves submit more work (it runs next frame).
		for (auto& function : queue)
			function();
	}

	void Application::OnEvent(Event& event)
	{
		Input::OnEvent(event);

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>(PX_BIND_EVENT_FN(OnWindowResize));
		dispatcher.Dispatch<WindowMinimizeEvent>(PX_BIND_EVENT_FN(OnWindowMinimize));

		{
			LayerIterationScope iteration(*this);
			for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
			{
				if (event.Handled)
					break;
				// Layers popped earlier in this pass no longer receive events.
				if (!IsPendingRemoval(it->get()))
					(*it)->OnEvent(event);
			}
		}

		// Layers see WindowClose first and can veto it (e.g. unsaved changes) by marking it handled.
		if (!event.Handled)
			dispatcher.Dispatch<WindowCloseEvent>(PX_BIND_EVENT_FN(OnWindowClose));
	}

	void Application::Run()
	{
		if (!IsInitialized())
		{
			PX_CORE_ERROR("Application::Run called on an application that failed to initialise");
			return;
		}

		m_LastFrameTime = GetTimeSeconds();

		while (m_Running)
		{
			const double time = GetTimeSeconds();
			Timestep timestep = m_Specification.FixedTimestep > 0.0f
									? Timestep(m_Specification.FixedTimestep)
									: Timestep(static_cast<float>(time - m_LastFrameTime));
			m_LastFrameTime = time;

			ExecuteMainThreadQueue();
			m_Window->PollEvents();

			if (!m_Minimized)
			{
				LayerIterationScope iteration(*this);
				for (auto& layer : m_LayerStack)
				{
					if (!IsPendingRemoval(layer.get()))
						layer->OnUpdate(timestep);
				}
			}

			Input::EndFrame();
			m_FrameCount++;

			if (m_Specification.MaxFrames != 0 && m_FrameCount >= m_Specification.MaxFrames)
				m_Running = false;
		}
	}

	bool Application::OnWindowClose([[maybe_unused]] WindowCloseEvent& e)
	{
		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		m_Minimized = e.GetWidth() == 0 || e.GetHeight() == 0;
		return false;
	}

	bool Application::OnWindowMinimize(WindowMinimizeEvent& e)
	{
		m_Minimized = e.IsMinimized();
		return false;
	}

	namespace {

		// Parses "true"/"false"/"1"/"0" (case-sensitive); an absent value means true.
		std::optional<bool> ParseBoolOption(const std::optional<std::string>& value)
		{
			if (!value)
				return true;
			if (*value == "true" || *value == "1")
				return true;
			if (*value == "false" || *value == "0")
				return false;
			return std::nullopt;
		}

		// Parses a strictly positive, finite number of seconds. Rejects whitespace and trailing characters.
		std::optional<float> ParsePositiveSeconds(const std::optional<std::string>& value)
		{
			if (!value || value->empty() || std::isspace(static_cast<unsigned char>(value->front())))
				return std::nullopt;

			// std::strtof rather than std::from_chars: libc++ on older Apple toolchains lacks floating-point
			// from_chars. The engine never calls setlocale, so the "C" locale (decimal point) applies.
			const char* begin = value->c_str();
			char* end = nullptr;
			errno = 0;
			const float seconds = std::strtof(begin, &end);
			if (errno != 0 || end != begin + value->size() || !std::isfinite(seconds) || seconds <= 0.0f)
				return std::nullopt;

			return seconds;
		}

	}

	const std::vector<std::string>& GetStandardFlagOptions()
	{
		static const std::vector<std::string> s_Flags = {"headless"};
		return s_Flags;
	}

	bool ApplyCommandLineOptions(ApplicationSpecification& specification)
	{
		const CommandLine& args = specification.CommandLineArgs;
		ApplicationSpecification result = specification;

		if (args.HasOption("headless"))
		{
			std::optional<bool> headless = ParseBoolOption(args.GetOption("headless"));
			if (!headless)
			{
				PX_CORE_ERROR("--headless expects true, false, 1 or 0");
				return false;
			}
			result.Window.Headless = *headless;
		}

		bool malformed = false;
		if (std::optional<uint64_t> frames = args.GetUnsignedOption("frames", &malformed))
			result.MaxFrames = *frames;
		if (malformed)
		{
			PX_CORE_ERROR("--frames expects a non-negative integer");
			return false;
		}

		if (args.HasOption("fixed-timestep"))
		{
			std::optional<float> seconds = ParsePositiveSeconds(args.GetOption("fixed-timestep"));
			if (!seconds)
			{
				PX_CORE_ERROR("--fixed-timestep expects a positive number of seconds");
				return false;
			}
			result.FixedTimestep = *seconds;
		}

		specification = std::move(result);
		return true;
	}

}
