#pragma once

#include <Paralax/Core/Assert.h>
#include <Paralax/Core/Log.h>
#include <Paralax/Core/UUID.h>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

	// A RIFF WAVE file in memory: 16-bit PCM or (bitsPerSample 32) IEEE float, from interleaved samples in [-1, 1].
	inline std::vector<uint8_t> MakeWav(uint16_t channels, uint32_t sampleRate, const std::vector<float>& samples, uint16_t bitsPerSample = 16)
	{
		std::vector<uint8_t> bytes;
		auto put = [&](uint64_t value, int size) {
			for (int i = 0; i < size; i++)
				bytes.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
		};
		auto tag = [&](const char* text) { bytes.insert(bytes.end(), text, text + 4); };
		const uint32_t bytesPerSample = bitsPerSample / 8u;
		const uint32_t dataBytes = static_cast<uint32_t>(samples.size()) * bytesPerSample;
		tag("RIFF");
		put(36u + dataBytes, 4);
		tag("WAVE");
		tag("fmt ");
		put(16, 4);
		put(bitsPerSample == 32 ? 3 : 1, 2); // IEEE float or PCM.
		put(channels, 2);
		put(sampleRate, 4);
		put(sampleRate * channels * bytesPerSample, 4);
		put(channels * bytesPerSample, 2);
		put(bitsPerSample, 2);
		tag("data");
		put(dataBytes, 4);
		for (float sample : samples)
		{
			if (bitsPerSample == 32)
			{
				uint32_t bits = 0;
				std::memcpy(&bits, &sample, sizeof(bits));
				put(bits, 4);
			}
			else
				put(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f))), 2);
		}
		return bytes;
	}

	// This test process's scratch root: unique per process (a random UUID), so test runs in parallel on one machine
	// (several build configurations, other developers' or agents' runs) never touch each other's files. Removed by
	// TestMain when the run ends.
	inline const std::filesystem::path& GetTestRoot()
	{
		static const std::filesystem::path s_Root = std::filesystem::temp_directory_path() / "ParalaxTests" / std::to_string(static_cast<uint64_t>(UUID()));
		return s_Root;
	}

	// Returns a fresh, empty per-test scratch directory under this process's test root.
	inline std::filesystem::path CreateTestDirectory(const std::string& name)
	{
		std::filesystem::path directory = GetTestRoot() / name;
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

	// Sets the engine's log level while alive (e.g. off, for tests that feed thousands of bad inputs on purpose).
	class ScopedLogLevel
	{
	public:
		explicit ScopedLogLevel(spdlog::level::level_enum level)
			: m_Previous(Log::GetCoreLogger()->level())
		{
			Log::SetLevel(level);
		}
		~ScopedLogLevel() { Log::SetLevel(m_Previous); }

		ScopedLogLevel(const ScopedLogLevel&) = delete;
		ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;

	private:
		spdlog::level::level_enum m_Previous;
	};

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
