#include "Panels/SceneHierarchyPanel.h"

#include "EditorCore/EditorContext.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Scene/Entity.h>

#include <imgui.h>

#include <cstring>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		std::string IdString(UUID id)
		{
			return std::to_string(static_cast<uint64_t>(id));
		}

		// Menu entries shared by the window and entity context menus. parent may be 0 (create at root).
		void DrawCreateMenu(EditorContext& context, UUID parent)
		{
			auto create = [&](const char* name, json components) {
				json params = {{"name", name}, {"components", std::move(components)}};
				if (parent.IsValid())
					params["parent"] = IdString(parent);
				context.Defer("entity.create", params);
			};
			auto mesh = [](uint64_t handle) { return json{{"Mesh", {{"Mesh", std::to_string(handle)}}}}; };

			if (ImGui::MenuItem("Empty"))
				create("Entity", json::object());
			ImGui::Separator();
			if (ImGui::MenuItem("Cube"))
				create("Cube", mesh(AssetManager::BuiltinCube));
			if (ImGui::MenuItem("Sphere"))
				create("Sphere", mesh(AssetManager::BuiltinSphere));
			if (ImGui::MenuItem("Plane"))
				create("Plane", mesh(AssetManager::BuiltinPlane));
			if (ImGui::MenuItem("Cylinder"))
				create("Cylinder", mesh(AssetManager::BuiltinCylinder));
			if (ImGui::MenuItem("Capsule"))
				create("Capsule", mesh(AssetManager::BuiltinCapsule));
			ImGui::Separator();
			if (ImGui::MenuItem("Directional Light"))
				create("Sun", {{"DirectionalLight", json::object()}, {"Transform", {{"Rotation", {-0.42, 0.25, 0.12, 0.86}}}}});
			if (ImGui::MenuItem("Point Light"))
				create("Point Light", {{"PointLight", json::object()}});
			if (ImGui::MenuItem("Spot Light"))
				create("Spot Light", {{"SpotLight", json::object()}});
			if (ImGui::MenuItem("Sky Light"))
				create("Sky", {{"SkyLight", json::object()}});
			ImGui::Separator();
			if (ImGui::MenuItem("Camera"))
				create("Camera", {{"Camera", json::object()}});
		}

		void DrawEntity(EditorContext& context, Scene& scene, Entity entity, int depth)
		{
			const auto& relationship = entity.GetComponent<RelationshipComponent>();
			const UUID id = entity.GetUUID();

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
			if (context.IsSelected(id))
				flags |= ImGuiTreeNodeFlags_Selected;
			if (relationship.Children.empty())
				flags |= ImGuiTreeNodeFlags_Leaf;

			ImGui::PushID(static_cast<int>(static_cast<uint64_t>(id) ^ (static_cast<uint64_t>(id) >> 32)));
			const bool open = ImGui::TreeNodeEx("##node", flags, "%s", entity.GetName().c_str());

			if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
				context.Select(id, ImGui::GetIO().KeyCtrl);

			if (ImGui::BeginDragDropSource())
			{
				const uint64_t value = id;
				ImGui::SetDragDropPayload("PARALAX_ENTITY", &value, sizeof(value));
				ImGui::Text("%s", entity.GetName().c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PARALAX_ENTITY"))
				{
					uint64_t dragged = 0;
					if (payload->DataSize == sizeof(uint64_t))
						std::memcpy(&dragged, payload->Data, sizeof(dragged));
					if (dragged != 0 && dragged != static_cast<uint64_t>(id))
						context.Defer("entity.set_parent", {{"id", std::to_string(dragged)}, {"parent", IdString(id)}});
				}
				ImGui::EndDragDropTarget();
			}

			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::BeginMenu("Create Child"))
				{
					DrawCreateMenu(context, id);
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
					context.Defer("entity.duplicate", {{"id", IdString(id)}});
				if (ImGui::MenuItem("Unparent", nullptr, false, relationship.Parent.IsValid()))
					context.Defer("entity.set_parent", {{"id", IdString(id)}, {"parent", nullptr}});
				ImGui::Separator();
				if (ImGui::MenuItem("Delete", "Delete"))
					context.Defer("entity.destroy", {{"id", IdString(id)}});
				ImGui::EndPopup();
			}

			if (open)
			{
				// UI recursion is bounded: deeper levels are summarised instead of drawn.
				if (depth < 64)
				{
					const std::vector<UUID> children = relationship.Children;
					for (UUID child : children)
					{
						if (std::optional<Entity> childEntity = scene.TryGetEntityByUUID(child))
							DrawEntity(context, scene, *childEntity, depth + 1);
					}
				}
				else
				{
					ImGui::TextDisabled("(%zu more levels)", relationship.Children.size());
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

	}

	void SceneHierarchyPanel::Draw(EditorContext& context)
	{
		ImGui::Begin("Hierarchy");
		Scene& scene = context.GetActiveScene();

		if (context.IsPlaying())
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f), "Playing - changes are discarded on stop");

		const std::vector<UUID> roots = scene.GetRootEntities();
		for (UUID root : roots)
		{
			if (std::optional<Entity> entity = scene.TryGetEntityByUUID(root))
				DrawEntity(context, scene, *entity, 0);
		}

		// Empty space: drop to unparent, click to deselect, right-click to create.
		ImGui::Dummy(ImGui::GetContentRegionAvail());
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PARALAX_ENTITY"))
			{
				uint64_t dragged = 0;
				if (payload->DataSize == sizeof(uint64_t))
					std::memcpy(&dragged, payload->Data, sizeof(dragged));
				if (dragged != 0)
					context.Defer("entity.set_parent", {{"id", std::to_string(dragged)}, {"parent", nullptr}});
			}
			ImGui::EndDragDropTarget();
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			context.ClearSelection();
		if (ImGui::BeginPopupContextItem("##hierarchy_context"))
		{
			DrawCreateMenu(context, UUID(0));
			ImGui::EndPopup();
		}

		ImGui::End();
	}

}
