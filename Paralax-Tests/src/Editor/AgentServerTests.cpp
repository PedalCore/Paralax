#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/AgentServer.h>
#include <EditorCore/EditorContext.h>

#include <Paralax/Network/TcpSocket.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	json Request(const json& id, const std::string& method, json params = json::object())
	{
		return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
	}

	int ErrorCode(const json& response)
	{
		return response.contains("error") ? response["error"]["code"].get<int>() : 0;
	}

	// A test agent: sends one line and waits for the response line while the main thread pumps the server.
	class TestClient
	{
	public:
		explicit TestClient(uint16_t port)
		{
			REQUIRE(m_Socket.Connect(port));
		}

		std::future<json> SendAsync(std::string line)
		{
			return std::async(std::launch::async, [this, line = std::move(line)] {
				if (!m_Socket.SendAll(line + "\n"))
					return json("send failed");
				// A deadline keeps a failing test from hanging: the future's destructor waits for this thread.
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
				std::optional<std::string> response = m_Reader.ReadLine([deadline] { return std::chrono::steady_clock::now() > deadline; });
				return response ? json::parse(*response, nullptr, false) : json("disconnected");
			});
		}

		std::future<json> SendAsync(const json& request) { return SendAsync(request.dump()); }

		TcpSocket& GetSocket() { return m_Socket; }

	private:
		TcpSocket m_Socket;
		LineReader m_Reader{m_Socket};
	};

	// Pumps ProcessRequests until the response arrives (or a generous timeout passes).
	json Await(std::future<json>& response, AgentServer& server, EditorContext& context)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (response.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
		{
			server.ProcessRequests(context);
			if (std::chrono::steady_clock::now() > deadline)
				FAIL("timed out waiting for the agent server");
		}
		return response.get();
	}

	json Call(TestClient& client, AgentServer& server, EditorContext& context, const json& request)
	{
		std::future<json> response = client.SendAsync(request);
		return Await(response, server, context);
	}

	void Authenticate(TestClient& client, AgentServer& server, EditorContext& context)
	{
		const json response = Call(client, server, context, Request(0, "session.authenticate", {{"token", server.GetToken()}}));
		REQUIRE(response["result"]["authenticated"] == true);
	}

}

TEST_CASE("AgentServer: requests map to editor commands with JSON-RPC results and errors")
{
	EditorContext context;
	size_t executed = 0;

	const json created = AgentServer::HandleRequest(context, Request(1, "entity.create", {{"name", "Agent"}}), executed);
	CHECK(created["jsonrpc"] == "2.0");
	CHECK(created["id"] == 1);
	REQUIRE(created["result"].contains("id"));
	CHECK(executed == 1);
	CHECK(context.GetEditorScene().GetEntityCount() == 1);

	const json unknown = AgentServer::HandleRequest(context, Request("a", "no.such.command"), executed);
	CHECK(ErrorCode(unknown) == AgentServer::MethodNotFound);
	CHECK(unknown["id"] == "a");

	const json failed = AgentServer::HandleRequest(context, Request(2, "entity.rename", {{"id", "1"}}), executed);
	CHECK(ErrorCode(failed) == AgentServer::CommandFailed);
	CHECK(failed["error"]["message"].get<std::string>().find("name") != std::string::npos);

	// Omitted params mean no parameters.
	const json info = AgentServer::HandleRequest(context, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "scene.info"}}, executed);
	CHECK(info.contains("result"));
}

TEST_CASE("AgentServer: an error quotes a refused method or value briefly, whatever its size")
{
	EditorContext context;
	size_t executed = 0;
	const std::string huge(16u << 20, 'x');

	const json unknown = AgentServer::HandleRequest(context, Request(1, huge), executed);
	CHECK(ErrorCode(unknown) == AgentServer::MethodNotFound);
	const std::string unknownMessage = unknown["error"]["message"].get<std::string>();
	CHECK(unknownMessage.size() < 256);
	CHECK_MESSAGE(unknownMessage.starts_with("Unknown command 'xxxx"), unknownMessage);
	CHECK_MESSAGE(unknownMessage.find("...' (16777216 bytes)") != std::string::npos, unknownMessage);
	CHECK(executed == 0);

	// A 16 MiB value refused by a field comes back as a short error naming the field.
	const json created = AgentServer::HandleRequest(context, Request(2, "entity.create", {{"name", "A"}, {"components", {{"PointLight", {{"Intensity", huge}}}}}}), executed);
	CHECK(ErrorCode(created) == AgentServer::CommandFailed);
	const std::string createdMessage = created["error"]["message"].get<std::string>();
	CHECK(createdMessage.size() < 256);
	CHECK_MESSAGE(createdMessage.starts_with("Field 'Intensity' expects"), createdMessage);
	CHECK_MESSAGE(createdMessage.ends_with("... (string of 16777216 bytes)"), createdMessage);
}

TEST_CASE("AgentServer: malformed requests are rejected without running anything")
{
	EditorContext context;
	size_t executed = 0;
	CHECK(ErrorCode(AgentServer::HandleRequest(context, json(5), executed)) == AgentServer::InvalidRequest);
	CHECK(ErrorCode(AgentServer::HandleRequest(context, {{"id", 1}, {"method", "help"}}, executed)) == AgentServer::InvalidRequest); // no version
	CHECK(ErrorCode(AgentServer::HandleRequest(context, {{"jsonrpc", "1.0"}, {"id", 1}, {"method", "help"}}, executed)) == AgentServer::InvalidRequest);
	CHECK(ErrorCode(AgentServer::HandleRequest(context, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", 7}}, executed)) == AgentServer::InvalidRequest);
	CHECK(ErrorCode(AgentServer::HandleRequest(context, {{"jsonrpc", "2.0"}, {"id", json::object()}, {"method", "help"}}, executed)) == AgentServer::InvalidRequest);
	CHECK(ErrorCode(AgentServer::HandleRequest(context, Request(1, "entity.create", json::array()), executed)) == AgentServer::CommandFailed);
	CHECK(ErrorCode(AgentServer::HandleRequest(context, json::array(), executed)) == AgentServer::InvalidRequest);
	CHECK(executed == 1); // Only the entity.create with bad params reached the registry, which rejected it.
	CHECK(context.GetEditorScene().GetEntityCount() == 0);
}

TEST_CASE("AgentServer: batches run in order, notifications get no response, nested batches are refused")
{
	EditorContext context;
	size_t executed = 0;
	const json notification = {{"jsonrpc", "2.0"}, {"method", "entity.create"}, {"params", {{"name", "Silent"}}}};
	const json batch = json::array({Request(1, "entity.create", {{"name", "A"}}), notification, json::array({Request(9, "help")}), Request(2, "scene.info")});

	const json responses = AgentServer::HandleRequest(context, batch, executed);
	REQUIRE(responses.is_array());
	REQUIRE(responses.size() == 3);
	CHECK(responses[0]["id"] == 1);
	CHECK(ErrorCode(responses[1]) == AgentServer::InvalidRequest);
	CHECK(responses[2]["id"] == 2);
	CHECK(responses[2]["result"]["entities"] == 2);
	CHECK(executed == 3);

	CHECK(AgentServer::HandleRequest(context, notification, executed).is_null());
	CHECK(AgentServer::HandleRequest(context, json::array({notification}), executed).is_null());
	CHECK(context.GetEditorScene().GetEntityCount() == 4);

	// An invalid request is answered even without an id.
	const json invalid = AgentServer::HandleRequest(context, {{"method", "help"}}, executed);
	CHECK(ErrorCode(invalid) == AgentServer::InvalidRequest);
	CHECK(invalid["id"].is_null());

	// Batches are capped.
	const json tooLarge = AgentServer::HandleRequest(context, json(json::array_t(AgentServer::MaxBatchSize + 1, Request(1, "scene.info"))), executed);
	CHECK(ErrorCode(tooLarge) == AgentServer::InvalidRequest);
}

TEST_CASE("AgentServer: connections must authenticate with the token first")
{
	EditorContext context;
	Scope<AgentServer> server = AgentServer::Start({});
	REQUIRE(server);
	CHECK(server->GetPort() != 0);
	CHECK(server->GetToken().size() == 32);

	// Each failed attempt is answered, then the connection is closed.
	const json attempts[] = {Request(1, "entity.create"), Request(2, "session.authenticate", {{"token", "wrong"}}),
							 Request(3, "session.authenticate", {{"token", 5}}), Request(4, "session.authenticate"), json::array({Request(5, "help")})};
	for (const json& attempt : attempts)
	{
		CAPTURE(attempt.dump());
		TestClient client(server->GetPort());
		CHECK(ErrorCode(Call(client, *server, context, attempt)) == AgentServer::NotAuthenticated);
		const json after = Call(client, *server, context, Request(9, "help"));
		CHECK((after == json("disconnected") || after == json("send failed")));
	}
	CHECK(context.GetEditorScene().GetEntityCount() == 0);

	TestClient client(server->GetPort());
	Authenticate(client, *server, context);
	const json created = Call(client, *server, context, Request(6, "entity.create", {{"name", "Remote"}}));
	CHECK(created["id"] == 6);
	CHECK(created["result"].contains("id"));
	CHECK(context.GetEditorScene().GetEntityCount() == 1);

	// Authentication is per connection.
	TestClient other(server->GetPort());
	CHECK(ErrorCode(Call(other, *server, context, Request(7, "help"))) == AgentServer::NotAuthenticated);
}

TEST_CASE("AgentServer: unauthenticated connections are closed after the timeout and may only send small messages")
{
	AgentServerSpecification specification;
	specification.AuthenticationTimeout = std::chrono::milliseconds(200);
	Scope<AgentServer> server = AgentServer::Start(specification);
	REQUIRE(server);
	EditorContext context;

	TcpSocket idle;
	REQUIRE(idle.Connect(server->GetPort()));
	LineReader reader(idle);
	CHECK_FALSE(reader.ReadLine()); // Closed by the server without a word.

	TestClient large(server->GetPort());
	std::future<json> response = large.SendAsync(std::string(8000, ' ') + "{}");
	const json result = Await(response, *server, context);
	CHECK((result == json("disconnected") || result == json("send failed")));
}

TEST_CASE("AgentServer: an explicit token is used as given")
{
	AgentServerSpecification specification;
	specification.Token = "secret";
	Scope<AgentServer> server = AgentServer::Start(specification);
	REQUIRE(server);
	CHECK(server->GetToken() == "secret");

	// The port is now taken.
	specification.Port = server->GetPort();
	CHECK_FALSE(AgentServer::Start(specification));
}

TEST_CASE("AgentServer: invalid JSON gets a parse error and the connection stays usable")
{
	EditorContext context;
	Scope<AgentServer> server = AgentServer::Start({});
	REQUIRE(server);
	TestClient client(server->GetPort());
	Authenticate(client, *server, context);

	std::future<json> parseError = client.SendAsync(std::string("{not json"));
	CHECK(ErrorCode(Await(parseError, *server, context)) == AgentServer::ParseError);
	std::future<json> invalidUtf8 = client.SendAsync(std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\xff\"}"));
	CHECK(ErrorCode(Await(invalidUtf8, *server, context)) == AgentServer::ParseError);

	// Deep nesting is refused before parsing (copies of deeply nested values would overflow the stack); brackets
	// inside strings do not count.
	const std::string deep = R"({"jsonrpc":"2.0","id":3,"method":"scene.info","params":{"x":)" + std::string(5000, '[') + std::string(5000, ']') + "}}";
	std::future<json> nested = client.SendAsync(deep);
	CHECK(ErrorCode(Await(nested, *server, context)) == AgentServer::InvalidRequest);
	const std::string bracketsInString = R"({"jsonrpc":"2.0","id":4,"method":"entity.create","params":{"name":")" + std::string(200, '[') + R"("}})";
	CHECK(Call(client, *server, context, json::parse(bracketsInString)).contains("result"));

	CHECK(Call(client, *server, context, Request(2, "scene.info")).contains("result"));
}

TEST_CASE("AgentServer: Listen binds the port but serves connections only once Run is called")
{
	// The order the editor relies on: a log sink installed before any server thread exists (sink changes are
	// main-thread-only and must not run concurrently with logging from those threads).
	Paralax::Testing::ScopedLogCapture capture;
	Scope<AgentServer> listening = AgentServer::Listen({});
	REQUIRE(listening);
	CHECK(listening->GetPort() != 0);
	CHECK(listening->GetToken().size() == 32);
	CHECK_FALSE(capture.Contains(spdlog::level::info, "listening"));
	listening.reset(); // Never run: nothing to join.

	Scope<AgentServer> server = AgentServer::Listen({});
	REQUIRE(server);
	EditorContext context;
	// The connection is queued by the OS; nobody answers it yet.
	TestClient client(server->GetPort());
	std::future<json> response = client.SendAsync(Request(0, "session.authenticate", {{"token", server->GetToken()}}));
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
	while (std::chrono::steady_clock::now() < until)
	{
		server->ProcessRequests(context);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(response.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

	REQUIRE(server->Run());
	CHECK(Await(response, *server, context)["result"]["authenticated"] == true);
	CHECK(capture.Contains(spdlog::level::info, "listening"));
	CHECK(server->Run()); // Already running: nothing happens.
	CHECK(Call(client, *server, context, Request(1, "scene.info")).contains("result"));
}

TEST_CASE("AgentServer: queued requests notify the owner so they can be processed promptly")
{
	std::atomic<int> notifications = 0;
	AgentServerSpecification specification;
	specification.OnRequestQueued = [&notifications] { notifications++; };
	Scope<AgentServer> server = AgentServer::Start(specification);
	REQUIRE(server);
	EditorContext context;
	TestClient client(server->GetPort());
	Authenticate(client, *server, context); // Authentication is handled on the network thread: no notification.
	CHECK(notifications == 0);
	CHECK(Call(client, *server, context, Request(1, "scene.info")).contains("result"));
	CHECK(notifications == 1);
}

TEST_CASE("AgentServer: connections over the limit are refused")
{
	AgentServerSpecification specification;
	specification.MaxConnections = 1;
	Scope<AgentServer> server = AgentServer::Start(specification);
	REQUIRE(server);
	EditorContext context;

	TestClient first(server->GetPort());
	Authenticate(first, *server, context); // Ensures the first connection has been accepted.

	TcpSocket second;
	REQUIRE(second.Connect(server->GetPort()));
	LineReader reader(second);
	std::optional<std::string> refusal = reader.ReadLine();
	REQUIRE(refusal);
	CHECK(json::parse(*refusal)["error"]["message"].get<std::string>().find("Too many") != std::string::npos);
	CHECK_FALSE(reader.ReadLine()); // Then closed.
}

TEST_CASE("AgentServer: messages over the size limit close the connection")
{
	AgentServerSpecification specification;
	specification.MaxMessageBytes = 64;
	Scope<AgentServer> server = AgentServer::Start(specification);
	REQUIRE(server);
	EditorContext context;

	TestClient client(server->GetPort());
	std::future<json> response = client.SendAsync(std::string(200000, ' '));
	// Depending on timing the send itself fails once the server has closed the connection.
	const json result = Await(response, *server, context);
	CHECK((result == json("disconnected") || result == json("send failed")));
}

TEST_CASE("AgentServer: stopping answers queued requests and disconnects clients")
{
	EditorContext context;
	Scope<AgentServer> server = AgentServer::Start({});
	REQUIRE(server);
	TestClient client(server->GetPort());
	Authenticate(client, *server, context);

	// Queue a request that is never processed, then stop the server.
	std::future<json> pending = client.SendAsync(Request(1, "scene.info"));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	server.reset();

	REQUIRE(pending.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	const json response = pending.get();
	// The request was either queued (and answered with a shutdown error carrying its id) or still in flight when the
	// connection closed.
	CHECK((ErrorCode(response) == AgentServer::ServerStopping || response == json("disconnected")));
	if (ErrorCode(response) == AgentServer::ServerStopping)
		CHECK(response["id"] == 1);
	CHECK(context.GetEditorScene().GetEntityCount() == 0);
}
