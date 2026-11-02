#include "EditorCore/CommandRegistry.h"

#include "EditorCore/EditorContext.h"

#include <Paralax/Core/Base.h>

#include <spdlog/fmt/ranges.h>

#include <string>
#include <vector>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		bool MatchesType(const json& value, const std::string& type)
		{
			if (type == "any")
				return true;
			if (type == "string")
				return value.is_string();
			if (type == "number")
				return value.is_number();
			if (type == "integer")
				return value.is_number_integer();
			if (type == "boolean")
				return value.is_boolean();
			if (type == "object")
				return value.is_object();
			if (type == "array")
				return value.is_array();
			return false;
		}

	}

	void CommandRegistry::Register(CommandDescriptor descriptor)
	{
		PX_CORE_VERIFY(!descriptor.Name.empty() && descriptor.Handler, "Commands need a name and a handler");
		PX_CORE_VERIFY(!m_Commands.contains(descriptor.Name), "Command '{}' is already registered", descriptor.Name);
		std::string name = descriptor.Name;
		m_Commands.emplace(std::move(name), std::move(descriptor));
	}

	bool CommandRegistry::Unregister(std::string_view name)
	{
		auto it = m_Commands.find(name);
		if (it == m_Commands.end())
			return false;
		m_Commands.erase(it);
		return true;
	}

	const CommandDescriptor* CommandRegistry::Find(std::string_view name) const
	{
		auto it = m_Commands.find(name);
		return it == m_Commands.end() ? nullptr : &it->second;
	}

	std::vector<const CommandDescriptor*> CommandRegistry::GetAll() const
	{
		std::vector<const CommandDescriptor*> all;
		for (const auto& [name, descriptor] : m_Commands)
			all.push_back(&descriptor);
		return all;
	}

	CommandResult CommandRegistry::Execute(EditorContext& context, std::string_view name, const json& params) const
	{
		const CommandDescriptor* descriptor = Find(name);
		if (!descriptor)
			return CommandResult::Fail(fmt::format("Unknown command '{}' (use 'help' to list commands)", name));

		const json& arguments = params.is_null() ? json::object() : params;
		if (!arguments.is_object())
			return CommandResult::Fail(fmt::format("'{}': parameters must be a JSON object", name));

		for (const CommandParameter& parameter : descriptor->Parameters)
		{
			auto it = arguments.find(parameter.Name);
			if (it == arguments.end())
			{
				if (parameter.Required)
					return CommandResult::Fail(fmt::format("'{}': missing required parameter '{}'", name, parameter.Name));
				continue;
			}
			if (!MatchesType(*it, parameter.Type))
				return CommandResult::Fail(fmt::format("'{}': parameter '{}' must be of type {}", name, parameter.Name, parameter.Type));
		}
		for (auto it = arguments.begin(); it != arguments.end(); ++it)
		{
			const bool known = std::any_of(descriptor->Parameters.begin(), descriptor->Parameters.end(), [&](const CommandParameter& p) { return p.Name == it.key(); });
			if (!known)
				return CommandResult::Fail(fmt::format("'{}': unknown parameter '{}'", name, it.key()));
		}

		if (descriptor->ModifiesScene)
		{
			// A command is its own undo step (and is rolled back on its own if it fails), never part of a drag the
			// user happens to be in the middle of.
			context.CommitInteractiveEdit();
			context.BeginUndoableChange(descriptor->Name);
		}

		CommandResult result;
		try
		{
			result = descriptor->Handler(context, arguments);
		}
		catch (const std::exception& e)
		{
			// Handlers must not throw; this boundary keeps a buggy command from taking the editor down.
			result = CommandResult::Fail(fmt::format("'{}' failed: {}", name, e.what()));
		}

		if (descriptor->ModifiesScene)
		{
			std::vector<std::string> warnings;
			context.EndUndoableChange(result.Success, &warnings);
			// Rolling back loads the scene as it was; if that dropped a value, the caller must hear of it too.
			if (!warnings.empty())
				result.Error += fmt::format(" (rolling back also dropped a value the scene could not hold: {})", fmt::join(warnings, "; "));
		}
		return result;
	}

	json CommandRegistry::Describe() const
	{
		json commands = json::array();
		for (const auto& [name, descriptor] : m_Commands)
		{
			json parameters = json::array();
			for (const CommandParameter& parameter : descriptor.Parameters)
				parameters.push_back({{"name", parameter.Name}, {"type", parameter.Type}, {"description", parameter.Description}, {"required", parameter.Required}});
			commands.push_back({{"name", name}, {"description", descriptor.Description}, {"parameters", parameters}, {"undoable", descriptor.ModifiesScene}});
		}
		return commands;
	}

}
