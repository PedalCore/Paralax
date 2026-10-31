#pragma once

#include <Paralax/Core/Assert.h>
#include <Paralax/Core/Log.h>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Paralax::Testing {

	// Initialises logging for the test runner: loggers accept every level so ScopedLogCapture sees everything,
	// while only warnings and errors reach the console.
	inline void InitTestLogging()
	{
		LogSpecification specification;
		specification.Level = spdlog::level::trace;
		specification.ConsoleOutput = false;
		Log::Init(specification);

		auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
		consoleSink->set_level(spdlog::level::warn);
		consoleSink->set_pattern("%^[%T] %n: %v%$");
		Log::AddSink(consoleSink);
	}

	// Returns the whole file as a string, or an empty string if it cannot be read.
	inline std::string ReadFileText(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::ostringstream contents;
		contents << file.rdbuf();
		return contents.str();
	}

	// Returns a fresh, empty per-test scratch directory under the system temp directory.
	inline std::filesystem::path CreateTestDirectory(const std::string& name)
	{
		std::filesystem::path directory = std::filesystem::temp_directory_path() / "ParalaxTests" / name;
		std::error_code error;
		std::filesystem::remove_all(directory, error);
		std::filesystem::create_directories(directory, error);
		return directory;
	}

	// Sets (or, with std::nullopt, unsets) an environment variable for the lifetime of the object.
	class ScopedEnvironmentVariable
	{
	public:
		ScopedEnvironmentVariable(std::string name, std::optional<std::string> value)
			: m_Name(std::move(name))
		{
			if (const char* previous = std::getenv(m_Name.c_str()))
				m_Previous = previous;
			Set(value);
		}
		~ScopedEnvironmentVariable() { Set(m_Previous); }

		ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
		ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

	private:
		void Set(const std::optional<std::string>& value)
		{
#if defined(_WIN32)
			_putenv_s(m_Name.c_str(), value ? value->c_str() : "");
#else
			if (value)
				setenv(m_Name.c_str(), value->c_str(), 1);
			else
				unsetenv(m_Name.c_str());
#endif
		}

		std::string m_Name;
		std::optional<std::string> m_Previous;
	};

	// Thrown by ScopedAssertionCapture when an assertion fails.
	class AssertionFailedError : public std::runtime_error
	{
	public:
		explicit AssertionFailedError(const AssertionInfo& info)
			: std::runtime_error(std::string(info.Expression) + (info.Message.empty() ? "" : ": " + info.Message))
			, Info(info)
		{
		}

		AssertionInfo Info;
	};

	// While alive, failed assertions throw AssertionFailedError instead of aborting the process,
	// so tests can verify that invalid usage is caught: CHECK_THROWS_AS(expr, AssertionFailedError).
	class ScopedAssertionCapture
	{
	public:
		ScopedAssertionCapture()
		{
			Assertion::SetHandler([](const AssertionInfo& info) { throw AssertionFailedError(info); });
		}
		~ScopedAssertionCapture() { Assertion::ResetHandler(); }

		ScopedAssertionCapture(const ScopedAssertionCapture&) = delete;
		ScopedAssertionCapture& operator=(const ScopedAssertionCapture&) = delete;
	};

	// Runs `function` with assertion capture active and returns the reported failure, if any.
	// Keeps tests free of unreachable code after [[noreturn]] assertion failures (MSVC C4702).
	template<typename F>
	std::optional<AssertionInfo> CaptureAssertion(F&& function)
	{
		ScopedAssertionCapture capture;
		try
		{
			function();
		}
		catch (const AssertionFailedError& e)
		{
			return e.Info;
		}
		return std::nullopt;
	}

	// Records formatted log messages while alive.
	class ScopedLogCapture
	{
	public:
		struct Entry
		{
			std::string LoggerName;
			spdlog::level::level_enum Level;
			std::string Message;
		};

		ScopedLogCapture()
			: m_Sink(std::make_shared<Sink>())
		{
			Log::AddSink(m_Sink);
		}
		~ScopedLogCapture() { Log::RemoveSink(m_Sink); }

		ScopedLogCapture(const ScopedLogCapture&) = delete;
		ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

		std::vector<Entry> GetEntries() const
		{
			std::scoped_lock lock(m_Sink->EntriesMutex);
			return m_Sink->Entries;
		}

		bool Contains(spdlog::level::level_enum level, const std::string& substring) const
		{
			for (const Entry& entry : GetEntries())
			{
				if (entry.Level == level && entry.Message.find(substring) != std::string::npos)
					return true;
			}
			return false;
		}

	private:
		class Sink : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			mutable std::mutex EntriesMutex;
			std::vector<Entry> Entries;

		protected:
			void sink_it_(const spdlog::details::log_msg& msg) override
			{
				std::scoped_lock lock(EntriesMutex);
				Entries.push_back({std::string(msg.logger_name.begin(), msg.logger_name.end()), msg.level,
								   std::string(msg.payload.begin(), msg.payload.end())});
			}

			void flush_() override {}
		};

		std::shared_ptr<Sink> m_Sink;
	};

}
