#include "EditorCore/AgentServer.h"
#include "EditorCore/ConsoleSink.h"
#include "EditorCore/EditorContext.h"
#include "EditorLayer.h"

#include <Paralax.h>
#include <Paralax/Core/EntryPoint.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Project/Project.h>

#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace Paralax {

	// Editor options (in addition to the standard engine options, see ApplyCommandLineOptions):
	//   --project <dir|file.pxproj>   open a project
	//   --new-project <dir>           create a project there (named after the directory) and open it
	//   --scene <path>                open a scene (relative to the project's Assets folder, or absolute)
	//   --agent-port <port>           start the agent JSON-RPC server on 127.0.0.1 (0 = any free port)
	//   --agent-info <file>           where to write the server's connection details (default: <project>/.paralax/AgentServer.json,
	//                                 or ./.paralax/AgentServer.json without a project)
	//   --export <dir>                export the project as a standalone game (project.export) and exit; the exit code is 1
	//                                 if the export failed. A relative path is taken from the working directory.

	namespace {

		// Lets the agent server's network threads wake the main thread. The layer installs the wake function while
		// attached; requests queued at other times are picked up by the next OnUpdate.
		struct AgentServerWake
		{
			std::mutex Mutex;
			std::function<void()> Wake;

			void operator()()
			{
				std::lock_guard lock(Mutex);
				if (Wake)
					Wake();
			}
		};

		// Runs agent requests on the main thread, and publishes the connection file while running. Requests are
		// processed every frame and also as soon as they arrive (via SubmitToMainThread), so agents are served while
		// the window is minimised and layers are not updated.
		class AgentServerLayer : public Layer
		{
		public:
			AgentServerLayer(Editor::EditorContext& context, Scope<Editor::AgentServer> server, std::shared_ptr<AgentServerWake> wake, std::filesystem::path infoFile)
				: Layer("AgentServer")
				, m_Context(context)
				, m_Server(std::move(server))
				, m_Wake(std::move(wake))
				, m_InfoFile(std::move(infoFile))
			{
			}

			void OnAttach() override
			{
				// The server accepts connections from here on, once the application exists and its log sinks are
				// installed: the network threads log, and sink changes must not race them (see Log).
				if (!m_Server->Run())
				{
					PX_CORE_ERROR("The agent server could not start; closing the editor");
					FailLaunch();
					return;
				}
				{
					std::lock_guard lock(m_Wake->Mutex);
					m_Wake->Wake = [this, alive = std::weak_ptr<int>(m_Alive)] {
						Application::Get().SubmitToMainThread([this, alive] {
							// The layer may have been detached since the request was queued.
							if (alive.lock() && m_Server)
								m_Server->ProcessRequests(m_Context);
						});
					};
				}
				std::error_code error;
				std::filesystem::create_directories(m_InfoFile.parent_path(), error);
				// The token grants full control of the editor, so the file is made owner-only while still empty,
				// before the token is written into it. (On Windows this only sets the read-only attribute; the file is
				// then protected by the directory's access control list, which by default is the user's.)
				std::filesystem::remove(m_InfoFile, error);
				std::ofstream(m_InfoFile, std::ios::trunc).close();
				std::filesystem::permissions(m_InfoFile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, error);
				if (error)
				{
					PX_CORE_ERROR("Could not restrict access to the agent connection file '{}': {}; closing the editor", ToUtf8(m_InfoFile), error.message());
					FailLaunch();
					return;
				}
				std::ofstream file(m_InfoFile, std::ios::trunc);
				file << nlohmann::json{{"host", "127.0.0.1"}, {"port", m_Server->GetPort()}, {"token", m_Server->GetToken()}}.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
				file.close();
				if (!file)
				{
					PX_CORE_ERROR("Could not write the agent connection file '{}'; closing the editor", ToUtf8(m_InfoFile));
					FailLaunch();
					return;
				}
				PX_CORE_INFO("Agent connection details written to '{}'", ToUtf8(m_InfoFile));
			}

			void OnDetach() override
			{
				{
					std::lock_guard lock(m_Wake->Mutex);
					m_Wake->Wake = nullptr;
				}
				m_Alive.reset();
				std::error_code error;
				std::filesystem::remove(m_InfoFile, error);
				m_Server.reset();
			}

			void OnUpdate(Timestep) override
			{
				if (m_Server)
					m_Server->ProcessRequests(m_Context);
			}

		private:
			// An agent asked for control and cannot get it, without a server or without the connection file (which
			// holds the token no agent can connect without): fail the launch rather than leave a launcher waiting for
			// the file until it gives up. The editor closes at the end of the frame, with exit code 1.
			static void FailLaunch()
			{
				Application::Get().SetExitCode(1);
				Application::Get().Close();
			}

			Editor::EditorContext& m_Context;
			Scope<Editor::AgentServer> m_Server;
			std::shared_ptr<AgentServerWake> m_Wake;
			std::shared_ptr<int> m_Alive = std::make_shared<int>(0);
			std::filesystem::path m_InfoFile;
		};

	}

	// Final: OnShutdown's contract (an override calls Shutdown() from its own destructor) would not hold for a subclass.
	class EditorApp final : public Application
	{
	public:
		// agentInfoFile is where the agent server, if there is one, publishes its connection details.
		EditorApp(const ApplicationSpecification& specification, Ref<Project> project, Scope<Editor::AgentServer> agentServer, std::shared_ptr<AgentServerWake> agentWake,
				  std::filesystem::path agentInfoFile)
			: Application(specification)
			, m_ConsoleSink(std::make_shared<Editor::ConsoleSink>())
			, m_Context(std::move(project))
		{
			Log::AddSink(m_ConsoleSink);
			if (!IsInitialized())
				return;

			const CommandLine& args = specification.CommandLineArgs;
			if (std::optional<std::string> scene = args.GetOption("scene"))
			{
				const std::filesystem::path scenePath = FromUtf8(*scene);
				if (!m_Context.OpenScene(scenePath))
					PX_CORE_ERROR("Could not open scene '{}'", ToUtf8(scenePath));
			}
			else if (Project* active = m_Context.GetProject(); active && active->GetStartScene().IsValid())
			{
				if (const AssetMetadata* metadata = active->GetAssetManager().GetMetadata(active->GetStartScene()))
					m_Context.OpenScene(metadata->FilePath);
			}
			// Only when no scene opened: an empty --scene or start scene is the user's file and stays empty.
			Editor::CreateDemoScene(m_Context);

			// A windowed editor plays sound; headless ones (agents, CI) mix silently and deterministically.
			if (!specification.Window.Headless)
				m_Context.SetAudioOutput(AudioOutput::Device);
			m_EditorLayer = PushLayer<Editor::EditorLayer>(m_Context, m_ConsoleSink);

			Editor::RegisterApplicationCommands(m_Context.GetCommands(), m_ConsoleSink);

			if (std::optional<std::string> output = args.GetOption("export"))
			{
				// On the command line a relative path means the working directory (the command itself takes relative
				// paths from the project directory).
				std::error_code error;
				const std::filesystem::path absolute = std::filesystem::absolute(FromUtf8(*output), error);
				const Editor::CommandResult result = m_Context.Execute("project.export", {{"output", ToUtf8(absolute)}});
				if (result.Success)
					PX_CORE_INFO("Exported the game to '{}'", result.Result["path"].get<std::string>());
				else
				{
					PX_CORE_ERROR("Export failed: {}", result.Error);
					SetExitCode(1);
				}
				Close();
				return;
			}

			if (agentServer)
				m_AgentServerLayer = PushOverlay<AgentServerLayer>(m_Context, std::move(agentServer), std::move(agentWake), std::move(agentInfoFile));
		}

		~EditorApp() override
		{
			// The editor's layers reference m_Context, which is destroyed before ~Application runs: shut down (and so
			// detach them, see OnShutdown) while it is still alive. Does nothing if the entry point already did.
			Shutdown();
			Log::RemoveSink(m_ConsoleSink);
		}

	protected:
		void OnShutdown() override
		{
			// The agent server goes first, so no request arrives while the editor tears down. The editor UI detaches
			// next, before the generic teardown detaches the ImGui overlay it draws with.
			if (Layer* server = std::exchange(m_AgentServerLayer, nullptr))
				PopOverlay(server);
			if (Layer* editor = std::exchange(m_EditorLayer, nullptr))
				PopLayer(editor);
		}

	private:
		Layer* m_EditorLayer = nullptr;
		Layer* m_AgentServerLayer = nullptr;
		std::shared_ptr<Editor::ConsoleSink> m_ConsoleSink;
		Editor::EditorContext m_Context;
	};

	Scope<Application> CreateApplication(const CommandLine& args)
	{
		ApplicationSpecification specification;
		specification.Name = "Paralax Editor";
		specification.CommandLineArgs = args;
		specification.Window.Title = "Paralax Editor";
		specification.LogFile = "Logs/Editor.log";
		specification.Graphics.Enabled = true;
		specification.ImGui.Enabled = true;

		if (!ApplyCommandLineOptions(specification))
			return nullptr;

		Ref<Project> project;
		if (std::optional<std::string> path = args.GetOption("new-project"))
		{
			const std::filesystem::path directory = FromUtf8(*path);
			project = Project::Create(directory, ToUtf8(directory.filename()));
			if (!project)
				return nullptr;
		}
		else if (std::optional<std::string> existing = args.GetOption("project"))
		{
			project = Project::Load(FromUtf8(*existing));
			if (!project)
				return nullptr;
		}

		// Layout is stored per project (or in the working directory without one); headless runs never persist it.
		if (!specification.Window.Headless)
			specification.ImGui.IniFile = project ? project->GetDirectory() / "EditorLayout.ini" : std::filesystem::path("EditorLayout.ini");
		if (project)
			specification.Window.Title = fmt::format("Paralax Editor - {}", project->GetName());

		// The server binds its port before the application exists so a bad port fails the launch: an agent that asked
		// for control would otherwise wait on an editor it cannot reach. It starts serving (and its threads) only
		// once its layer is attached, after the application has installed the log sinks.
		Scope<Editor::AgentServer> agentServer;
		auto agentWake = std::make_shared<AgentServerWake>();
		std::filesystem::path agentInfoFile;
		bool malformedPort = false;
		if (std::optional<uint64_t> port = args.GetUnsignedOption("agent-port", &malformedPort); port || malformedPort)
		{
			if (malformedPort || *port > UINT16_MAX)
			{
				PX_CORE_ERROR("--agent-port expects a port number between 0 and 65535");
				return nullptr;
			}
			// For the same reason, an editor that would have nowhere to publish the connection details (and so the
			// token no agent can connect without) does not start either. The error is logged.
			std::optional<std::filesystem::path> infoFile = Editor::ResolveAgentInfoPath(FromUtf8(args.GetOption("agent-info").value_or("")), project ? project->GetDirectory() : std::filesystem::path());
			if (!infoFile)
				return nullptr;
			agentInfoFile = std::move(*infoFile);
			Editor::AgentServerSpecification serverSpecification;
			serverSpecification.Port = static_cast<uint16_t>(*port);
			serverSpecification.OnRequestQueued = [agentWake] { (*agentWake)(); };
			agentServer = Editor::AgentServer::Listen(serverSpecification);
			if (!agentServer)
				return nullptr;
		}

		return CreateScope<EditorApp>(specification, project, std::move(agentServer), std::move(agentWake), std::move(agentInfoFile));
	}

}
