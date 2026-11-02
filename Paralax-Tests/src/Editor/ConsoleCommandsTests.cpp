#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/AgentServer.h>
#include <EditorCore/ConsoleSink.h>
#include <EditorCore/EditorContext.h>

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Core/JsonText.h>
#include <Paralax/Scripting/ScriptAsset.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	// An editor context with the console commands over a sink attached to the engine log, as the editor has them.
	struct ConsoleFixture
	{
		explicit ConsoleFixture(size_t capacity = 2000, size_t maxMessageBytes = ConsoleSink::DefaultMaxMessageBytes)
			: Sink(std::make_shared<ConsoleSink>(capacity, maxMessageBytes))
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
	CHECK(all["complete"] == true);

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

	// A revision this log has not reached comes from another one (an earlier editor session): refused, not answered
	// with "nothing since".
	PX_CORE_INFO("one message");
	const uint64_t current = fixture.Get()["revision"].get<uint64_t>();
	CHECK(fixture.Context.Execute("console.get", {{"sinceRevision", current}}).Success);
	const CommandResult foreign = fixture.Context.Execute("console.get", {{"sinceRevision", current + 1}});
	CHECK_FALSE(foreign.Success);
	CHECK(foreign.Error.find("not a revision of this log") != std::string::npos);
}

TEST_CASE("ConsoleCommands: the console keeps its most recent messages, and console.clear empties it")
{
	ConsoleFixture fixture(3);
	const uint64_t start = fixture.Get()["revision"].get<uint64_t>();
	PX_CORE_INFO("message 0");
	PX_CORE_INFO("message 1");
	const uint64_t afterTwo = fixture.Get()["revision"].get<uint64_t>();
	for (int i = 2; i < 5; i++)
		PX_CORE_INFO("message {}", i);
	const json kept = fixture.Get({{"minLevel", "trace"}});
	CHECK(Texts(kept) == std::vector<std::string>{"message 2", "message 3", "message 4"});
	// Messages pushed out are reported as missing to a caller that asked for them, and only to one.
	CHECK(kept["complete"] == false);
	CHECK(fixture.Get({{"sinceRevision", start}})["complete"] == false);
	const json sinceTwo = fixture.Get({{"sinceRevision", afterTwo}});
	CHECK(sinceTwo["complete"] == true);
	CHECK(Texts(sinceTwo).size() == 3);

	const CommandResult cleared = fixture.Context.Execute("console.clear");
	REQUIRE(cleared.Success);
	CHECK(cleared.Result["revision"].get<uint64_t>() > kept["revision"].get<uint64_t>());
	CHECK(fixture.Get()["revision"] == cleared.Result["revision"]); // The clear's own revision, nothing after it.
	CHECK(fixture.Get({{"minLevel", "trace"}})["messages"].empty());
	CHECK(fixture.Sink->GetMessages().empty());
	// Cleared messages are gone too; nothing after the clear is.
	CHECK(fixture.Get({{"sinceRevision", afterTwo}})["complete"] == false);
	CHECK(fixture.Get({{"sinceRevision", cleared.Result["revision"]}})["complete"] == true);
	PX_CORE_INFO("after clearing");
	CHECK(Texts(fixture.Get()) == std::vector<std::string>{"after clearing"});
}

TEST_CASE("ConsoleCommands: each message keeps at most the console's byte limit of its text, cut between characters")
{
	{
		ConsoleFixture fixture;
		CHECK(fixture.Sink->GetMaxMessageBytes() == ConsoleSink::DefaultMaxMessageBytes);
		const uint64_t start = fixture.Get()["revision"].get<uint64_t>();
		// A 16 MiB message (a path from an agent, say) keeps 17 KiB and says how long it was.
		const std::string huge(16 * 1024 * 1024, 'x');
		PX_CORE_INFO("Cannot open '{}'", huge);
		PX_CORE_INFO("short");
		const std::vector<std::string> texts = Texts(fixture.Get({{"sinceRevision", start}}));
		REQUIRE(texts.size() == 2);
		CHECK(texts[0] == "Cannot open '" + std::string(ConsoleSink::DefaultMaxMessageBytes - 13, 'x') + "... (16777230 bytes)");
		CHECK(texts[1] == "short");
		CHECK(fixture.Sink->GetMessages().front().Text.size() < ConsoleSink::DefaultMaxMessageBytes + 32);
	}

	// A character the limit falls inside is left out whole.
	ConsoleFixture fixture(10, 5);
	const uint64_t start = fixture.Get()["revision"].get<uint64_t>();
	PX_CORE_INFO("abcde");
	PX_CORE_INFO("abc\xE6\x97\xA5");
	PX_CORE_INFO("abcd\xC3\xA9");
	CHECK(Texts(fixture.Get({{"sinceRevision", start}})) == std::vector<std::string>{"abcde", "abc... (6 bytes)", "abcd... (6 bytes)"});
}

TEST_CASE("ConsoleCommands: a script's long line and error reach the console as the script system cut them")
{
	// The script system cuts them to MaxMessageBytes; the console keeps enough more that their full sizes survive.
	ConsoleFixture fixture;
	Ref<ScriptAsset> script = ScriptAsset::Create("Loud.lua", "local Loud = {} function Loud:OnCreate() local text = string.rep('a', 16 * 1024 * 1024) print(text) error(text) end return Loud");
	script->Handle = AssetHandle(9002);
	fixture.Context.GetAssets().AddMemoryAsset(script);
	REQUIRE(fixture.Context.Execute("entity.create", {{"components", {{"Script", {{"Script", "9002"}}}}}}).Success);
	const uint64_t start = fixture.Get()["revision"].get<uint64_t>();
	REQUIRE(fixture.Context.Execute("play.start", {{"paused", true}}).Success);
	REQUIRE(fixture.Context.Execute("play.step").Success);
	const std::vector<std::string> texts = Texts(fixture.Get({{"sinceRevision", start}, {"minLevel", "trace"}}));
	REQUIRE(fixture.Context.Execute("play.stop").Success);

	CHECK(std::find(texts.begin(), texts.end(), std::string(MaxMessageBytes, 'a') + "... (16777216 bytes)") != texts.end());
	const auto error = std::find_if(texts.begin(), texts.end(), [](const std::string& text) { return text.starts_with("Script error: "); });
	REQUIRE(error != texts.end());
	CHECK(error->size() < MaxMessageBytes + 64);
	CHECK(error->find("OnCreate: ") != std::string::npos);
	CHECK(error->find("... (16777") != std::string::npos); // The error's own size (16 MiB and its context), not the console's cut.
	for (const std::string& text : texts)
		CHECK(text.size() < MaxMessageBytes + 64);
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
