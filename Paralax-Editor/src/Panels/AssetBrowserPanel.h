#pragma once

namespace Paralax::Editor {

	class EditorContext;

	// Lists the project's assets (and built-ins); assets can be dragged onto asset fields, imported from the
	// Assets folder, and scenes opened by double-clicking.
	class AssetBrowserPanel
	{
	public:
		void Draw(EditorContext& context);

	private:
		char m_ImportPath[260] = {};
	};

}
