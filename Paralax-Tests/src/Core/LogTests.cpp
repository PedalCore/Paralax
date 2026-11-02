#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/Log.h>
#include <Paralax/Core/PathEncoding.h>

#include <spdlog/sinks/ringbuffer_sink.h>

#include <filesystem>
#include <string>

using namespace Paralax;
using Paralax::Testing::ScopedLogCapture;

TEST_CASE("Log: core and client loggers are initialised by the test runner")
{
	REQUIRE(Log::IsInitialized());
	CHECK(Log::GetCoreLogger()->name() == "PARALAX");
	CHECK(Log::GetClientLogger()->name() == "APP");
}

TEST_CASE("Log: macros route messages to the correct logger and level")
{
	ScopedLogCapture capture;

	PX_CORE_INFO("core info {}", 1);
	PX_CORE_ERROR("core error");
	PX_WARN("client warn {}", "x");
	PX_TRACE("client trace");

	const auto entries = capture.GetEntries();
	REQUIRE(entries.size() == 4);
	CHECK(entries[0].LoggerName == "PARALAX");
	CHECK(entries[0].Level == spdlog::level::info);
	CHECK(entries[0].Message == "core info 1");
	CHECK(entries[1].Level == spdlog::level::err);
	CHECK(entries[2].LoggerName == "APP");
	CHECK(entries[2].Message == "client warn x");
	CHECK(entries[3].Level == spdlog::level::trace);
}

TEST_CASE("Log: removed sinks no longer receive messages")
{
	auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(16);
	Log::AddSink(sink);
	PX_INFO("captured");
	Log::RemoveSink(sink);
	PX_INFO("not captured");
	PX_CORE_ERROR("not captured either");

	const std::vector<std::string> messages = sink->last_formatted();
	REQUIRE(messages.size() == 1);
	CHECK(messages[0].find("captured") != std::string::npos);
}

TEST_CASE("Log: SetLevel filters lower-severity messages")
{
	ScopedLogCapture capture;

	Log::SetLevel(spdlog::level::err);
	PX_CORE_WARN("filtered");
	PX_CORE_ERROR("kept");
	Log::SetLevel(spdlog::level::trace);

	const auto entries = capture.GetEntries();
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].Message == "kept");
}

namespace {

	// Shuts the test runner's logging down for the duration of a test and restores it afterwards.
	struct ScopedLogReinit
	{
		ScopedLogReinit() { Log::Shutdown(); }
		~ScopedLogReinit()
		{
			Log::Shutdown();
			Paralax::Testing::InitTestLogging();
		}
	};

}

TEST_CASE("Log: macros are safe no-ops before initialisation and do not evaluate arguments")
{
	ScopedLogReinit reinit;
	REQUIRE_FALSE(Log::IsInitialized());

	int evaluations = 0;
	PX_CORE_ERROR("not logged {}", ++evaluations);
	PX_INFO("not logged {}", ++evaluations);
	CHECK(evaluations == 0);
	CHECK(Log::AddFileSink("unused.log") == nullptr);
}

TEST_CASE("Log: a second Init is ignored with a warning")
{
	ScopedLogCapture capture;
	auto core = Log::GetCoreLogger();

	LogSpecification other;
	other.Level = spdlog::level::off;
	Log::Init(other);

	CHECK(Log::GetCoreLogger() == core);
	CHECK(Log::GetCoreLogger()->level() == spdlog::level::trace);
	CHECK(capture.Contains(spdlog::level::warn, "already initialised"));
}

TEST_CASE("Log: Init with FilePath writes messages to the file, creating directories")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("LogFile");
	const std::filesystem::path path = directory / "nested" / "Test.log";
	{
		ScopedLogReinit reinit;
		LogSpecification specification;
		specification.ConsoleOutput = false;
		specification.FilePath = path;
		Log::Init(specification);

		PX_CORE_INFO("hello from core");
		PX_ERROR("hello from client");
		Log::Flush();
	}

	const std::string contents = Paralax::Testing::ReadFileText(path);
	CHECK(contents.find("PARALAX: hello from core") != std::string::npos);
	CHECK(contents.find("APP: hello from client") != std::string::npos);

	std::error_code error;
	std::filesystem::remove_all(directory, error);
}

TEST_CASE("Log: AddFileSink returns a removable sink and reports unopenable paths")
{
	ScopedLogCapture capture;
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("FileSink");
	const std::filesystem::path path = directory / "Sink.log";

	spdlog::sink_ptr sink = Log::AddFileSink(path);
	REQUIRE(sink);
	PX_CORE_WARN("in file");
	Log::RemoveSink(sink);
	PX_CORE_WARN("after removal");
	sink->flush();
	sink.reset();

	const std::string contents = Paralax::Testing::ReadFileText(path);
	CHECK(contents.find("in file") != std::string::npos);
	CHECK(contents.find("after removal") == std::string::npos);

	// A path whose parent is a regular file can never be opened.
	CHECK(Log::AddFileSink(path / "child.log") == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "Failed to open log file"));

	std::error_code error;
	std::filesystem::remove_all(directory, error);
}

TEST_CASE("Log: AddFileSink opens a file at a path outside the ANSI code page, creating its directories")
{
	// "Protokoll-é-日本-Ω/Журнал.log" as UTF-8 bytes. No ANSI code page holds all of these characters, so on Windows
	// only a wide file name can open it: path::string(), which converts to the code page, throws for it.
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("NonAsciiLogFile");
	const std::filesystem::path path = directory / FromUtf8("Protokoll-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC-\xCE\xA9") / FromUtf8("\xD0\x96\xD1\x83\xD1\x80\xD0\xBD\xD0\xB0\xD0\xBB.log");
	{
		ScopedLogCapture capture;
		spdlog::sink_ptr sink = Log::AddFileSink(path);
		REQUIRE(sink);
		CHECK_FALSE(capture.Contains(spdlog::level::err, "Failed to open log file"));
		PX_CORE_WARN("written to a non-ASCII path");
		Log::RemoveSink(sink);
		sink->flush();
	}

	CHECK(std::filesystem::is_regular_file(path));
	CHECK(Paralax::Testing::ReadFileText(path).find("written to a non-ASCII path") != std::string::npos);

	std::error_code error;
	std::filesystem::remove_all(directory, error);
}
