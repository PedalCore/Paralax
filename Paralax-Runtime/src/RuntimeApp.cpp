#include <Paralax.h>
#include <Paralax/Core/EntryPoint.h>

namespace Paralax {

	class RuntimeApp : public Application
	{
	public:
		explicit RuntimeApp(const ApplicationSpecification& specification)
			: Application(specification)
		{
		}
	};

	Scope<Application> CreateApplication(const CommandLine& args)
	{
		ApplicationSpecification specification;
		specification.Name = "Paralax Runtime";
		specification.CommandLineArgs = args;
		specification.Window.Title = "Paralax Runtime";
		specification.LogFile = "Logs/Runtime.log";

		if (!ApplyCommandLineOptions(specification))
			return nullptr;

		return CreateScope<RuntimeApp>(specification);
	}

}
