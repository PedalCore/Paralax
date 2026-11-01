#include "EditorCore/AgentServer.h"
#include "EditorCore/ConsoleSink.h"
#include "EditorCore/EditorContext.h"
#include "EditorLayer.h"

#include <Paralax.h>
#include <Paralax/Core/EntryPoint.h>
#include <Paralax/Project/Project.h>

#include <fstream>
#include <functional>
#include <memory>
#include <mutex>

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
					PX_CORE_ERROR("Could not restrict access to the agent connection file '{}': {}", m_InfoFile.string(), error.message());
					return;
				}
				std::ofstream file(m_InfoFile, std::ios::trunc);
				file << nlohmann::json{{"host", "127.0.0.1"}, {"port", m_Server->GetPort()}, {"token", m_Server->GetToken()}}.dump(2) << "\n";
				file.close();
				if (!file)
				{
					PX_CORE_ERROR("Could not write the agent connection file '{}'", m_InfoFile.string());
					return;
				}
				PX_CORE_INFO("Agent connection details written to '{}'", m_InfoFile.string());
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
			Editor::EditorContext& m_Context;
			Scope<Editor::AgentServer> m_Server;
			std::shared_ptr<AgentServerWake> m_Wake;
			std::shared_ptr<int> m_Alive = std::make_shared<int>(0);
			std::filesystem::path m_InfoFile;
		};

	}

	class EditorApp : public Application
	{
	public:
		EditorApp(const ApplicationSpecification& specification, Ref<Project> project, Scope<Editor::AgentServer> agentServer, std::shared_ptr<AgentServerWake> agentWake)
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
				if (!m_Context.OpenScene(*scene))
					PX_CORE_ERROR("Could not open scene '{}'", *scene);
			}
			else if (Project* active = m_Context.GetProject(); active && active->GetStartScene().IsValid())
			{
				if (const AssetMetadata* metadata = active->GetAssetManager().GetMetadata(active->GetStartScene()))
					m_Context.OpenScene(metadata->FilePath);
			}
			if (m_Context.GetEditorScene().GetEntityCount() == 0)
				Editor::EditorLayer::CreateDemoScene(m_Context);

			m_EditorLayer = PushLayer<Editor::EditorLayer>(m_Context, m_ConsoleSink);

			m_Context.GetCommands().Register({"editor.quit", "Closes the editor at the end of the current frame. Unsaved changes are lost.", {}, false, [](Editor::EditorContext&, const nlohmann::json&) {
												  Application::Get().Close();
												  return Editor::CommandResult::Ok();
											  }});

			if (std::optional<std::string> output = args.GetOption("export"))
			{
				// On the command line a relative path means the working directory (the command itself takes relative
				// paths from the project directory).
				std::error_code error;
				const std::u8string absolute = std::filesystem::absolute(std::filesystem::path(std::u8string(output->begin(), output->end())), error).generic_u8string();
				const Editor::CommandResult result = m_Context.Execute("project.export", {{"output", std::string(absolute.begin(), absolute.end())}});
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
			{
				std::filesystem::path infoFile = args.GetOption("agent-info").value_or("");
				if (infoFile.empty())
					infoFile = (m_Context.GetProject() ? m_Context.GetProject()->GetDirectory() : std::filesystem::current_path()) / ".paralax" / "AgentServer.json";
				m_AgentServerLayer = PushOverlay<AgentServerLayer>(m_Context, std::move(agentServer), std::move(agentWake), std::move(infoFile));
			}
		}

		~EditorApp() override
		{
			// These layers reference m_Context, which is destroyed before ~Application would detach them: detach
			// them here while it is still alive.
			if (m_AgentServerLayer)
				PopOverlay(m_AgentServerLayer);
			if (m_EditorLayer)
				PopLayer(m_EditorLayer);
			Log::RemoveSink(m_ConsoleSink);
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
			const std::filesystem::path directory(*path);
			project = Project::Create(directory, directory.filename().string());
			if (!project)
				return nullptr;
		}
		else if (std::optional<std::string> existing = args.GetOption("project"))
		{
			project = Project::Load(*existing);
			if (!project)
				return nullptr;
		}

		// Layout is stored per project (or in the working directory without one); headless runs never persist it.
		if (!specification.Window.Headless)
			specification.ImGui.IniFile = project ? project->GetDirectory() / "EditorLayout.ini" : std::filesystem::path("EditorLayout.ini");
		if (project)
			specification.Window.Title = fmt::format("Paralax Editor - {}", project->GetName());

		// The server starts before the application so a bad port fails the launch: an agent that asked for control
		// would otherwise wait on an editor it cannot reach.
		Scope<Editor::AgentServer> agentServer;
		auto agentWake = std::make_shared<AgentServerWake>();
		bool malformedPort = false;
		if (std::optional<uint64_t> port = args.GetUnsignedOption("agent-port", &malformedPort); port || malformedPort)
		{
			if (malformedPort || *port > UINT16_MAX)
			{
				PX_CORE_ERROR("--agent-port expects a port number between 0 and 65535");
				return nullptr;
			}
			Editor::AgentServerSpecification serverSpecification;
			serverSpecification.Port = static_cast<uint16_t>(*port);
			serverSpecification.OnRequestQueued = [agentWake] { (*agentWake)(); };
			agentServer = Editor::AgentServer::Start(serverSpecification);
			if (!agentServer)
				return nullptr;
		}

		return CreateScope<EditorApp>(specification, project, std::move(agentServer), std::move(agentWake));
	}

}
