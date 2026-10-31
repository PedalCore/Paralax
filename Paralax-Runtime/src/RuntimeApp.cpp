#include <Paralax.h>
#include <Paralax/Core/EntryPoint.h>

namespace Paralax {

	// Clears the back buffer every frame; placeholder until the scene renderer lands.
	class BackgroundLayer : public Layer
	{
	public:
		BackgroundLayer()
			: Layer("Background")
		{
		}

		void OnAttach() override
		{
			if (Application::Get().HasGraphics())
				m_CommandList = Application::Get().GetGraphicsDevice().GetDevice()->createCommandList();
		}

		void OnRender() override
		{
			Application& app = Application::Get();
			m_CommandList->open();
			m_CommandList->clearTextureFloat(app.GetSwapchain().GetCurrentBackBuffer(), nvrhi::AllSubresources, nvrhi::Color(0.015f, 0.02f, 0.035f, 1.0f));
			m_CommandList->close();
			app.GetGraphicsDevice().GetDevice()->executeCommandList(m_CommandList);
		}

	private:
		nvrhi::CommandListHandle m_CommandList;
	};

	class RuntimeApp : public Application
	{
	public:
		explicit RuntimeApp(const ApplicationSpecification& specification)
			: Application(specification)
		{
			PushLayer<BackgroundLayer>();
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

		return CreateScope<RuntimeApp>(specification);
	}

}
