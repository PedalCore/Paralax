#pragma once

#include <cstdint>
#include <set>

struct ImFont;

namespace Paralax::Editor {

	class EditorContext;

	// Shows and edits the primary selected entity: name, tag and every reflected component, with an "Add Component"
	// menu. Also edits the selected entity's mesh materials inline (material edits are saved to their files but are
	// not part of scene undo). Structural changes are deferred to EditorContext::ExecuteDeferred.
	class InspectorPanel
	{
	public:
		void Draw(EditorContext& context, ImFont* headerFont);
		// Writes edited materials to disk; unless forced, waits until no widget is being dragged.
		void SaveMaterials(EditorContext& context, bool force);

	private:
		std::set<uint64_t> m_UnsavedMaterials;
	};

}
