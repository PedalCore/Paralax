#include "Panels/AssetBrowserPanel.h"

#include "EditorCore/EditorContext.h"

#include <Paralax/Core/PathEncoding.h>

#include <imgui.h>

namespace Paralax::Editor {

	namespace {

		// One row of the asset table. With contextAction, right-clicking the row offers that action; contextChosen is
		// set when it is picked.
		void AssetRow(const char* type, const std::string& name, uint64_t handle, bool* doubleClicked = nullptr, const char* contextAction = nullptr, bool* contextChosen = nullptr)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(static_cast<int>(handle ^ (handle >> 32)));
			ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
			if (doubleClicked)
				*doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			if (contextAction && contextChosen && ImGui::BeginPopupContextItem())
			{
				*contextChosen = ImGui::MenuItem(contextAction);
				ImGui::EndPopup();
			}
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload("PARALAX_ASSET", &handle, sizeof(handle));
				ImGui::Text("%s", name.c_str());
				ImGui::EndDragDropSource();
			}
			ImGui::PopID();
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%s", type);
			ImGui::TableSetColumnIndex(2);
			ImGui::TextDisabled("%llu", static_cast<unsigned long long>(handle));
		}

	}

	void AssetBrowserPanel::Draw(EditorContext& context)
	{
		ImGui::Begin("Assets");
		if (context.GetProject())
		{
			ImGui::SetNextItemWidth(-90.0f);
			ImGui::InputTextWithHint("##import", "Path inside Assets, e.g. Models/ship.glb", m_ImportPath, sizeof(m_ImportPath));
			ImGui::SameLine();
			if (ImGui::Button("Import", ImVec2(-FLT_MIN, 0.0f)) && m_ImportPath[0] != '\0')
			{
				const CommandResult result = context.Execute("asset.import", {{"path", m_ImportPath}});
				if (!result.Success)
					PX_ERROR("{}", result.Error);
			}
		}
		else
		{
			ImGui::TextDisabled("No project open: only built-in assets are available.");
		}

		if (ImGui::BeginTable("##assets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
		{
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (const BuiltinAssetInfo& info : AssetManager::GetBuiltinAssets())
				AssetRow(std::string(AssetTypeToString(info.Type)).c_str(), fmt::format("{} (built-in)", info.Name), info.Handle);

			for (const AssetMetadata& metadata : context.GetAssets().GetRegisteredAssets())
			{
				std::string name = ToUtf8(metadata.FilePath);
				if (metadata.Parent.IsValid())
					name += fmt::format(" > {} {}", AssetTypeToString(metadata.Type), static_cast<uint64_t>(metadata.Handle) % 10000);
				bool doubleClicked = false;
				bool createPrefab = false;
				const bool isModel = metadata.Type == AssetType::Mesh && !metadata.Parent.IsValid();
				AssetRow(std::string(AssetTypeToString(metadata.Type)).c_str(), name, metadata.Handle, &doubleClicked, isModel ? "Create Prefab From Model" : nullptr, &createPrefab);
				if (doubleClicked && metadata.Type == AssetType::Scene)
					context.Execute("scene.open", {{"path", ToUtf8(metadata.FilePath)}});
				// It writes files and registers assets (changing the list being drawn): run it after the panels.
				if (createPrefab)
					context.Defer("model.create_prefab", {{"model", std::to_string(static_cast<uint64_t>(metadata.Handle))}});
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}

}
