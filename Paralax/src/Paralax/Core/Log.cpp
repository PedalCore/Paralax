#include "pxpch.h"
#include "Paralax/Core/Log.h"

#include "Paralax/Core/PathEncoding.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <vector>

namespace Paralax {

	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	namespace {

		std::shared_ptr<spdlog::logger> CreateLogger(const char* name, const std::vector<spdlog::sink_ptr>& sinks, spdlog::level::level_enum level)
		{
			auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
			logger->set_level(level);
			logger->flush_on(spdlog::level::warn);
			return logger;
		}

	}

	void Log::Init(const LogSpecification& specification)
	{
		if (IsInitialized())
		{
			s_CoreLogger->warn("Log::Init called while already initialised; ignoring the new specification");
			return;
		}

		std::vector<spdlog::sink_ptr> sinks;

		if (specification.ConsoleOutput)
		{
			auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			consoleSink->set_pattern("%^[%T] %n: %v%$");
			sinks.push_back(consoleSink);
		}

		s_CoreLogger = CreateLogger("PARALAX", sinks, specification.Level);
		s_ClientLogger = CreateLogger("APP", sinks, specification.Level);

		if (!specification.FilePath.empty())
			AddFileSink(specification.FilePath);
	}

	spdlog::sink_ptr Log::AddFileSink(const std::filesystem::path& path)
	{
		if (!IsInitialized())
			return nullptr;

		// spdlog opens the file through the C runtime, which takes the native narrow string, so path::string() is right
		// here (not ToUtf8). It throws on Windows for a name the code page cannot represent, and spdlog throws on
		// failure; both stay at this boundary.
		spdlog::sink_ptr sink;
		std::string error;
		try
		{
			sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
			sink->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");
		}
		catch (const std::exception& e)
		{
			error = e.what();
			sink.reset();
		}

		if (!sink)
		{
			if (s_CoreLogger)
				s_CoreLogger->error("Failed to open log file '{}': {}", ToUtf8(path), error);
			return nullptr;
		}

		AddSink(sink);
		return sink;
	}

	void Log::Shutdown()
	{
		Flush();
		s_ClientLogger.reset();
		s_CoreLogger.reset();
	}

	void Log::AddSink(const spdlog::sink_ptr& sink)
	{
		for (auto* logger : {s_CoreLogger.get(), s_ClientLogger.get()})
		{
			if (logger)
				logger->sinks().push_back(sink);
		}
	}

	void Log::RemoveSink(const spdlog::sink_ptr& sink)
	{
		for (auto* logger : {s_CoreLogger.get(), s_ClientLogger.get()})
		{
			if (!logger)
				continue;

			auto& sinks = logger->sinks();
			std::erase(sinks, sink);
		}
	}

	void Log::SetLevel(spdlog::level::level_enum level)
	{
		for (auto* logger : {s_CoreLogger.get(), s_ClientLogger.get()})
		{
			if (logger)
				logger->set_level(level);
		}
	}

	void Log::Flush()
	{
		for (auto* logger : {s_CoreLogger.get(), s_ClientLogger.get()})
		{
			if (logger)
				logger->flush();
		}
	}

}
