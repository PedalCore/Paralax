#include <Paralax.h>
#include <Paralax/Core/EntryPoint.h>
#include <Paralax/Physics/PhysicsSystem.h>
#include <Paralax/Project/Project.h>
#include <Paralax/Renderer/SceneRenderer.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>
#include <Paralax/Scripting/ScriptSystem.h>

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
				, m_Scene(std::move(scene))
				, m_FailOnScriptError(failOnScriptError)
			{
			}

			void OnAttach() override
			{
				Application& app = Application::Get();
				// Scripts first: forces they apply in OnUpdate act in the same update's physics step.
				m_Scripts = static_cast<ScriptSystem*>(m_Scene->AddSystem(CreateScope<ScriptSystem>(m_Project->GetAssetManager())));
				m_Scene->AddSystem(CreateScope<PhysicsSystem>());
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
				m_Scene->OnRuntimeStart();
			}

			void OnDetach() override
			{
				m_Scene->OnRuntimeStop();
				if (m_FailOnScriptError && !m_Scripts->GetErrors().empty())
				{
					PX_CORE_ERROR("{} script error(s) occurred", m_Scripts->GetErrors().size());
					Application::Get().SetExitCode(ScriptErrorExitCode);
				}
				m_Renderer.reset();
			}

			void OnUpdate(Timestep timestep) override { m_Scene->OnUpdateRuntime(timestep); }

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
				m_Scene->SetViewportSize(width, height);
				const float aspect = static_cast<float>(width) / static_cast<float>(height);

				std::optional<CameraView> view;
				if (std::optional<Entity> camera = m_Scene->GetPrimaryCameraEntity())
					view = CameraView::FromEntity(*m_Scene, *camera, aspect);
				if (!view)
				{
					if (!m_WarnedNoCamera)
					{
						PX_CORE_WARN("The scene has no primary camera; using a default view");
						m_WarnedNoCamera = true;
					}
					view = CameraView::LookAt(glm::vec3(0.0f, 2.0f, 8.0f), glm::vec3(0.0f), glm::radians(50.0f), aspect);
				}
				m_Renderer->Render(*m_Scene, *view, swapchain.GetCurrentFramebuffer());
			}

		private:
			Ref<Project> m_Project;
			Ref<Scene> m_Scene;
			bool m_FailOnScriptError = false;
			ScriptSystem* m_Scripts = nullptr;
			Scope<SceneRenderer> m_Renderer;
			bool m_WarnedNoCamera = false;
		};

		// Finds the game next to the executable (how exported games are laid out).
		std::optional<std::filesystem::path> FindBundledProject(const CommandLine& args)
		{
			if (args.GetProgram().empty())
				return std::nullopt;
			std::error_code error;
			const std::filesystem::path directory = std::filesystem::absolute(args.GetProgram(), error).parent_path();
			if (error)
				return std::nullopt;
			std::optional<std::filesystem::path> found;
			for (const auto& entry : std::filesystem::directory_iterator(directory, error))
			{
				if (entry.path().extension() != Project::FileExtension)
					continue;
				if (found)
					return std::nullopt; // Ambiguous.
				found = entry.path();
			}
			return found;
		}

		Ref<Scene> LoadStartScene(Project& project, const CommandLine& args)
		{
			std::filesystem::path path;
			if (std::optional<std::string> scene = args.GetOption("scene"))
			{
				std::optional<std::filesystem::path> registryPath = project.GetAssetManager().ToRegistryPath(*scene);
				if (!registryPath)
				{
					PX_CORE_ERROR("--scene '{}' is not inside the project's assets", *scene);
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
			projectPath = FindBundledProject(args);
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
