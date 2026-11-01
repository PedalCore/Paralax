#include "Panels/FieldWidgets.h"

#include "EditorCore/EditorContext.h"

#include <Paralax/Math/Math.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace Paralax::Editor {

	namespace {

		float DragSpeed(const FieldInfo& field)
		{
			if (field.Min < field.Max)
				return std::max((field.Max - field.Min) * 0.002f, 0.001f);
			return 0.02f;
		}

	}

	bool DrawField(EditorContext& context, const FieldInfo& field, void* object, const char* undoLabel)
	{
		void* value = field.Access(object);
		bool changed = false;
		ImGui::PushID(field.Name.c_str());

		// Widgets edit a copy; the copy is written back only after ApplyInteractiveEdit has snapshotted the scene,
		// so the undo step captures the value from before the interaction. Continuous widgets (drags, text) keep
		// their edit open with TouchInteractiveEdit while active; EditorLayer closes it the first frame they are not.
		const ImGuiID key = ImGui::GetID("##value");
		// A null undoLabel means the object is not scene data (e.g. renderer settings): no undo tracking.
		const bool undoable = undoLabel != nullptr;
		auto write = [&](auto& target, const auto& newValue, ImGuiID editKey) {
			if (undoable)
				context.ApplyInteractiveEdit(editKey, undoLabel);
			target = newValue;
			changed = true;
		};
		// Discrete changes (checkbox, combo, drop, add/remove) are one complete step each.
		auto writeOnce = [&](auto&& apply) {
			if (undoable)
			{
				context.CommitInteractiveEdit();
				context.BeginUndoableChange(undoLabel);
			}
			apply();
			if (undoable)
				context.EndUndoableChange(true);
			changed = true;
		};

		const bool hasRange = field.Min < field.Max;
		bool continuous = true;
		switch (field.Type)
		{
			case FieldType::Bool:
			{
				continuous = false;
				bool& target = *static_cast<bool*>(value);
				bool edited = target;
				if (ImGui::Checkbox("##value", &edited))
					writeOnce([&] { target = edited; });
				break;
			}

			case FieldType::Int:
			{
				int& target = *static_cast<int*>(value);
				int edited = target;
				if (ImGui::DragInt("##value", &edited, 0.2f, hasRange ? static_cast<int>(field.Min) : 0, hasRange ? static_cast<int>(field.Max) : 0))
					write(target, edited, key);
				break;
			}

			case FieldType::Float:
			{
				float& target = *static_cast<float*>(value);
				if (field.Flags & FieldFlagAngle)
				{
					float degrees = glm::degrees(target);
					if (ImGui::DragFloat("##value", &degrees, 0.25f, hasRange ? glm::degrees(field.Min) : 0.0f, hasRange ? glm::degrees(field.Max) : 0.0f, "%.1f°"))
						write(target, glm::radians(degrees), key);
				}
				else
				{
					float edited = target;
					if (ImGui::DragFloat("##value", &edited, DragSpeed(field), hasRange ? field.Min : 0.0f, hasRange ? field.Max : 0.0f, "%.3f",
										 hasRange ? ImGuiSliderFlags_AlwaysClamp : 0))
						write(target, edited, key);
				}
				break;
			}

			case FieldType::Vec2:
			{
				glm::vec2& target = *static_cast<glm::vec2*>(value);
				glm::vec2 edited = target;
				if (ImGui::DragFloat2("##value", glm::value_ptr(edited), 0.02f, hasRange ? field.Min : 0.0f, hasRange ? field.Max : 0.0f, "%.3f", hasRange ? ImGuiSliderFlags_AlwaysClamp : 0))
					write(target, edited, key);
				break;
			}

			case FieldType::Vec3:
			{
				glm::vec3& target = *static_cast<glm::vec3*>(value);
				glm::vec3 edited = target;
				const bool edit = (field.Flags & FieldFlagColor) ? ImGui::ColorEdit3("##value", glm::value_ptr(edited), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
																 : ImGui::DragFloat3("##value", glm::value_ptr(edited), 0.02f, hasRange ? field.Min : 0.0f, hasRange ? field.Max : 0.0f, "%.3f",
																					 hasRange ? ImGuiSliderFlags_AlwaysClamp : 0);
				if (edit)
					write(target, edited, key);
				break;
			}

			case FieldType::Vec4:
			{
				glm::vec4& target = *static_cast<glm::vec4*>(value);
				glm::vec4 edited = target;
				const bool edit = (field.Flags & FieldFlagColor) ? ImGui::ColorEdit4("##value", glm::value_ptr(edited), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)
																 : ImGui::DragFloat4("##value", glm::value_ptr(edited), 0.02f, hasRange ? field.Min : 0.0f, hasRange ? field.Max : 0.0f, "%.3f",
																					 hasRange ? ImGuiSliderFlags_AlwaysClamp : 0);
				if (edit)
					write(target, edited, key);
				break;
			}

			case FieldType::Quat:
			{
				// Edited as Euler angles in degrees; stored as a unit quaternion.
				glm::quat& target = *static_cast<glm::quat*>(value);
				glm::vec3 degrees = glm::degrees(Math::QuatToEuler(target));
				if (ImGui::DragFloat3("##value", glm::value_ptr(degrees), 0.25f, 0.0f, 0.0f, "%.1f°"))
					write(target, glm::normalize(Math::EulerToQuat(glm::radians(degrees))), key);
				break;
			}

			case FieldType::String:
			{
				std::string& target = *static_cast<std::string*>(value);
				std::string edited = target;
				if (ImGui::InputText("##value", &edited))
					write(target, edited, key);
				break;
			}

			case FieldType::Asset:
			{
				UUID& target = *static_cast<UUID*>(value);
				uint64_t handle = target;
				const AssetType type = context.GetAssets().GetAssetType(UUID(handle));
				const std::string label = handle == 0 ? "None" : fmt::format("{} ({})", handle, AssetTypeToString(type));
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputScalar("##value", ImGuiDataType_U64, &handle, nullptr, nullptr, "%llu"))
					write(target, UUID(handle), key);
				if (undoable && ImGui::IsItemActive())
					context.TouchInteractiveEdit(key);
				ImGui::SetItemTooltip("%s", label.c_str());
				// Accept asset handles dragged from the asset browser.
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PARALAX_ASSET"); payload && payload->DataSize == sizeof(uint64_t))
					{
						uint64_t dropped = 0;
						std::memcpy(&dropped, payload->Data, sizeof(dropped));
						writeOnce([&] { target = UUID(dropped); });
					}
					ImGui::EndDragDropTarget();
				}
				continuous = false; // Touched above, before the tooltip and drop target replaced the last item.
				break;
			}

			case FieldType::AssetArray:
			{
				continuous = false;
				auto& handles = *static_cast<std::vector<UUID>*>(value);
				ImGui::TextDisabled("%zu slot(s)", handles.size());
				for (size_t i = 0; i < handles.size(); i++)
				{
					ImGui::PushID(static_cast<int>(i));
					const ImGuiID slotKey = ImGui::GetID("##slot");
					uint64_t handle = handles[i];
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::InputScalar("##slot", ImGuiDataType_U64, &handle, nullptr, nullptr, "%llu"))
						write(handles[i], UUID(handle), slotKey);
					if (undoable && ImGui::IsItemActive())
						context.TouchInteractiveEdit(slotKey);
					ImGui::PopID();
				}
				if (ImGui::SmallButton("+"))
					writeOnce([&] { handles.emplace_back(UUID(0)); });
				ImGui::SameLine();
				if (!handles.empty() && ImGui::SmallButton("-"))
					writeOnce([&] { handles.pop_back(); });
				break;
			}

			case FieldType::Enum:
			{
				continuous = false;
				int& index = *static_cast<int*>(value);
				const char* preview = index >= 0 && static_cast<size_t>(index) < field.EnumNames.size() ? field.EnumNames[static_cast<size_t>(index)].c_str() : "?";
				if (ImGui::BeginCombo("##value", preview))
				{
					for (size_t i = 0; i < field.EnumNames.size(); i++)
					{
						if (ImGui::Selectable(field.EnumNames[i].c_str(), static_cast<size_t>(index) == i) && static_cast<size_t>(index) != i)
							writeOnce([&] { index = static_cast<int>(i); });
					}
					ImGui::EndCombo();
				}
				break;
			}
		}

		if (undoable && continuous && ImGui::IsItemActive())
			context.TouchInteractiveEdit(key);

		if (!field.Description.empty())
			ImGui::SetItemTooltip("%s", field.Description.c_str());
		ImGui::PopID();
		return changed;
	}

	bool DrawFields(EditorContext& context, std::span<const FieldInfo> fields, void* object, const char* undoLabel)
	{
		bool changed = false;
		if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 120.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			for (const FieldInfo& field : fields)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(field.Name.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed |= DrawField(context, field, object, undoLabel);
			}
			ImGui::EndTable();
		}
		return changed;
	}

}
