#include "pxpch.h"
#include "Paralax/Network/TcpSocket.h"

#if !defined(PX_PLATFORM_WINDOWS)

	#include <arpa/inet.h>
	#include <fcntl.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <poll.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <unistd.h>

	#include <cerrno>
	#include <cstring>

namespace Paralax {

	namespace {

		int ToFd(intptr_t handle)
		{
			return static_cast<int>(handle);
		}

		// Writing to a closed connection must fail with EPIPE, not kill the process. Linux uses MSG_NOSIGNAL per send
		// instead; macOS needs the socket option.
		void DisableSigPipe([[maybe_unused]] int fd)
		{
	#if defined(SO_NOSIGPIPE)
			const int noSigPipe = 1;
			::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
	#endif
		}

		void SetFlag(int fd, int flag, bool enabled)
		{
			const int flags = ::fcntl(fd, F_GETFL, 0);
			if (flags >= 0)
				::fcntl(fd, F_SETFL, enabled ? (flags | flag) : (flags & ~flag));
		}

		// Creates a TCP socket that child processes (e.g. a launched game) do not inherit.
		int CreateSocket()
		{
			const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
			if (fd >= 0)
			{
				::fcntl(fd, F_SETFD, FD_CLOEXEC);
				DisableSigPipe(fd);
			}
			return fd;
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
		const int fd = CreateSocket();
		if (fd < 0)
		{
			PX_CORE_ERROR("socket() failed: {}", std::strerror(errno));
			return false;
		}
		const int reuse = 1;
		::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

		sockaddr_in address = LoopbackAddress(port);
		if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd, 8) != 0)
		{
			PX_CORE_ERROR("Cannot listen on 127.0.0.1:{}: {}", port, std::strerror(errno));
			::close(fd);
			return false;
		}
		// Non-blocking, so a connection that is reset between WaitReadable and Accept cannot block Accept forever
		// (BSD kernels drop such connections from the queue).
		SetFlag(fd, O_NONBLOCK, true);
		m_Handle = fd;
		return true;
	}

	uint16_t TcpSocket::GetPort() const
	{
		sockaddr_in address{};
		socklen_t length = sizeof(address);
		if (!IsValid() || ::getsockname(ToFd(m_Handle), reinterpret_cast<sockaddr*>(&address), &length) != 0)
			return 0;
		return ntohs(address.sin_port);
	}

	TcpSocket TcpSocket::Accept()
	{
		if (!IsValid())
			return TcpSocket();
		const int fd = ::accept(ToFd(m_Handle), nullptr, nullptr);
		if (fd < 0)
			return TcpSocket();
		::fcntl(fd, F_SETFD, FD_CLOEXEC);
		// BSD-derived systems let accepted sockets inherit O_NONBLOCK from the listener; connections block.
		SetFlag(fd, O_NONBLOCK, false);
		DisableSigPipe(fd);
		const int noDelay = 1;
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
		// A peer that stops reading must not block the server thread forever.
		timeval sendTimeout{};
		sendTimeout.tv_sec = 10;
		::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
		return TcpSocket(fd);
	}

	bool TcpSocket::Connect(uint16_t port)
	{
		Close();
		const int fd = CreateSocket();
		if (fd < 0)
			return false;
		sockaddr_in address = LoopbackAddress(port);
		if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			::close(fd);
			return false;
		}
		m_Handle = fd;
		return true;
	}

	bool TcpSocket::SendAll(const std::string& data)
	{
		if (!IsValid())
			return false;
		size_t sent = 0;
		while (sent < data.size())
		{
	#if defined(MSG_NOSIGNAL)
			const ssize_t result = ::send(ToFd(m_Handle), data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
	#else
			const ssize_t result = ::send(ToFd(m_Handle), data.data() + sent, data.size() - sent, 0);
	#endif
			if (result <= 0)
			{
				if (result < 0 && errno == EINTR)
					continue;
				return false;
			}
			sent += static_cast<size_t>(result);
		}
		return true;
	}

	std::optional<std::string> TcpSocket::Receive(size_t maxBytes)
	{
		if (!IsValid())
			return std::nullopt;
		std::string buffer(maxBytes, '\0');
		for (;;)
		{
			const ssize_t result = ::recv(ToFd(m_Handle), buffer.data(), buffer.size(), 0);
			if (result < 0 && errno == EINTR)
				continue;
			if (result <= 0)
				return std::nullopt;
			buffer.resize(static_cast<size_t>(result));
			return buffer;
		}
	}

	TcpSocket::WaitResult TcpSocket::WaitReadable(int timeoutMilliseconds) const
	{
		if (!IsValid())
			return WaitResult::Error;
		pollfd descriptor{};
		descriptor.fd = ToFd(m_Handle);
		descriptor.events = POLLIN;
		const int result = ::poll(&descriptor, 1, timeoutMilliseconds);
		if (result == 0 || (result < 0 && errno == EINTR))
			return WaitResult::Timeout;
		if (result < 0 || (descriptor.revents & POLLNVAL) != 0)
			return WaitResult::Error;
		// POLLHUP/POLLERR count as readable: the following recv reports the end of the stream.
		return WaitResult::Readable;
	}

	void TcpSocket::Shutdown() const
	{
		if (IsValid())
			::shutdown(ToFd(m_Handle), SHUT_RDWR);
	}

	void TcpSocket::Close()
	{
		if (m_Handle != InvalidHandle)
		{
			::close(ToFd(m_Handle));
			m_Handle = InvalidHandle;
		}
	}

}

#endif
