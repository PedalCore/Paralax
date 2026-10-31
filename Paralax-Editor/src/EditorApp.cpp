#include <Paralax.h>
#include <Paralax/Core/EntryPoint.h>

namespace Paralax {

	class EditorApp : public Application
	{
	public:
		explicit EditorApp(const ApplicationSpecification& specification)
			: Application(specification)
		{
		}
	};

	Scope<Application> CreateApplication(const CommandLine& args)
	{
		ApplicationSpecification specification;
		specification.Name = "Paralax Editor";
		specification.CommandLineArgs = args;
		specification.Window.Title = "Paralax Editor";
		specification.LogFile = "Logs/Editor.log";

		if (!ApplyCommandLineOptions(specification))
			return nullptr;

		return CreateScope<EditorApp>(specification);
	}

}
