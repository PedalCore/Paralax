#pragma once

namespace Paralax::Editor {

	class EditorContext;

	// Tree of the scene's entities: selection, drag-and-drop reparenting/reordering and a context menu for
	// creating, duplicating and deleting entities. All changes go through editor commands (undoable).
	class SceneHierarchyPanel
	{
	public:
		void Draw(EditorContext& context);
	};

}
