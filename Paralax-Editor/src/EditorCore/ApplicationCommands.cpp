#include "EditorCore/ConsoleSink.h"
#include "EditorCore/EditorContext.h"

#include <Paralax/Core/Application.h>

#include <utility>

namespace Paralax::Editor {

	void RegisterApplicationCommands(CommandRegistry& registry, std::shared_ptr<ConsoleSink> sink)
	{
		// The log is what commands refer to when they fail ("see the log"), so agents can read it too.
		RegisterConsoleCommands(registry, std::move(sink));
		registry.Register({"editor.quit", "Closes the editor at the end of the current frame. Unsaved changes are lost.", {}, false, [](EditorContext&, const nlohmann::json&) {
							   Application::Get().Close();
							   return CommandResult::Ok();
						   }});
	}

}
