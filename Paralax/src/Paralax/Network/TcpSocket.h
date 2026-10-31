#pragma once

#include "Paralax/Core/Base.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace Paralax {

	// Minimal TCP socket for local tooling (the agent control server). Loopback IPv4 only. Movable, not copyable; the
	// socket is closed on destruction and is not inherited by child processes. Not thread-safe: each socket is used by
	// one thread at a time, except Shutdown(), which another thread may call to unblock pending sends and receives.
	// Connections block (use WaitReadable to wait interruptibly); listening sockets never block in Accept.
	class TcpSocket
	{
	public:
		TcpSocket() = default;
		~TcpSocket();

		TcpSocket(const TcpSocket&) = delete;
		TcpSocket& operator=(const TcpSocket&) = delete;
		TcpSocket(TcpSocket&& other) noexcept;
		TcpSocket& operator=(TcpSocket&& other) noexcept;

		// Listens on 127.0.0.1:port (0 = any free port). Returns false (with an error logged) on failure.
		bool Listen(uint16_t port);
		// The bound port of a listening socket.
		uint16_t GetPort() const;
		// Accepts a pending connection (as a blocking socket); returns an invalid socket if none is pending (use
		// WaitReadable first) or on error.
		TcpSocket Accept();
		// Connects to 127.0.0.1:port.
		bool Connect(uint16_t port);

		// Sends all bytes. Returns false if the connection failed.
		bool SendAll(const std::string& data);
		// Receives up to maxBytes; returns an empty optional on disconnect or error.
		std::optional<std::string> Receive(size_t maxBytes = 65536);

		enum class WaitResult
		{
			Readable, // Data (or a pending connection, or end-of-stream) is available.
			Timeout,
			Error
		};
		// Waits until the socket is readable or timeoutMilliseconds pass.
		WaitResult WaitReadable(int timeoutMilliseconds) const;

		// Shuts the connection down in both directions without releasing the handle: blocked sends and receives on
		// other threads return with an error. Safe to call from any thread while the socket is alive.
		void Shutdown() const;
		void Close();
		bool IsValid() const { return m_Handle != InvalidHandle; }

	private:
		using Handle = intptr_t;
		static constexpr Handle InvalidHandle = -1;
		explicit TcpSocket(Handle handle)
			: m_Handle(handle)
		{
		}

		Handle m_Handle = InvalidHandle;
	};

	// Reads newline-delimited messages from a socket, buffering partial lines.
	class LineReader
	{
	public:
		explicit LineReader(TcpSocket& socket, size_t maxLineLength = 16 * 1024 * 1024)
			: m_Socket(socket)
			, m_MaxLineLength(maxLineLength)
		{
		}

		// Returns the next line (without '\n' or a trailing '\r'), or an empty optional when the connection ends,
		// a line exceeds the maximum length, or shouldStop (polled every few milliseconds, may be empty) returns true.
		std::optional<std::string> ReadLine(const std::function<bool()>& shouldStop = {});

		void SetMaxLineLength(size_t maxLineLength) { m_MaxLineLength = maxLineLength; }

	private:
		TcpSocket& m_Socket;
		size_t m_MaxLineLength;
		std::string m_Buffer;
		size_t m_ScanOffset = 0; // Bytes of m_Buffer already known to contain no newline.
	};

}
