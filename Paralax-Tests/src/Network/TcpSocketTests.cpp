#include <doctest/doctest.h>

#include <Paralax/Network/TcpSocket.h>

#include <thread>

using namespace Paralax;

namespace {

	struct ConnectedPair
	{
		TcpSocket Listener;
		TcpSocket Client;
		TcpSocket Server;
	};

	ConnectedPair Connect()
	{
		ConnectedPair pair;
		REQUIRE(pair.Listener.Listen(0));
		REQUIRE(pair.Listener.GetPort() != 0);
		REQUIRE(pair.Client.Connect(pair.Listener.GetPort()));
		pair.Server = pair.Listener.Accept();
		REQUIRE(pair.Server.IsValid());
		return pair;
	}

}

TEST_CASE("TcpSocket: a loopback connection carries data both ways")
{
	ConnectedPair pair = Connect();
	REQUIRE(pair.Client.SendAll("ping"));
	std::string received;
	while (received.size() < 4)
	{
		std::optional<std::string> chunk = pair.Server.Receive();
		REQUIRE(chunk);
		received += *chunk;
	}
	CHECK(received == "ping");

	REQUIRE(pair.Server.SendAll("pong"));
	std::optional<std::string> reply = pair.Client.Receive(4);
	REQUIRE(reply);
	CHECK(reply->size() <= 4);
	CHECK(std::string_view("pong").starts_with(*reply));
}

TEST_CASE("TcpSocket: closed and invalid sockets fail cleanly")
{
	TcpSocket socket;
	CHECK_FALSE(socket.IsValid());
	CHECK(socket.GetPort() == 0);
	CHECK_FALSE(socket.Receive());
	CHECK(socket.WaitReadable(0) == TcpSocket::WaitResult::Error);
	CHECK_FALSE(socket.Accept().IsValid());

	ConnectedPair pair = Connect();
	pair.Client.Close();
	CHECK_FALSE(pair.Client.IsValid());
	// The peer sees the end of the stream as readable, then a failed receive.
	CHECK(pair.Server.WaitReadable(5000) == TcpSocket::WaitResult::Readable);
	CHECK_FALSE(pair.Server.Receive());

	// Nothing listens on the port any more once the listener closes.
	const uint16_t port = pair.Listener.GetPort();
	pair.Listener.Close();
	TcpSocket late;
	CHECK_FALSE(late.Connect(port));
}

TEST_CASE("TcpSocket: WaitReadable times out without data and reports pending data")
{
	ConnectedPair pair = Connect();
	CHECK(pair.Server.WaitReadable(10) == TcpSocket::WaitResult::Timeout);
	REQUIRE(pair.Client.SendAll("x"));
	CHECK(pair.Server.WaitReadable(5000) == TcpSocket::WaitResult::Readable);
	// No further connection is pending on the listener.
	CHECK(pair.Listener.WaitReadable(10) == TcpSocket::WaitResult::Timeout);
}

TEST_CASE("TcpSocket: moving transfers ownership of the connection")
{
	ConnectedPair pair = Connect();
	TcpSocket moved(std::move(pair.Client));
	CHECK_FALSE(pair.Client.IsValid()); // NOLINT(bugprone-use-after-move): checking the moved-from state is the point
	CHECK(moved.IsValid());
	REQUIRE(moved.SendAll("m"));
	TcpSocket assigned;
	assigned = std::move(moved);
	CHECK(assigned.IsValid());
	CHECK_FALSE(moved.IsValid()); // NOLINT(bugprone-use-after-move)
	CHECK(pair.Server.Receive() == std::optional<std::string>("m"));
}

TEST_CASE("TcpSocket: binding a port that is in use fails")
{
	TcpSocket first;
	REQUIRE(first.Listen(0));
	TcpSocket second;
	CHECK_FALSE(second.Listen(first.GetPort()));
	CHECK_FALSE(second.IsValid());
}

TEST_CASE("LineReader: splits newline-delimited messages across arbitrary chunks")
{
	ConnectedPair pair = Connect();
	REQUIRE(pair.Client.SendAll("first\r\nsec"));
	LineReader reader(pair.Server);
	CHECK(reader.ReadLine() == std::optional<std::string>("first"));
	REQUIRE(pair.Client.SendAll("ond\n\nthird\n"));
	CHECK(reader.ReadLine() == std::optional<std::string>("second"));
	CHECK(reader.ReadLine() == std::optional<std::string>(""));
	CHECK(reader.ReadLine() == std::optional<std::string>("third"));
	pair.Client.Close();
	CHECK_FALSE(reader.ReadLine()); // End of stream.
}

TEST_CASE("LineReader: a line longer than the limit ends the stream")
{
	ConnectedPair pair = Connect();
	REQUIRE(pair.Client.SendAll(std::string(100, 'a')));
	LineReader reader(pair.Server, 16);
	CHECK_FALSE(reader.ReadLine());

	// Also when the whole oversized line, newline included, arrives at once; lines within the limit still work.
	ConnectedPair second = Connect();
	REQUIRE(second.Client.SendAll("short\n" + std::string(100, 'b') + "\n"));
	LineReader limited(second.Server, 16);
	CHECK(limited.ReadLine() == std::optional<std::string>("short"));
	CHECK_FALSE(limited.ReadLine());
}

TEST_CASE("LineReader: the stop callback interrupts a blocked read")
{
	ConnectedPair pair = Connect();
	LineReader reader(pair.Server);
	int polls = 0;
	CHECK_FALSE(reader.ReadLine([&] { return ++polls >= 3; }));
	CHECK(polls == 3);
}
