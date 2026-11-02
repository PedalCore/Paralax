#include <doctest/doctest.h>

#include "GPUTestContext.h"
#include "TestUtilities.h"

#include <EditorCore/ConsoleSink.h>
#include <EditorCore/EditorContext.h>
#include <EditorLayer.h>

#include <Paralax/Core/Application.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace Paralax;
using namespace Paralax::Editor;

namespace {

	// A command as the reference shows it: "name { a: type, b?: type }", or the bare name without parameters.
	std::string Signature(const CommandDescriptor& command)
	{
		std::string signature = command.Name;
		for (size_t i = 0; i < command.Parameters.size(); i++)
		{
			const CommandParameter& parameter = command.Parameters[i];
			signature += i == 0 ? " { " : ", ";
			signature += parameter.Name + (parameter.Required ? "" : "?") + ": " + parameter.Type;
		}
		if (!command.Parameters.empty())
			signature += " }";
		return signature;
	}

	struct DocumentedCommand
	{
		std::string Signature;
		bool Undoable = false;
		size_t Line = 0;
	};

	constexpr std::string_view ReferenceHeading = "## Command reference";

	// The entries of the command reference: within the section that ReferenceHeading starts and the next "## " heading
	// ends, every line that starts with "- `", its signature being the text up to the next backtick and " (undoable)"
	// right after it marking a command that edits the scene. Other lines (prose, subheadings, continuations, code)
	// are not entries. Repeated names and unterminated signatures are added to problems.
	std::map<std::string, DocumentedCommand> ParseReference(std::string_view markdown, std::vector<std::string>& problems)
	{
		std::map<std::string, DocumentedCommand> entries;
		bool inSection = false;
		bool foundSection = false;
		size_t lineNumber = 0;
		for (size_t start = 0; start < markdown.size();)
		{
			size_t end = markdown.find('\n', start);
			if (end == std::string_view::npos)
				end = markdown.size();
			std::string_view line = markdown.substr(start, end - start);
			start = end + 1;
			lineNumber++;
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);

			if (line == ReferenceHeading)
			{
				inSection = foundSection = true;
				continue;
			}
			if (line.starts_with("## "))
				inSection = false;
			if (!inSection || !line.starts_with("- `"))
				continue;

			const size_t close = line.find('`', 3);
			if (close == std::string_view::npos)
			{
				problems.push_back(fmt::format("line {}: the entry's signature has no closing backtick", lineNumber));
				continue;
			}
			DocumentedCommand entry;
			entry.Signature = std::string(line.substr(3, close - 3));
			entry.Undoable = line.substr(close + 1).starts_with(" (undoable)");
			entry.Line = lineNumber;
			const std::string name = entry.Signature.substr(0, entry.Signature.find(' '));
			if (const auto [it, added] = entries.try_emplace(name, entry); !added)
				problems.push_back(fmt::format("line {}: '{}' is documented again (first on line {})", lineNumber, name, it->second.Line));
		}
		if (!foundSection)
			problems.push_back(fmt::format("there is no '{}' section", ReferenceHeading));
		return entries;
	}

	// Every way the reference and the registry disagree: a command it lacks, an entry for no command (unless the
	// registry is known to be partial), and a signature or undo mark that differs.
	std::vector<std::string> CompareReference(const CommandRegistry& registry, std::string_view markdown, bool registryIsPartial = false)
	{
		std::vector<std::string> problems;
		const std::map<std::string, DocumentedCommand> documented = ParseReference(markdown, problems);
		for (const CommandDescriptor* command : registry.GetAll())
		{
			const auto it = documented.find(command->Name);
			if (it == documented.end())
			{
				problems.push_back(fmt::format("'{}' is missing: add `{}`{}", command->Name, Signature(*command), command->ModifiesScene ? " (undoable)" : ""));
				continue;
			}
			if (it->second.Signature != Signature(*command))
				problems.push_back(fmt::format("line {}: `{}` should be `{}`", it->second.Line, it->second.Signature, Signature(*command)));
			if (it->second.Undoable != command->ModifiesScene)
				problems.push_back(fmt::format("line {}: '{}' {} undoable", it->second.Line, command->Name, command->ModifiesScene ? "is" : "is not"));
		}
		if (!registryIsPartial)
		{
			for (const auto& [name, entry] : documented)
			{
				if (!registry.Find(name))
					problems.push_back(fmt::format("line {}: '{}' is documented but no such command exists", entry.Line, name));
			}
		}
		return problems;
	}

	std::string Join(const std::vector<std::string>& problems)
	{
		std::string text;
		for (const std::string& problem : problems)
			text += "\n  " + problem;
		return text;
	}

	CommandResult DoNothing(EditorContext&, const nlohmann::json&)
	{
		return CommandResult::Ok();
	}

}

TEST_CASE("CommandReference: the check reports missing, stale, repeated and mismatched entries")
{
	CommandRegistry registry;
	registry.Register({"a.plain", "", {}, false, &DoNothing});
	registry.Register({"a.edit", "", {{"x", "string", "", true}, {"y", "integer", "", false}}, true, &DoNothing});
	registry.Register({"a.missing", "", {}, false, &DoNothing});

	const std::string_view correct = "# Title\n"
									 "- `a.outside`: before the section, so not an entry\n"
									 "## Command reference\n"
									 "\n"
									 "Prose about `a.plain`.\n"
									 "### Group\n"
									 "- `a.plain`: does nothing\n"
									 "- `a.edit { x: string, y?: integer }` (undoable): edits\n"
									 "  - `a.nested`: indented, so a continuation and not an entry\n"
									 "- `a.missing`: listed\n"
									 "## Next section\n"
									 "- `a.after`: after the section, so not an entry\n";
	CHECK(CompareReference(registry, correct).empty());
	// Windows line endings read the same.
	std::string crlf;
	for (char c : correct)
		crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
	CHECK(CompareReference(registry, crlf).empty());

	const std::vector<std::string> problems = CompareReference(registry, "## Command reference\n"
																		 "- `a.plain { z: any }`: a parameter it does not take\n"
																		 "- `a.edit { x: string, y: integer }`: y is optional, and the undo mark is missing\n"
																		 "- `a.stale`: no such command\n"
																		 "- `a.plain`: listed twice\n"
																		 "- `a.broken: no closing backtick\n");
	const std::string text = Join(problems);
	CHECK_MESSAGE(problems.size() == 7, text);
	CHECK_MESSAGE(text.find("line 2: `a.plain { z: any }` should be `a.plain`") != std::string::npos, text);
	CHECK_MESSAGE(text.find("line 3: `a.edit { x: string, y: integer }` should be `a.edit { x: string, y?: integer }`") != std::string::npos, text);
	CHECK_MESSAGE(text.find("line 3: 'a.edit' is undoable") != std::string::npos, text);
	CHECK_MESSAGE(text.find("'a.missing' is missing: add `a.missing`") != std::string::npos, text);
	CHECK_MESSAGE(text.find("line 4: 'a.stale' is documented but no such command exists") != std::string::npos, text);
	CHECK_MESSAGE(text.find("line 5: 'a.plain' is documented again (first on line 2)") != std::string::npos, text);
	CHECK_MESSAGE(text.find("line 6: the entry's signature has no closing backtick") != std::string::npos, text);

	// A partial registry does not make the other entries stale; without the section, nothing is documented.
	CHECK(CompareReference(registry, "## Command reference\n- `a.plain`: x\n- `a.edit { x: string, y?: integer }` (undoable): x\n- `a.missing`: x\n- `a.other`: x\n", true).empty());
	const std::vector<std::string> noSection = CompareReference(registry, "# Agents\n- `a.plain`: x\n");
	CHECK(noSection.size() == 4);
	CHECK(Join(noSection).find("there is no '## Command reference' section") != std::string::npos);
}

TEST_CASE("CommandReference: docs/Agents.md documents every command the editor registers, with its parameters")
{
	// Read from the source tree, never through __FILE__ (see Paralax-Tests/CMakeLists.txt).
	const std::string markdown = Paralax::Testing::ReadFileText(std::filesystem::path(PX_SOURCE_ROOT) / "docs" / "Agents.md");
	REQUIRE_FALSE(markdown.empty());

	// Every command the editor registers: its context's, the application's, and the editor layer's while attached.
	// The layer needs a graphics device (only one may exist, so the shared test device is released first). Members
	// destroyed in reverse order: the application first, which detaches the layer while the context is still alive.
	EditorContext context;
	auto sink = std::make_shared<ConsoleSink>();
	RegisterApplicationCommands(context.GetCommands(), sink);
	REQUIRE(context.GetCommands().Find("entity.create"));
	REQUIRE(context.GetCommands().Find("editor.quit"));
	Scope<Application> app;
	Paralax::Testing::ReleaseTestGraphicsDevice();
	const bool withLayer = Paralax::Testing::RequireTestGraphicsDevice();
	Paralax::Testing::ReleaseTestGraphicsDevice();
	if (withLayer)
	{
		ApplicationSpecification specification;
		specification.Window.Headless = true;
		specification.Window.Width = 320;
		specification.Window.Height = 200;
		specification.Graphics.Enabled = true;
		app = CreateScope<Application>(specification);
		REQUIRE(app->IsInitialized());
		app->PushLayer<EditorLayer>(context, sink);
		REQUIRE(context.GetCommands().Find("render.settings.get"));
	}

	// Without the layer its entries cannot be told from stale ones, so only what is registered is checked.
	const std::vector<std::string> problems = CompareReference(context.GetCommands(), markdown, !withLayer);
	CHECK_MESSAGE(problems.empty(), (std::string("docs/Agents.md's command reference is out of date:") + Join(problems)));
}
