#pragma once

#include "EditorCore/ConsoleSink.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Paralax::Editor {

	class EditorContext;

	// Shows log output (with level filter and search) and runs editor commands typed as "<command> <json params>",
	// e.g. `entity.create {"name": "Box"}`.
	class ConsolePanel
	{
	public:
		explicit ConsolePanel(std::shared_ptr<ConsoleSink> sink);
		void Draw(EditorContext& context);

	private:
		std::shared_ptr<ConsoleSink> m_Sink;
		std::string m_Filter;
		std::string m_Command;
		bool m_ShowTrace = false;
		// Messages are copied from the sink only when its revision changes, not every frame.
		std::vector<ConsoleSink::Message> m_Messages;
		uint64_t m_LastRevision = UINT64_MAX;
	};

}
