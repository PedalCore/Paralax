#pragma once

#include <spdlog/details/os.h>
#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <ctime>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Paralax::Editor {

	// Keeps the most recent log messages for the editor console. Thread-safe (logging may happen on any thread).
	class ConsoleSink : public spdlog::sinks::base_sink<std::mutex>
	{
	public:
		struct Message
		{
			spdlog::level::level_enum Level;
			std::string Logger;
			std::string Text;
			std::string Time;
		};

		explicit ConsoleSink(size_t capacity = 2000)
			: m_Capacity(capacity)
		{
		}

		std::vector<Message> GetMessages() const
		{
			std::scoped_lock lock(m_MessagesMutex);
			return {m_Messages.begin(), m_Messages.end()};
		}

		void Clear()
		{
			std::scoped_lock lock(m_MessagesMutex);
			m_Messages.clear();
			m_Revision++;
		}

		// Increments whenever messages are added or cleared, so the UI can auto-scroll on change.
		uint64_t GetRevision() const
		{
			std::scoped_lock lock(m_MessagesMutex);
			return m_Revision;
		}

	protected:
		void sink_it_(const spdlog::details::log_msg& msg) override
		{
			std::time_t time = std::chrono::system_clock::to_time_t(msg.time);
			const std::tm local = spdlog::details::os::localtime(time);
			char buffer[16];
			std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);

			std::scoped_lock lock(m_MessagesMutex);
			m_Messages.push_back({msg.level, std::string(msg.logger_name.begin(), msg.logger_name.end()), std::string(msg.payload.begin(), msg.payload.end()), buffer});
			while (m_Messages.size() > m_Capacity)
				m_Messages.pop_front();
			m_Revision++;
		}

		void flush_() override {}

	private:
		size_t m_Capacity;
		mutable std::mutex m_MessagesMutex;
		std::deque<Message> m_Messages;
		uint64_t m_Revision = 0;
	};

}
