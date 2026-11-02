#pragma once

#include <Paralax/Core/Base.h>
#include <Paralax/Core/JsonText.h>
#include <Paralax/Network/TcpSocket.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Paralax::Editor {

	class EditorContext;

	struct AgentServerSpecification
	{
		uint16_t Port = 0; // 0 picks a free port; read it back with GetPort().
		std::string Token; // Shared secret clients must present; empty generates a random one.
		uint32_t MaxConnections = 4;
		size_t MaxMessageBytes = 16 * 1024 * 1024; // Per request line, once authenticated (4 KB before).
		// A connection that has not authenticated within this time is closed.
		std::chrono::milliseconds AuthenticationTimeout{10000};
		// Called on a network thread whenever a request is queued, e.g. to wake the main thread so requests are
		// processed even while the window is minimised. Must be thread-safe; may be empty.
		std::function<void()> OnRequestQueued;
	};

	// JSON-RPC 2.0 server on 127.0.0.1 through which agents drive the editor. Each connection sends one request per
	// line ('\n'-terminated) and receives one response line per request, in order. The first request must be
	// "session.authenticate" with {"token": "..."} within the authentication timeout; anything else (or a wrong token) closes the
	// connection. Every other method is an editor command (see "help"), executed on the main thread by
	// ProcessRequests. JSON-RPC batches (arrays, up to MaxBatchSize) run in one main-thread pass. Requests nested
	// deeper than MaxJsonDepth are refused.
	//
	// Thread-safety: network I/O runs on internal threads, which exist from Run() on. Listen, Start, Run,
	// ProcessRequests and destruction are main-thread-only. Destruction stops the server, failing any queued requests.
	class AgentServer
	{
	public:
		// Binds the port and prepares the token, but accepts no connection until Run(): no thread of the server runs
		// before then, so the owner can finish main-thread-only setup (installing log sinks, say) first. Clients that
		// connect in between wait in the listen backlog. Returns nullptr (error logged) if the port cannot be bound or
		// no token can be generated.
		static Scope<AgentServer> Listen(const AgentServerSpecification& specification);
		// Starts accepting connections on an internal thread. Returns false (error logged) if the thread cannot be
		// created; the server then serves nobody and is destroyed as usual. Calling it again does nothing.
		bool Run();
		// Listen() and Run() in one step, for owners with nothing to set up in between.
		static Scope<AgentServer> Start(const AgentServerSpecification& specification);
		~AgentServer();

		AgentServer(const AgentServer&) = delete;
		AgentServer& operator=(const AgentServer&) = delete;

		uint16_t GetPort() const { return m_Port; }
		const std::string& GetToken() const { return m_Token; }

		// Executes queued requests against the context and hands responses back to their connections. Returns the
		// number of requests (batch entries count individually) that were executed.
		size_t ProcessRequests(EditorContext& context);

		// Executes one parsed JSON-RPC request or batch and returns the response (null for notifications only).
		// Exposed for tests; does not perform authentication.
		static nlohmann::json HandleRequest(EditorContext& context, const nlohmann::json& request, size_t& executed);

		// JSON-RPC error codes.
		static constexpr int ParseError = -32700;
		static constexpr int InvalidRequest = -32600;
		static constexpr int MethodNotFound = -32601;
		static constexpr int CommandFailed = -32000;
		static constexpr int NotAuthenticated = -32001;
		static constexpr int ServerStopping = -32002;

		// The engine-wide limit, shared with the file loaders.
		static constexpr int MaxJsonDepth = Paralax::MaxJsonDepth;
		static constexpr size_t MaxBatchSize = 256;

	private:
		AgentServer() = default;

		struct PendingRequest
		{
			nlohmann::json Request;
			std::promise<nlohmann::json> Response;
		};

		struct Connection
		{
			// Used by the connection thread; the destructor only calls Shutdown() on it (thread-safe).
			TcpSocket Socket;
			std::thread Thread;
			std::atomic<bool> Finished = false;
		};

		void AcceptLoop();
		void ServeConnection(TcpSocket& socket);
		// Called on connection threads. Returns the response to send, or null if none is due.
		nlohmann::json Dispatch(nlohmann::json request, bool& authenticated);
		void ReapFinishedConnections();

		static nlohmann::json MakeError(const nlohmann::json& id, int code, std::string_view message);

	private:
		AgentServerSpecification m_Specification;
		TcpSocket m_Listener;
		uint16_t m_Port = 0;
		std::string m_Token;

		std::atomic<bool> m_Stopping = false;
		std::thread m_AcceptThread;
		std::mutex m_ConnectionsMutex; // Guards m_Connections (accept thread and destructor).
		std::vector<Scope<Connection>> m_Connections;

		std::mutex m_QueueMutex; // Guards m_Queue and, with m_Stopping, whether new requests may be queued.
		std::deque<Scope<PendingRequest>> m_Queue;
	};

}
