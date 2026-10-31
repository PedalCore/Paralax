#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

#include <Paralax/Scene/Entity.h>

#include <stdexcept>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	CommandDescriptor Echo(std::string name, std::vector<CommandParameter> parameters)
	{
		return {std::move(name), "Returns its parameters.", std::move(parameters), false,
				[](EditorContext&, const json& params) { return CommandResult::Ok(params); }};
	}

}

TEST_CASE("CommandRegistry: parameters are checked against their declared JSON types")
{
	EditorContext context;
	CommandRegistry& registry = context.GetCommands();
	registry.Register(Echo("test.types", {{"i", "integer", "", false}, {"n", "number", "", false}, {"b", "boolean", "", false}, {"s", "string", "", false}, {"o", "object", "", false}, {"a", "array", "", false}, {"x", "any", "", false}}));

	CHECK(context.Execute("test.types", {{"i", 3}, {"n", 2.5}, {"b", true}, {"s", "t"}, {"o", json::object()}, {"a", json::array()}, {"x", nullptr}}).Success);
	CHECK(context.Execute("test.types", {{"n", 2}}).Success); // Integers are numbers.
	CHECK_FALSE(context.Execute("test.types", {{"i", 2.5}}).Success);
	CHECK_FALSE(context.Execute("test.types", {{"b", 1}}).Success);
	CHECK_FALSE(context.Execute("test.types", {{"s", 1}}).Success);
	CHECK_FALSE(context.Execute("test.types", {{"o", json::array()}}).Success);
	CHECK_FALSE(context.Execute("test.types", {{"a", json::object()}}).Success);
	CHECK(context.Execute("test.types", {{"x", json::array({1, "two"})}}).Success);
}

TEST_CASE("CommandRegistry: a throwing handler fails the command and rolls back its changes")
{
	EditorContext context;
	context.GetCommands().Register({"test.throw", "Creates an entity, then throws.", {}, true, [](EditorContext& c, const json&) -> CommandResult {
										c.GetActiveScene().CreateEntity("Doomed");
										throw std::runtime_error("boom");
									}});
	const CommandResult result = context.Execute("test.throw");
	CHECK_FALSE(result.Success);
	CHECK(result.Error.find("boom") != std::string::npos);
	CHECK(context.GetEditorScene().GetEntityCount() == 0);
	CHECK_FALSE(context.IsChangeOpen());
	CHECK_FALSE(context.CanUndo());
}

TEST_CASE("CommandRegistry: commands can be unregistered and described")
{
	EditorContext context;
	CommandRegistry& registry = context.GetCommands();
	registry.Register(Echo("test.temporary", {{"value", "string", "A value.", true}}));
	const json described = registry.Describe();
	bool found = false;
	for (const json& command : described)
	{
		if (command["name"] == "test.temporary")
		{
			found = true;
			CHECK(command["parameters"][0]["name"] == "value");
			CHECK(command["parameters"][0]["required"] == true);
			CHECK(command["undoable"] == false);
		}
	}
	CHECK(found);

	CHECK(registry.Unregister("test.temporary"));
	CHECK_FALSE(registry.Unregister("test.temporary"));
	CHECK(registry.Find("test.temporary") == nullptr);
	CHECK_FALSE(context.Execute("test.temporary", {{"value", "x"}}).Success);
}

TEST_CASE("CommandRegistry: duplicate or incomplete registrations fail verification")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	EditorContext context;
	CommandRegistry& registry = context.GetCommands();
	CHECK_THROWS_AS(registry.Register(Echo("entity.create", {})), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(registry.Register(Echo("", {})), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(registry.Register({"test.nohandler", "", {}, false, nullptr}), Paralax::Testing::AssertionFailedError);
}
