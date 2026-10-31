#include "pxpch.h"
#include "Paralax/Core/Assert.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Paralax {

	namespace {

		std::mutex s_HandlerMutex;
		Assertion::Handler s_Handler;

		void Report(const AssertionInfo& info)
		{
			if (Log::IsInitialized())
			{
				PX_CORE_CRITICAL("Assertion failed: {} ({}:{}){}{}", info.Expression, info.File, info.Line,
								 info.Message.empty() ? "" : " - ", info.Message);
				Log::Flush();
			}
			else
			{
				std::fprintf(stderr, "Assertion failed: %s (%s:%d)%s%s\n", info.Expression, info.File, info.Line,
							 info.Message.empty() ? "" : " - ", info.Message.c_str());
				std::fflush(stderr);
			}
		}

	}

	void Assertion::SetHandler(Handler handler)
	{
		std::scoped_lock lock(s_HandlerMutex);
		s_Handler = std::move(handler);
	}

	void Assertion::ResetHandler()
	{
		SetHandler({});
	}

	bool Assertion::HasCustomHandler()
	{
		std::scoped_lock lock(s_HandlerMutex);
		return static_cast<bool>(s_Handler);
	}

	void Assertion::Fail(const char* expression, const char* file, int line, std::string message)
	{
		AssertionInfo info{expression, file, line, std::move(message)};

		Handler handler;
		{
			std::scoped_lock lock(s_HandlerMutex);
			handler = s_Handler;
		}

		if (handler)
		{
			handler(info);
			// A handler that returns would let execution continue past a violated invariant.
			std::fprintf(stderr, "Assertion handler returned for '%s'; aborting\n", info.Expression);
			std::fflush(stderr);
			std::abort();
		}

		Report(info);
		PX_DEBUGBREAK();
		std::abort();
	}

}
