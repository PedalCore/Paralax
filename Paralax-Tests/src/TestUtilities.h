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

	// The test runner's own main() arguments, set by TestMain before any test runs, for tests of how the engine reads
	// the process arguments (GetProcessArguments).
	struct MainArguments
	{
		int Count = 0;
		const char* const* Values = nullptr;
	};

	inline MainArguments& GetMainArguments()
	{
		static MainArguments s_Arguments;
		return s_Arguments;
	}

	// Returns the whole file as a string, or an empty string if it cannot be read.
	inline std::string ReadFileText(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::ostringstream contents;
		contents << file.rdbuf();
		return contents.str();
	}

	// A 16-bit PNG in memory (exported height maps are greyscale), written with uncompressed deflate blocks so no
	// compressor is needed. channels is 1 (grey), 2 (grey and alpha), 3 (RGB) or 4 (RGBA); values holds width * height
	// * channels samples, row by row from the top.
	inline std::vector<uint8_t> MakePng16(uint32_t width, uint32_t height, const std::vector<uint16_t>& values, uint32_t channels = 1)
	{
		auto crc32 = [](const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
			for (size_t i = 0; i < size; i++)
			{
				crc ^= data[i];
				for (int bit = 0; bit < 8; bit++)
					crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
			}
			return crc;
		};
		auto put32 = [](std::vector<uint8_t>& out, uint32_t value) {
			for (int shift = 24; shift >= 0; shift -= 8)
				out.push_back(static_cast<uint8_t>(value >> shift));
		};
		auto chunk = [&](std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
			put32(out, static_cast<uint32_t>(data.size()));
			std::vector<uint8_t> body(type, type + 4);
			body.insert(body.end(), data.begin(), data.end());
			out.insert(out.end(), body.begin(), body.end());
			put32(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
		};

		// Scanlines: filter byte 0, then big-endian 16-bit samples.
		std::vector<uint8_t> raw;
		for (uint32_t y = 0; y < height; y++)
		{
			raw.push_back(0);
			for (uint32_t x = 0; x < width * channels; x++)
			{
				const uint16_t value = values[static_cast<size_t>(y) * width * channels + x];
				raw.push_back(static_cast<uint8_t>(value >> 8));
				raw.push_back(static_cast<uint8_t>(value & 0xFF));
			}
		}
		// zlib stream of stored blocks (at most 65535 bytes each), then Adler-32.
		std::vector<uint8_t> zlib{0x78, 0x01};
		for (size_t offset = 0; offset < raw.size() || offset == 0; offset += 65535)
		{
			const size_t length = std::min<size_t>(65535, raw.size() - offset);
			zlib.push_back(offset + length >= raw.size() ? 1 : 0);
			zlib.push_back(static_cast<uint8_t>(length & 0xFF));
			zlib.push_back(static_cast<uint8_t>(length >> 8));
			zlib.push_back(static_cast<uint8_t>(~length & 0xFF));
			zlib.push_back(static_cast<uint8_t>((~length >> 8) & 0xFF));
			zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
			if (raw.empty())
				break;
		}
		uint32_t a = 1, b = 0;
		for (uint8_t byte : raw)
		{
			a = (a + byte) % 65521;
			b = (b + a) % 65521;
		}
		put32(zlib, (b << 16) | a);

		std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
		std::vector<uint8_t> header;
		put32(header, width);
		put32(header, height);
		const uint8_t colourType = channels == 1 ? 0 : channels == 2 ? 4
												   : channels == 3   ? 2
																	 : 6;
		header.insert(header.end(), {16, colourType, 0, 0, 0}); // 16-bit, deflate, no filter, no interlace.
		chunk(png, "IHDR", header);
		chunk(png, "IDAT", zlib);
		chunk(png, "IEND", {});
		return png;
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

	// Creates a file of exactly `bytes` bytes by writing its last byte only, so a file beyond a loader's size limit
	// costs (on file systems with sparse files) neither time nor disk space to make.
	inline void CreateSparseFile(const std::filesystem::path& path, uintmax_t bytes)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (bytes > 0)
		{
			file.seekp(static_cast<std::streamoff>(bytes - 1));
			file.put(' ');
		}
	}

	// A JSON value nested `depth` levels deep, alternating arrays and objects around a number. Hostile input for
	// every loader: nlohmann parses it iteratively, but copying, dumping or walking the parsed value would overflow
	// the stack, so loaders must refuse it on the text before parsing.
	inline std::string DeeplyNestedJson(size_t depth)
	{
		std::string text;
		text.reserve(depth * 4 + 1);
		for (size_t level = 0; level < depth; level++)
			text += level % 2 == 0 ? "[" : "{\"k\":";
		text += "0";
		for (size_t level = depth; level-- > 0;)
			text += level % 2 == 0 ? "]" : "}";
		return text;
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

		bool Contains(spdlog::level::level_enum level, const std::string& substring) const { return Count(level, substring) > 0; }

		// How many messages at this level contain the substring.
		size_t Count(spdlog::level::level_enum level, const std::string& substring) const
		{
			size_t count = 0;
			for (const Entry& entry : GetEntries())
			{
				if (entry.Level == level && entry.Message.find(substring) != std::string::npos)
					count++;
			}
			return count;
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
