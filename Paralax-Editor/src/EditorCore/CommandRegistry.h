#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Paralax::Editor {

	class EditorContext;

	struct CommandResult
	{
		bool Success = true;
		nlohmann::json Result = nlohmann::json::object();
		std::string Error;

		static CommandResult Ok(nlohmann::json result = nlohmann::json::object())
		{
			CommandResult r;
			r.Result = std::move(result);
			return r;
		}
		static CommandResult Fail(std::string error)
		{
			CommandResult r;
			r.Success = false;
			r.Error = std::move(error);
			return r;
		}
	};

	struct CommandParameter
	{
		std::string Name;
		std::string Type; // JSON type name as documented to agents: "string", "number", "integer", "boolean", "object", "array", "any"
		std::string Description;
		bool Required = true;
	};

	struct CommandDescriptor
	{
		std::string Name; // e.g. "entity.create"
		std::string Description;
		std::vector<CommandParameter> Parameters;
		// Commands that change the scene are recorded for undo (the scene is snapshotted before they run).
		bool ModifiesScene = false;
		std::function<CommandResult(EditorContext&, const nlohmann::json& params)> Handler;
	};

	// The single way to act on the editor. The UI, scripts, the JSON-RPC server (agents) and the command line all
	// execute commands here, so every editor action is scriptable and undoable. Main-thread-only.
	class CommandRegistry
	{
	public:
		// Verifies the name is unique and the handler is set.
		void Register(CommandDescriptor descriptor);
		// Removes a command (used by UI layers that register commands while attached). Returns false if unknown.
		bool Unregister(std::string_view name);

		const CommandDescriptor* Find(std::string_view name) const;
		std::vector<const CommandDescriptor*> GetAll() const;

		// Validates parameters against the descriptor (required parameters present, basic JSON types match, no
		// unknown parameters), then runs the handler. Never throws.
		CommandResult Execute(EditorContext& context, std::string_view name, const nlohmann::json& params) const;

		// Machine-readable description of all commands (names, descriptions, parameters) for agents.
		nlohmann::json Describe() const;

	private:
		std::map<std::string, CommandDescriptor, std::less<>> m_Commands;
	};

}
