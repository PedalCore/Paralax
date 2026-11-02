#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/AgentServer.h>
#include <EditorCore/ConsoleSink.h>
#include <EditorCore/EditorContext.h>

#include <string>
#include <vector>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	// An editor context with the console commands over a sink attached to the engine log, as the editor has them.
	struct ConsoleFixture
	{
		explicit ConsoleFixture(size_t capacity = 2000)
			: Sink(std::make_shared<ConsoleSink>(capacity))
		{
			RegisterConsoleCommands(Context.GetCommands(), Sink);
			Log::AddSink(Sink);
		}
		~ConsoleFixture() { Log::RemoveSink(Sink); }

		ConsoleFixture(const ConsoleFixture&) = delete;
		ConsoleFixture& operator=(const ConsoleFixture&) = delete;

		json Get(json params = json::object())
		{
			const CommandResult result = Context.Execute("console.get", params);
			REQUIRE_MESSAGE(result.Success, result.Error);
			return result.Result;
		}

		std::shared_ptr<ConsoleSink> Sink;
		EditorContext Context;
	};

	std::vector<std::string> Texts(const json& result)
	{
		std::vector<std::string> texts;
		for (const json& message : result["messages"])
			texts.push_back(message["text"].get<std::string>());
		return texts;
	}

}

TEST_CASE("ConsoleCommands: console.get returns the latest log messages, filtered by level and revision")
{
	ConsoleFixture fixture;
	const uint64_t start = fixture.Get()["revision"].get<uint64_t>();
	PX_CORE_TRACE("trace line");
	PX_CORE_INFO("info line");
	PX_CORE_WARN("warning line");
	PX_ERROR("error line"); // The client logger too.

	// Info and above by default, oldest first, each with its level, logger, time and revision.
	const json all = fixture.Get({{"sinceRevision", start}});
	CHECK(Texts(all) == std::vector<std::string>{"info line", "warning line", "error line"});
	CHECK(all["messages"][1]["level"] == "warning");
	CHECK(all["messages"][2]["level"] == "error");
	CHECK(all["messages"][0]["logger"] != all["messages"][2]["logger"]);
	CHECK_FALSE(all["messages"][0]["time"].get<std::string>().empty());
	CHECK(all["messages"][0]["revision"].get<uint64_t>() < all["messages"][2]["revision"].get<uint64_t>());
	CHECK(all["revision"].get<uint64_t>() == start + 4);
	CHECK(all["omitted"] == 0);

	CHECK(Texts(fixture.Get({{"sinceRevision", start}, {"minLevel", "trace"}})).size() == 4);
	CHECK(Texts(fixture.Get({{"sinceRevision", start}, {"minLevel", "warn"}})) == std::vector<std::string>{"warning line", "error line"});
	CHECK(Texts(fixture.Get({{"sinceRevision", start}, {"minLevel", "error"}})) == std::vector<std::string>{"error line"});

	// The limit keeps the most recent and says how many older ones it left out.
	const json limited = fixture.Get({{"sinceRevision", start}, {"limit", 2}});
	CHECK(Texts(limited) == std::vector<std::string>{"warning line", "error line"});
	CHECK(limited["omitted"] == 1);

	// From a revision on: only what was logged since.
	const uint64_t before = all["revision"].get<uint64_t>();
	PX_CORE_ERROR("later line");
	CHECK(Texts(fixture.Get({{"sinceRevision", before}})) == std::vector<std::string>{"later line"});
	CHECK(fixture.Get({{"sinceRevision", fixture.Get()["revision"]}})["messages"].empty());
}

TEST_CASE("ConsoleCommands: console.get rejects invalid parameters")
{
	ConsoleFixture fixture(50);
	for (const json& params : {json{{"limit", 0}}, json{{"limit", -1}}, json{{"limit", 51}}, json{{"limit", 1.5}}, json{{"limit", "10"}}, json{{"minLevel", "loud"}},
							   json{{"minLevel", 3}}, json{{"sinceRevision", -1}}, json{{"sinceRevision", "0"}}, json{{"tail", true}}})
	{
		CAPTURE(params.dump());
		CHECK_FALSE(fixture.Context.Execute("console.get", params).Success);
	}
	CHECK(fixture.Context.Execute("console.get", {{"limit", 50}}).Success);
}

TEST_CASE("ConsoleCommands: the console keeps its most recent messages, and console.clear empties it")
{
	ConsoleFixture fixture(3);
	for (int i = 0; i < 5; i++)
		PX_CORE_INFO("message {}", i);
	const json kept = fixture.Get({{"minLevel", "trace"}});
	CHECK(Texts(kept) == std::vector<std::string>{"message 2", "message 3", "message 4"});

	const CommandResult cleared = fixture.Context.Execute("console.clear");
	REQUIRE(cleared.Success);
	CHECK(cleared.Result["revision"].get<uint64_t>() > kept["revision"].get<uint64_t>());
	CHECK(fixture.Get({{"minLevel", "trace"}})["messages"].empty());
	CHECK(fixture.Sink->GetMessages().empty());
	PX_CORE_INFO("after clearing");
	CHECK(Texts(fixture.Get()) == std::vector<std::string>{"after clearing"});
}

TEST_CASE("ConsoleCommands: an agent reads why a command failed over JSON-RPC")
{
	ConsoleFixture fixture;
	size_t executed = 0;
	auto call = [&](const std::string& method, json params) {
		return AgentServer::HandleRequest(fixture.Context, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(params)}}, executed);
	};
	const uint64_t revision = call("console.get", json::object())["result"]["revision"].get<uint64_t>();

	// The error only points at the log; the log names the problem.
	const json failed = call("scene.open", {{"path", "Missing.pxscene"}});
	REQUIRE(failed.contains("error"));
	CHECK(failed["error"]["message"].get<std::string>().find("console.get") != std::string::npos);
	const json log = call("console.get", {{"sinceRevision", revision}, {"minLevel", "error"}})["result"];
	REQUIRE(log["messages"].size() == 1);
	CHECK(log["messages"][0]["text"].get<std::string>().find("Missing.pxscene") != std::string::npos);
}

TEST_CASE("ConsoleCommands: registering without a sink is an asserted precondition")
{
	EditorContext context;
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(RegisterConsoleCommands(context.GetCommands(), nullptr), Paralax::Testing::AssertionFailedError);
	CHECK_FALSE(context.GetCommands().Find("console.get"));
}
