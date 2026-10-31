#include "pxpch.h"
#include "Paralax/Network/TcpSocket.h"

namespace Paralax {

	std::optional<std::string> LineReader::ReadLine(const std::function<bool()>& shouldStop)
	{
		for (;;)
		{
			// Only newly received bytes are scanned, so long lines cost linear time.
			const size_t newline = m_Buffer.find('\n', m_ScanOffset);
			if (newline != std::string::npos)
			{
				// A complete line can arrive in a single chunk, so the limit is checked here too.
				if (newline > m_MaxLineLength)
					return std::nullopt;
				std::string line = m_Buffer.substr(0, newline);
				m_Buffer.erase(0, newline + 1);
				m_ScanOffset = 0;
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				return line;
			}
			m_ScanOffset = m_Buffer.size();
			if (m_Buffer.size() > m_MaxLineLength)
				return std::nullopt;

			if (shouldStop)
			{
				// Wait in short slices so a stop request is noticed promptly without closing the socket under us.
				const TcpSocket::WaitResult wait = m_Socket.WaitReadable(50);
				if (shouldStop() || wait == TcpSocket::WaitResult::Error)
					return std::nullopt;
				if (wait == TcpSocket::WaitResult::Timeout)
					continue;
			}

			std::optional<std::string> chunk = m_Socket.Receive();
			if (!chunk)
				return std::nullopt;
			m_Buffer += *chunk;
		}
	}

}
