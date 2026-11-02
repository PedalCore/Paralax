#include "Panels/InspectorPanel.h"

#include "EditorCore/EditorContext.h"
#include "Panels/FieldWidgets.h"

#include <Paralax/Asset/MaterialAsset.h>
#include <Paralax/Asset/MaterialSerializer.h>
#include <Paralax/Asset/MeshAsset.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Entity.h>

#include <imgui.h>
#include <imgui_stdlib.h>

namespace Paralax::Editor {

	namespace {

		// Returns true if the material changed this frame.
		bool DrawMaterialEditor(EditorContext& context, AssetHandle handle, int slot)
		{
			Ref<MaterialAsset> material = context.GetAssets().GetAsset<MaterialAsset>(handle);
			if (!material)
				return false;
			const bool builtin = context.GetAssets().IsBuiltin(handle);
			const std::string label = fmt::format("Material {} ({})", slot, builtin ? "Default" : std::to_string(static_cast<uint64_t>(handle)));
			if (!ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
				return false;
			bool changed = false;
			if (builtin)
			{
				ImGui::TextDisabled("Built-in material (create a material to edit).");
			}
			// Materials are assets, not scene data: edits are saved to their files and are not part of scene undo.
			else if (DrawFields(context, MaterialSerializer::GetMaterialFields(), material.get(), nullptr))
			{
				material->Version++;
				changed = true;
			}
			ImGui::TreePop();
			return changed;
		}

	}

	void InspectorPanel::Draw(EditorContext& context, ImFont* headerFont)
	{
		ImGui::Begin("Inspector");
		std::optional<UUID> selected = context.GetPrimarySelection();
		std::optional<Entity> entity = selected ? context.GetActiveScene().TryGetEntityByUUID(*selected) : std::nullopt;
		if (!entity)
		{
			ImGui::TextDisabled("Select an entity to inspect it.");
			ImGui::End();
			return;
		}

		Scene& scene = context.GetActiveScene();
		entt::registry& registry = scene.GetRegistry();
		const std::string id = std::to_string(static_cast<uint64_t>(entity->GetUUID()));

		// Name
		{
			std::string name = entity->GetName();
			if (headerFont)
				ImGui::PushFont(headerFont, 0.0f);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty())
			{
				TruncateUtf8(name, TagComponent::MaxLength); // The longest name entity.rename accepts.
				context.Defer("entity.rename", {{"id", id}, {"name", name}});
			}
			if (headerFont)
				ImGui::PopFont();
			ImGui::TextDisabled("ID %s", id.c_str());
		}
		ImGui::Spacing();

		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			void* component = info.Get(registry, entity->GetHandle());
			if (!component)
				continue;

			ImGui::PushID(info.Name.c_str());
			const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
			const bool open = ImGui::CollapsingHeader(info.Name.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
			if (info.Remove)
			{
				ImGui::SameLine(rightEdge - ImGui::GetFrameHeight());
				if (ImGui::SmallButton("x"))
					context.Defer("component.remove", {{"id", id}, {"component", info.Name}});
				ImGui::SetItemTooltip("Remove %s", info.Name.c_str());
			}
			if (open)
			{
				DrawFields(context, info.Fields, component, "Edit Component");
				if (info.Name == "Mesh")
				{
					// Inline material editing for each slot (override or the mesh's own material).
					const auto& meshComponent = *static_cast<MeshComponent*>(component);
					if (Ref<MeshAsset> mesh = context.GetAssets().GetAsset<MeshAsset>(meshComponent.Mesh))
					{
						for (size_t slot = 0; slot < mesh->Materials.size(); slot++)
						{
							AssetHandle handle = slot < meshComponent.Materials.size() && meshComponent.Materials[slot].IsValid() ? meshComponent.Materials[slot] : mesh->Materials[slot];
							if (!handle.IsValid())
								handle = AssetHandle(AssetManager::BuiltinDefaultMaterial);
							if (DrawMaterialEditor(context, handle, static_cast<int>(slot)))
								m_UnsavedMaterials.insert(static_cast<uint64_t>(handle));
						}
					}
				}
			}
			ImGui::PopID();
		}

		ImGui::Spacing();
		const float width = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width * 0.2f);
		if (ImGui::Button("Add Component", ImVec2(width * 0.6f, 0.0f)))
			ImGui::OpenPopup("AddComponent");
		if (ImGui::BeginPopup("AddComponent"))
		{
			for (const ComponentInfo& info : ComponentRegistry::GetAll())
			{
				if (!info.UserEditable || info.Has(registry, entity->GetHandle()))
					continue;
				if (ImGui::MenuItem(info.Name.c_str()))
					context.Defer("component.set", {{"id", id}, {"components", {{info.Name, nlohmann::json::object()}}}});
				if (!info.Description.empty())
					ImGui::SetItemTooltip("%s", info.Description.c_str());
			}
			ImGui::EndPopup();
		}

		ImGui::End();
		SaveMaterials(context, false);
	}

	void InspectorPanel::SaveMaterials(EditorContext& context, bool force)
	{
		// Material edits are written once the interaction ends rather than on every frame of a drag.
		if (m_UnsavedMaterials.empty() || (!force && ImGui::IsAnyItemActive()))
			return;
		AssetManager& assets = context.GetAssets();
		for (uint64_t value : m_UnsavedMaterials)
		{
			const AssetHandle handle(value);
			Ref<MaterialAsset> material = assets.GetAsset<MaterialAsset>(handle);
			const AssetMetadata* metadata = assets.GetMetadata(handle);
			// glTF sub-materials change in memory only.
			if (!material || !metadata || metadata->Parent.IsValid())
				continue;
			if (!MaterialSerializer::SaveToFile(*material, assets.ToAbsolutePath(metadata->FilePath)))
				PX_CORE_ERROR("Could not save material '{}'", ToUtf8(metadata->FilePath));
		}
		m_UnsavedMaterials.clear();
	}

}
