#pragma once

#include <Paralax/Core/Reflection.h>

namespace Paralax::Editor {

	class EditorContext;

	// Draws an ImGui widget for one reflected field and writes edits into the object. Each interaction (from first
	// change to release) is recorded as one undo step via the context's interactive edit, which the editor closes
	// once per frame (EditorContext::EndStaleInteractiveEdit). A null undoLabel edits without undo (for objects that
	// are not scene data). Returns true if the value changed this frame.
	bool DrawField(EditorContext& context, const FieldInfo& field, void* object, const char* undoLabel);

	// Draws all fields of an object in a two-column property table.
	bool DrawFields(EditorContext& context, std::span<const FieldInfo> fields, void* object, const char* undoLabel);

}
