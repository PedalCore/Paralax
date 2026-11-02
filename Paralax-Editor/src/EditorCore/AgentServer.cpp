#include "EditorCore/AgentServer.h"

#include "EditorCore/EditorContext.h"

#include <Paralax/Core/JsonText.h>
#include <Paralax/Core/Log.h>

#include <chrono>
#include <exception>
#include <optional>
#include <random>
#include <system_error>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		// A random token, or nullopt (error logged) without an OS entropy source: the token grants full control of
		// the editor, so nothing weaker stands in for it.
		std::optional<std::string> GenerateToken()
		{
			try
			{
				std::random_device device;
				std::string token;
				token.reserve(32);
				constexpr std::string_view digits = "0123456789abcdef";
				for (int i = 0; i < 4; i++)
				{
					uint32_t value = device();
					for (int nibble = 0; nibble < 8; nibble++, value >>= 4)
						token.push_back(digits[value & 0xF]);
				}
				return token;
			}
			catch (const std::exception& e)
			{
				PX_CORE_ERROR("Agent server: cannot generate a token, no entropy source is available ({})", e.what());
				return std::nullopt;
			}
		}

		// Compares without an early exit so response timing does not reveal how much of the token matched.
		bool TokensEqual(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			unsigned char difference = 0;
			for (size_t i = 0; i < a.size(); i++)
				difference |= static_cast<unsigned char>(a[i] ^ b[i]);
			return difference == 0;
		}

		bool IsValidId(const json& id)
		{
			return id.is_null() || id.is_string() || id.is_number();
		}

		// Requests without an "id" member are notifications: they run but get no response.
		bool IsNotification(const json& request)
		{
			return request.is_object() && !request.contains("id");
		}

		// The id of a request, if it has a valid one (null otherwise).
		json RequestId(const json& request)
		{
			if (request.is_object())
			{
				if (auto member = request.find("id"); member != request.end() && IsValidId(*member))
					return *member;
			}
			return json();
		}

		std::string Serialize(const json& message)
		{
			// Scene data can contain arbitrary bytes (names read from files); replacing invalid UTF-8 keeps dump()
			// from throwing.
			return message.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
		}

		constexpr int AcceptPollMilliseconds = 50;
		// Before authentication a connection may only send a small request, and must authenticate promptly.
		constexpr size_t UnauthenticatedMaxMessageBytes = 4096;

	}

	Scope<AgentServer> AgentServer::Listen(const AgentServerSpecification& specification)
	{
		Scope<AgentServer> server(new AgentServer());
		server->m_Specification = specification;
		if (specification.Token.empty())
		{
			std::optional<std::string> token = GenerateToken();
			if (!token)
				return nullptr;
			server->m_Token = std::move(*token);
		}
		else
		{
			server->m_Token = specification.Token;
		}
		if (!server->m_Listener.Listen(specification.Port))
			return nullptr;
		server->m_Port = server->m_Listener.GetPort();
		return server;
	}

	bool AgentServer::Run()
	{
		if (m_AcceptThread.joinable())
			return true;
		try
		{
			m_AcceptThread = std::thread([this] { AcceptLoop(); });
		}
		catch (const std::system_error& e)
		{
			// Thread creation failed (resource exhaustion): report it rather than terminate the editor.
			PX_CORE_ERROR("Agent server: cannot start the accept thread: {}", e.what());
			return false;
		}
		PX_CORE_INFO("Agent server listening on 127.0.0.1:{}", m_Port);
		return true;
	}

	Scope<AgentServer> AgentServer::Start(const AgentServerSpecification& specification)
	{
		Scope<AgentServer> server = Listen(specification);
		if (!server || !server->Run())
			return nullptr;
		return server;
	}

	AgentServer::~AgentServer()
	{
		{
			std::lock_guard lock(m_QueueMutex);
			m_Stopping = true;
			// Connection threads may be waiting for these; answer them so they can exit.
			for (Scope<PendingRequest>& pending : m_Queue)
				pending->Response.set_value(MakeError(RequestId(pending->Request), ServerStopping, "The editor is shutting down"));
			m_Queue.clear();
		}
		if (m_AcceptThread.joinable())
			m_AcceptThread.join();
		std::lock_guard lock(m_ConnectionsMutex);
		// Unblocks connection threads stuck sending to (or receiving from) a slow peer.
		for (Scope<Connection>& connection : m_Connections)
			connection->Socket.Shutdown();
		for (Scope<Connection>& connection : m_Connections)
			connection->Thread.join();
		m_Connections.clear();
	}

	void AgentServer::AcceptLoop()
	{
		while (!m_Stopping)
		{
			const TcpSocket::WaitResult wait = m_Listener.WaitReadable(AcceptPollMilliseconds);
			if (wait == TcpSocket::WaitResult::Error)
			{
				PX_CORE_ERROR("Agent server: listening socket failed; no further connections are accepted");
				return;
			}
			if (wait == TcpSocket::WaitResult::Timeout)
				continue;

			TcpSocket client = m_Listener.Accept();
			if (!client.IsValid())
			{
				// Nothing was pending after all, or accepting failed (e.g. out of descriptors): back off briefly rather
				// than spinning on a listener that stays readable.
				std::this_thread::sleep_for(std::chrono::milliseconds(AcceptPollMilliseconds));
				continue;
			}

			ReapFinishedConnections();
			std::lock_guard lock(m_ConnectionsMutex);
			if (m_Stopping)
				return;
			if (m_Connections.size() >= m_Specification.MaxConnections)
			{
				client.SendAll(Serialize(MakeError(nullptr, InvalidRequest, "Too many agent connections")));
				continue; // The client socket closes here.
			}
			auto connection = CreateScope<Connection>();
			connection->Socket = std::move(client);
			Connection* raw = connection.get();
			try
			{
				connection->Thread = std::thread([this, raw] {
					ServeConnection(raw->Socket);
					// The peer sees the end of the stream now; the handle is released when the connection is reaped.
					raw->Socket.Shutdown();
					raw->Finished = true;
				});
			}
			catch (const std::system_error& e)
			{
				// Thread creation failed (resource exhaustion): refuse this client rather than terminate the editor.
				PX_CORE_ERROR("Agent server: cannot start a connection thread: {}", e.what());
				continue;
			}
			m_Connections.push_back(std::move(connection));
		}
	}

	void AgentServer::ReapFinishedConnections()
	{
		std::lock_guard lock(m_ConnectionsMutex);
		std::erase_if(m_Connections, [](Scope<Connection>& connection) {
			if (!connection->Finished)
				return false;
			connection->Thread.join();
			return true;
		});
	}

	void AgentServer::ServeConnection(TcpSocket& socket)
	{
		LineReader reader(socket, std::min(m_Specification.MaxMessageBytes, UnauthenticatedMaxMessageBytes));
		bool authenticated = false;
		const auto authenticationDeadline = std::chrono::steady_clock::now() + m_Specification.AuthenticationTimeout;
		auto shouldStop = [&] { return m_Stopping.load() || (!authenticated && std::chrono::steady_clock::now() > authenticationDeadline); };
		while (std::optional<std::string> line = reader.ReadLine(shouldStop))
		{
			if (line->find_first_not_of(" \t") == std::string::npos)
				continue;

			json response;
			// Refused on the text, before parsing: copying or dumping a deeply nested value would overflow the stack.
			if (JsonExceedsDepth(*line, MaxJsonDepth))
			{
				response = MakeError(nullptr, InvalidRequest, fmt::format("JSON nesting deeper than {} levels", MaxJsonDepth));
			}
			else
			{
				// Parse without exceptions: a discarded value means malformed JSON.
				json request = json::parse(*line, nullptr, false);
				response = request.is_discarded() ? MakeError(nullptr, ParseError, "Invalid JSON") : Dispatch(std::move(request), authenticated);
			}
			if (!response.is_null() && !socket.SendAll(Serialize(response)))
				return;
			if (!authenticated)
				return; // A failed authentication ends the connection.
			reader.SetMaxLineLength(m_Specification.MaxMessageBytes);
		}
	}

	json AgentServer::Dispatch(json request, bool& authenticated)
	{
		if (!authenticated)
		{
			if (!request.is_object())
				return MakeError(nullptr, NotAuthenticated, "Authenticate first: call 'session.authenticate' with {\"token\": ...}");
			auto idMember = request.find("id");
			const json id = idMember != request.end() && IsValidId(*idMember) ? *idMember : json();
			auto method = request.find("method");
			if (method == request.end() || !method->is_string() || *method != "session.authenticate")
				return MakeError(id, NotAuthenticated, "Authenticate first: call 'session.authenticate' with {\"token\": ...}");

			auto params = request.find("params");
			const json* token = nullptr;
			if (params != request.end() && params->is_object())
				if (auto member = params->find("token"); member != params->end())
					token = &*member;
			if (!token || !token->is_string() || !TokensEqual(token->get_ref<const std::string&>(), m_Token))
				return MakeError(id, NotAuthenticated, "Invalid token");

			authenticated = true;
			return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"authenticated", true}}}};
		}

		const json id = RequestId(request);
		auto pending = CreateScope<PendingRequest>();
		pending->Request = std::move(request);
		std::future<json> response = pending->Response.get_future();
		{
			std::lock_guard lock(m_QueueMutex);
			if (m_Stopping)
				return MakeError(id, ServerStopping, "The editor is shutting down");
			m_Queue.push_back(std::move(pending));
		}
		if (m_Specification.OnRequestQueued)
			m_Specification.OnRequestQueued();
		return response.get();
	}

	size_t AgentServer::ProcessRequests(EditorContext& context)
	{
		std::deque<Scope<PendingRequest>> queue;
		{
			std::lock_guard lock(m_QueueMutex);
			queue.swap(m_Queue);
		}
		size_t executed = 0;
		for (Scope<PendingRequest>& pending : queue)
			pending->Response.set_value(HandleRequest(context, pending->Request, executed));
		return executed;
	}

	json AgentServer::HandleRequest(EditorContext& context, const json& request, size_t& executed)
	{
		if (request.is_array())
		{
			if (request.empty())
				return MakeError(nullptr, InvalidRequest, "Empty batch");
			if (request.size() > MaxBatchSize)
				return MakeError(nullptr, InvalidRequest, fmt::format("Batches are limited to {} requests", MaxBatchSize));
			json responses = json::array();
			for (const json& entry : request)
			{
				// Batches do not nest; rejecting them here also bounds the recursion depth.
				json response = entry.is_array() ? MakeError(nullptr, InvalidRequest, "Batches cannot be nested") : HandleRequest(context, entry, executed);
				if (!response.is_null())
					responses.push_back(std::move(response));
			}
			return responses.empty() ? json() : responses;
		}

		if (!request.is_object())
			return MakeError(nullptr, InvalidRequest, "A request must be a JSON object");
		auto idMember = request.find("id");
		if (idMember != request.end() && !IsValidId(*idMember))
			return MakeError(nullptr, InvalidRequest, "'id' must be a string, number or null");
		const json id = idMember == request.end() ? json() : *idMember;

		// Invalid requests are answered even without an id (with a null id), as JSON-RPC requires.
		auto version = request.find("jsonrpc");
		auto method = request.find("method");
		if (version == request.end() || *version != "2.0" || method == request.end() || !method->is_string())
			return MakeError(id, InvalidRequest, "Expected {\"jsonrpc\": \"2.0\", \"method\": string, ...}");
		const bool notification = IsNotification(request);

		const std::string& name = method->get_ref<const std::string&>();
		if (name == "session.authenticate")
			return notification ? json() : json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"authenticated", true}}}};
		if (!context.GetCommands().Find(name))
			return notification ? json() : MakeError(id, MethodNotFound, fmt::format("Unknown command {} (use 'help' to list commands)", QuoteText(name)));

		auto params = request.find("params");
		static const json s_NoParameters = json::object();
		const json& arguments = params == request.end() ? s_NoParameters : *params;
		CommandResult result = context.Execute(name, arguments);
		executed++;
		if (notification)
			return json();
		if (!result.Success)
			return MakeError(id, CommandFailed, result.Error);
		return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result.Result)}};
	}

	json AgentServer::MakeError(const json& id, int code, std::string_view message)
	{
		return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
	}

}
