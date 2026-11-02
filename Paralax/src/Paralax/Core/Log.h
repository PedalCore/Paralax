#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

#include <filesystem>
#include <memory>

namespace Paralax {

	struct LogSpecification
	{
		// When non-empty, all log output is also written to this file (truncated on start).
		std::filesystem::path FilePath;
		spdlog::level::level_enum Level = spdlog::level::trace;
		// When false, nothing is written to stdout (sinks added via AddSink still receive messages).
		bool ConsoleOutput = true;
	};

	// Engine-wide logging. "Core" is used by the engine itself, "Client" by applications and games.
	// Init() should be called before logging (the entry point does this); log macros are no-ops until then.
	// Logging itself is thread-safe. Init, Shutdown and sink management are main-thread-only and must not run
	// concurrently with logging from other threads.
	class Log
	{
	public:
		// Creates the loggers. Calling Init again while initialised has no effect (a warning is logged).
		static void Init(const LogSpecification& specification = {});
		static void Shutdown();
		static bool IsInitialized() { return s_CoreLogger != nullptr; }

		static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }

		// Attaches an additional sink (e.g. editor console, agent log stream, test capture) to both loggers.
		static void AddSink(const spdlog::sink_ptr& sink);
		static void RemoveSink(const spdlog::sink_ptr& sink);

		// Opens (truncating) a log file and attaches it as a sink. Missing parent directories are created. The path may
		// hold any characters the file system allows (on Windows, those outside the ANSI code page too).
		// Returns the sink so it can be removed later, or nullptr if logging is not initialised or the file cannot be
		// opened (an error is logged).
		static spdlog::sink_ptr AddFileSink(const std::filesystem::path& path);

		static void SetLevel(spdlog::level::level_enum level);
		static void Flush();

	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};

}

// Log macros. Arguments are not evaluated when logging is not initialised.
#define PX_INTERNAL_LOG(loggerKind, level, ...)                                   \
	do                                                                            \
	{                                                                             \
		if (auto& pxLogger = ::Paralax::Log::Get##loggerKind##Logger(); pxLogger) \
			pxLogger->level(__VA_ARGS__);                                         \
	} while (false)

// Core log macros
#define PX_CORE_TRACE(...) PX_INTERNAL_LOG(Core, trace, __VA_ARGS__)
#define PX_CORE_INFO(...) PX_INTERNAL_LOG(Core, info, __VA_ARGS__)
#define PX_CORE_WARN(...) PX_INTERNAL_LOG(Core, warn, __VA_ARGS__)
#define PX_CORE_ERROR(...) PX_INTERNAL_LOG(Core, error, __VA_ARGS__)
#define PX_CORE_CRITICAL(...) PX_INTERNAL_LOG(Core, critical, __VA_ARGS__)

// Client log macros
#define PX_TRACE(...) PX_INTERNAL_LOG(Client, trace, __VA_ARGS__)
#define PX_INFO(...) PX_INTERNAL_LOG(Client, info, __VA_ARGS__)
#define PX_WARN(...) PX_INTERNAL_LOG(Client, warn, __VA_ARGS__)
#define PX_ERROR(...) PX_INTERNAL_LOG(Client, error, __VA_ARGS__)
#define PX_CRITICAL(...) PX_INTERNAL_LOG(Client, critical, __VA_ARGS__)
