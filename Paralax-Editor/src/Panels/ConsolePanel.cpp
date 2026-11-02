#include "Panels/ConsolePanel.h"

#include "EditorCore/EditorContext.h"

#include <Paralax/Core/JsonText.h>

#include <imgui.h>
#include <imgui_stdlib.h>

namespace Paralax::Editor {

	ConsolePanel::ConsolePanel(std::shared_ptr<ConsoleSink> sink)
		: m_Sink(std::move(sink))
	{
	}

	void ConsolePanel::Draw(EditorContext& context)
	{
		ImGui::Begin("Console");

		// Through the command, as agents clear it.
		if (ImGui::Button("Clear"))
			context.Execute("console.clear");
		ImGui::SameLine();
		ImGui::Checkbox("Verbose", &m_ShowTrace);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputTextWithHint("##filter", "Filter", &m_Filter);

		const float commandHeight = ImGui::GetFrameHeightWithSpacing();
		ImGui::BeginChild("##log", ImVec2(0, -commandHeight), ImGuiChildFlags_Borders);
		const std::string& filter = m_Filter;
		const uint64_t revision = m_Sink->GetRevision();
		const bool updated = revision != m_LastRevision;
		if (updated)
			m_Messages = m_Sink->GetMessages();
		for (const ConsoleSink::Message& message : m_Messages)
		{
			if (!m_ShowTrace && message.Level < spdlog::level::info)
				continue;
			if (!filter.empty() && message.Text.find(filter) == std::string::npos)
				continue;

			ImVec4 color(0.80f, 0.82f, 0.86f, 1.0f);
			if (message.Level >= spdlog::level::err)
				color = ImVec4(0.95f, 0.42f, 0.40f, 1.0f);
			else if (message.Level == spdlog::level::warn)
				color = ImVec4(0.95f, 0.78f, 0.35f, 1.0f);
			else if (message.Level < spdlog::level::info)
				color = ImVec4(0.55f, 0.58f, 0.63f, 1.0f);
			ImGui::TextDisabled("%s", message.Time.c_str());
			ImGui::SameLine();
			ImGui::TextColored(color, "%s", message.Text.c_str());
		}
		if (updated && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
			ImGui::SetScrollHereY(1.0f);
		m_LastRevision = revision;
		ImGui::EndChild();

		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputTextWithHint("##command", "Command, e.g. entity.create {\"name\": \"Box\"}  (help lists commands)", &m_Command,
									 ImGuiInputTextFlags_EnterReturnsTrue))
		{
			const std::string line = m_Command;
			const size_t space = line.find(' ');
			const std::string name = line.substr(0, space);
			const std::string parameters = space == std::string::npos ? std::string() : line.substr(space + 1);
			// Like the agent server, deep nesting is refused on the text before parsing: copying or dumping the
			// parsed value would overflow the stack.
			if (JsonExceedsDepth(parameters, MaxJsonDepth))
			{
				PX_ERROR("Command parameters nest deeper than {} levels", MaxJsonDepth);
			}
			else
			{
				nlohmann::json params = nlohmann::json::object();
				if (space != std::string::npos)
					params = nlohmann::json::parse(parameters, nullptr, false);
				if (params.is_discarded())
				{
					PX_ERROR("Command parameters are not valid JSON");
				}
				else if (!name.empty())
				{
					const CommandResult result = context.Execute(name, params);
					if (result.Success)
						PX_INFO("{} -> {}", name, result.Result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
					else
						PX_ERROR("{} failed: {}", name, result.Error);
				}
			}
			m_Command.clear();
			ImGui::SetKeyboardFocusHere(-1);
		}

		ImGui::End();
	}

}
