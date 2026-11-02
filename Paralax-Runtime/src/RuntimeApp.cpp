#include <Paralax.h>
#include <Paralax/Core/EngineResources.h>
#include <Paralax/Core/EntryPoint.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Game/GameSession.h>
#include <Paralax/Project/Project.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>

namespace Paralax {

	// Runtime options (in addition to the standard engine options, see ApplyCommandLineOptions):
	//   --project <dir|file.pxproj>   the game to run (default: the only .pxproj next to the executable)
	//   --scene <path>                scene to play instead of the project's start scene (relative to Assets)
	//   --fail-on-script-error        exit with code 4 if any script error occurred (automated runs, CI)
	namespace {

		constexpr int ScriptErrorExitCode = 4;

		// Plays one scene: simulates it every update and renders it through its primary camera.
		class GameLayer : public Layer
		{
		public:
			GameLayer(Ref<Project> project, Ref<Scene> scene, bool failOnScriptError)
				: Layer("Game")
				, m_Project(std::move(project))
				, m_StartScene(std::move(scene))
				, m_FailOnScriptError(failOnScriptError)
			{
			}

			void OnAttach() override
			{
				Application& app = Application::Get();
				if (app.HasGraphics())
				{
					m_Renderer = CreateScope<SceneRenderer>(app.GetGraphicsDevice(), m_Project->GetAssetManager());
					if (!m_Renderer->Initialize())
					{
						PX_CORE_ERROR("Failed to initialise the scene renderer");
						m_Renderer.reset();
						app.SetExitCode(1);
						app.Close();
					}
				}
				// Headless runs mix sound silently.
				GameSessionSpecification specification;
				specification.Audio = app.GetSpecification().Window.Headless ? AudioOutput::Offline : AudioOutput::Device;
				m_Game = CreateScope<GameSession>(m_Project->GetAssetManager(), *m_StartScene, specification);
				m_StartScene = nullptr; // The session keeps its own copy.
			}

			void OnDetach() override
			{
				m_Game->End(); // OnDestroy runs first: its errors count too.
				const size_t errors = m_Game->GetScriptErrors().size();
				m_Game.reset();
				if (m_FailOnScriptError && errors > 0)
				{
					PX_CORE_ERROR("{} script error(s) occurred", errors);
					Application::Get().SetExitCode(ScriptErrorExitCode);
				}
				m_Renderer.reset();
			}

			void OnUpdate(Timestep timestep) override { m_Game->Update(timestep); }

			void OnRender() override
			{
				if (!m_Renderer)
					return;
				Application& app = Application::Get();
				Swapchain& swapchain = app.GetSwapchain();
				const uint32_t width = swapchain.GetWidth();
				const uint32_t height = swapchain.GetHeight();
				if (width == 0 || height == 0)
					return;
				Scene& scene = m_Game->GetScene(); // May be a different scene each update (Scene.Load).
				scene.SetViewportSize(width, height);
				const float aspect = static_cast<float>(width) / static_cast<float>(height);

				std::optional<CameraView> view;
				if (std::optional<Entity> camera = scene.GetPrimaryCameraEntity())
					view = CameraView::FromEntity(scene, *camera, aspect);
				if (!view)
				{
					if (!m_WarnedNoCamera)
					{
						PX_CORE_WARN("The scene has no primary camera; using a default view");
						m_WarnedNoCamera = true;
					}
					view = CameraView::LookAt(glm::vec3(0.0f, 2.0f, 8.0f), glm::vec3(0.0f), glm::radians(50.0f), aspect);
				}
				m_Renderer->Render(scene, *view, swapchain.GetCurrentFramebuffer());
			}

		private:
			Ref<Project> m_Project;
			Ref<Scene> m_StartScene;
			Scope<GameSession> m_Game;
			bool m_FailOnScriptError = false;
			Scope<SceneRenderer> m_Renderer;
			bool m_WarnedNoCamera = false;
		};

		// Finds the game next to the executable (how exported games are laid out). The executable's real location is
		// used rather than argv[0], which may be a bare name found on PATH.
		std::optional<std::filesystem::path> FindBundledProject()
		{
			const std::filesystem::path executable = GetExecutablePath();
			if (executable.empty())
				return std::nullopt;
			std::error_code error;
			std::optional<std::filesystem::path> found;
			for (auto it = std::filesystem::directory_iterator(executable.parent_path(), error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			{
				if (it->path().extension() != Project::FileExtension)
					continue;
				if (found)
					return std::nullopt; // Ambiguous.
				found = it->path();
			}
			return found;
		}

		Ref<Scene> LoadStartScene(Project& project, const CommandLine& args)
		{
			std::filesystem::path path;
			if (std::optional<std::string> scene = args.GetOption("scene"))
			{
				// Arguments are in the native narrow encoding (the ANSI code page on Windows); logs are UTF-8.
				const std::filesystem::path scenePath(*scene);
				std::string nameError;
				std::optional<std::filesystem::path> registryPath = project.GetAssetManager().ToRegistryPath(scenePath, &nameError);
				if (!registryPath)
				{
					if (!nameError.empty())
						PX_CORE_ERROR("--scene: {}", nameError);
					else
						PX_CORE_ERROR("--scene '{}' is not inside the project's assets", ToUtf8(scenePath));
					return nullptr;
				}
				path = project.GetAssetManager().ToAbsolutePath(*registryPath);
			}
			else if (const AssetMetadata* metadata = project.GetAssetManager().GetMetadata(project.GetStartScene()))
			{
				path = project.GetAssetManager().ToAbsolutePath(metadata->FilePath);
			}
			else
			{
				PX_CORE_ERROR("The project '{}' has no start scene (set one in the editor, or pass --scene)", project.GetName());
				return nullptr;
			}
			return SceneSerializer::LoadFromFile(path);
		}

	}

	class RuntimeApp : public Application
	{
	public:
		RuntimeApp(const ApplicationSpecification& specification, Ref<Project> project, Ref<Scene> scene)
			: Application(specification)
		{
			if (!IsInitialized())
				return;
			PushLayer<GameLayer>(std::move(project), std::move(scene), specification.CommandLineArgs.HasOption("fail-on-script-error"));
		}
	};

	Scope<Application> CreateApplication(const CommandLine& args)
	{
		ApplicationSpecification specification;
		specification.Name = "Paralax Runtime";
		specification.CommandLineArgs = args;
		specification.Window.Title = "Paralax Runtime";
		specification.LogFile = "Logs/Runtime.log";
		specification.Graphics.Enabled = true;

		if (!ApplyCommandLineOptions(specification))
			return nullptr;

		std::optional<std::filesystem::path> projectPath;
		if (std::optional<std::string> option = args.GetOption("project"))
			projectPath = std::filesystem::path(*option);
		else
			projectPath = FindBundledProject();
		if (!projectPath)
		{
			PX_CORE_ERROR("No game to run: pass --project <dir|file.pxproj>, or place exactly one .pxproj next to the executable");
			return nullptr;
		}

		Ref<Project> project = Project::Load(*projectPath);
		if (!project)
			return nullptr;
		Ref<Scene> scene = LoadStartScene(*project, args);
		if (!scene)
			return nullptr;

		specification.Window.Title = project->GetName();
		return CreateScope<RuntimeApp>(specification, std::move(project), std::move(scene));
	}

}
