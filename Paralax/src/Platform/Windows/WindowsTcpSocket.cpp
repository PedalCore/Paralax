#include "pxpch.h"
#include "Paralax/Network/TcpSocket.h"

#if defined(PX_PLATFORM_WINDOWS)

	#include <WinSock2.h>
	#include <WS2tcpip.h>

namespace Paralax {

	namespace {

		SOCKET ToSocket(intptr_t handle)
		{
			return static_cast<SOCKET>(handle);
		}

		bool EnsureWinsock()
		{
			static const bool s_Initialized = [] {
				WSADATA data;
				const int result = WSAStartup(MAKEWORD(2, 2), &data);
				if (result != 0)
					PX_CORE_ERROR("WSAStartup failed (error {})", result);
				return result == 0;
			}();
			return s_Initialized;
		}

		// Creates a TCP socket that child processes (e.g. a launched game) do not inherit.
		SOCKET CreateSocket()
		{
			return ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
		}

		void SetNonBlocking(SOCKET s, bool enabled)
		{
			u_long mode = enabled ? 1 : 0;
			::ioctlsocket(s, FIONBIO, &mode);
		}

		sockaddr_in LoopbackAddress(uint16_t port)
		{
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			return address;
		}

	}

	TcpSocket::~TcpSocket()
	{
		Close();
	}

	TcpSocket::TcpSocket(TcpSocket&& other) noexcept
		: m_Handle(std::exchange(other.m_Handle, InvalidHandle))
	{
	}

	TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			m_Handle = std::exchange(other.m_Handle, InvalidHandle);
		}
		return *this;
	}

	bool TcpSocket::Listen(uint16_t port)
	{
		Close();
		if (!EnsureWinsock())
			return false;
		const SOCKET s = CreateSocket();
		if (s == INVALID_SOCKET)
		{
			PX_CORE_ERROR("socket() failed (WSA error {})", WSAGetLastError());
			return false;
		}
		// Exclusive use: another process must not be able to steal the port.
		const BOOL exclusive = TRUE;
		::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
		sockaddr_in address = LoopbackAddress(port);
		if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(s, 8) != 0)
		{
			PX_CORE_ERROR("Cannot listen on 127.0.0.1:{} (WSA error {})", port, WSAGetLastError());
			::closesocket(s);
			return false;
		}
		// Non-blocking, so a connection that is reset between WaitReadable and Accept cannot block Accept forever.
		SetNonBlocking(s, true);
		m_Handle = static_cast<intptr_t>(s);
		return true;
	}

	uint16_t TcpSocket::GetPort() const
	{
		sockaddr_in address{};
		int length = sizeof(address);
		if (!IsValid() || ::getsockname(ToSocket(m_Handle), reinterpret_cast<sockaddr*>(&address), &length) != 0)
			return 0;
		return ntohs(address.sin_port);
	}

	TcpSocket TcpSocket::Accept()
	{
		if (!IsValid())
			return TcpSocket();
		const SOCKET s = ::accept(ToSocket(m_Handle), nullptr, nullptr);
		if (s == INVALID_SOCKET)
			return TcpSocket();
		// Accepted sockets inherit the listener's non-blocking mode; connections block.
		SetNonBlocking(s, false);
		::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
		const BOOL noDelay = TRUE;
		::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
		// A peer that stops reading must not block the server thread forever.
		const DWORD sendTimeoutMilliseconds = 10000;
		::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMilliseconds), sizeof(sendTimeoutMilliseconds));
		return TcpSocket(static_cast<intptr_t>(s));
	}

	bool TcpSocket::Connect(uint16_t port)
	{
		Close();
		if (!EnsureWinsock())
			return false;
		const SOCKET s = CreateSocket();
		if (s == INVALID_SOCKET)
			return false;
		sockaddr_in address = LoopbackAddress(port);
		if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			::closesocket(s);
			return false;
		}
		m_Handle = static_cast<intptr_t>(s);
		return true;
	}

	bool TcpSocket::SendAll(const std::string& data)
	{
		if (!IsValid())
			return false;
		size_t sent = 0;
		while (sent < data.size())
		{
			const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, 1 << 20));
			const int result = ::send(ToSocket(m_Handle), data.data() + sent, chunk, 0);
			if (result <= 0)
				return false;
			sent += static_cast<size_t>(result);
		}
		return true;
	}

	std::optional<std::string> TcpSocket::Receive(size_t maxBytes)
	{
		if (!IsValid())
			return std::nullopt;
		std::string buffer(maxBytes, '\0');
		const int result = ::recv(ToSocket(m_Handle), buffer.data(), static_cast<int>(std::min<size_t>(maxBytes, INT32_MAX)), 0);
		if (result <= 0)
			return std::nullopt;
		buffer.resize(static_cast<size_t>(result));
		return buffer;
	}

	TcpSocket::WaitResult TcpSocket::WaitReadable(int timeoutMilliseconds) const
	{
		if (!IsValid())
			return WaitResult::Error;
		WSAPOLLFD descriptor{};
		descriptor.fd = ToSocket(m_Handle);
		descriptor.events = POLLRDNORM;
		const int result = ::WSAPoll(&descriptor, 1, timeoutMilliseconds);
		if (result == 0)
			return WaitResult::Timeout;
		if (result < 0 || (descriptor.revents & POLLNVAL) != 0)
			return WaitResult::Error;
		return WaitResult::Readable;
	}

	void TcpSocket::Shutdown() const
	{
		if (IsValid())
			::shutdown(ToSocket(m_Handle), SD_BOTH);
	}

	void TcpSocket::Close()
	{
		if (m_Handle != InvalidHandle)
		{
			::closesocket(ToSocket(m_Handle));
			m_Handle = InvalidHandle;
		}
	}

}

#endif
