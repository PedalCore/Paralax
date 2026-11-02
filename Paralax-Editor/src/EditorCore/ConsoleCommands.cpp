#include "EditorCore/ConsoleSink.h"
#include "EditorCore/EditorContext.h"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace Paralax::Editor {

	using json = nlohmann::json;

	namespace {

		constexpr uint64_t DefaultLimit = 100;

		// Level names as agents write and read them: spdlog's, with "warn" also accepted.
		constexpr std::array<std::pair<std::string_view, spdlog::level::level_enum>, 7> LevelNames{{
			{"trace", spdlog::level::trace},
			{"debug", spdlog::level::debug},
			{"info", spdlog::level::info},
			{"warning", spdlog::level::warn},
			{"warn", spdlog::level::warn},
			{"error", spdlog::level::err},
			{"critical", spdlog::level::critical},
		}};

		std::optional<spdlog::level::level_enum> ParseLevel(std::string_view name)
		{
			for (const auto& [levelName, level] : LevelNames)
			{
				if (levelName == name)
					return level;
			}
			return std::nullopt;
		}

		std::string_view LevelName(spdlog::level::level_enum level)
		{
			for (const auto& [levelName, candidate] : LevelNames)
			{
				if (candidate == level)
					return levelName;
			}
			return "off";
		}

		// A non-negative integer parameter, whichever way the JSON stores it (literals from C++ are signed).
		std::optional<uint64_t> NonNegative(const json& value)
		{
			if (value.is_number_unsigned())
				return value.get<uint64_t>();
			if (value.is_number_integer() && value.get<int64_t>() >= 0)
				return static_cast<uint64_t>(value.get<int64_t>());
			return std::nullopt;
		}

	}

	void RegisterConsoleCommands(CommandRegistry& registry, std::shared_ptr<ConsoleSink> sink)
	{
		PX_CORE_VERIFY(sink, "The console commands need a console sink");

		registry.Register({"console.get",
						   "The editor log, as the Console panel shows it: the most recent messages, oldest first. Commands that fail with "
						   "'see the log' explain themselves here; pass the returned revision as sinceRevision to see only what came after.",
						   {{"limit", "integer", "At most this many messages, the most recent (default 100).", false},
							{"minLevel", "string", "Least severe level shown: trace, debug, info (default), warning, error or critical.", false},
							{"sinceRevision", "integer", "Only messages logged after this revision (from an earlier result).", false}},
						   false,
						   [sink](EditorContext&, const json& params) {
							   const uint64_t maxLimit = std::max<uint64_t>(sink->GetCapacity(), 1);
							   uint64_t limit = DefaultLimit;
							   if (params.contains("limit"))
							   {
								   std::optional<uint64_t> requested = NonNegative(params["limit"]);
								   if (!requested || *requested == 0 || *requested > maxLimit)
									   return CommandResult::Fail(fmt::format("'limit' must be between 1 and {}", maxLimit));
								   limit = *requested;
							   }
							   spdlog::level::level_enum minLevel = spdlog::level::info;
							   if (params.contains("minLevel"))
							   {
								   std::optional<spdlog::level::level_enum> level = ParseLevel(params["minLevel"].get<std::string>());
								   if (!level)
									   return CommandResult::Fail("'minLevel' must be trace, debug, info, warning, error or critical");
								   minLevel = *level;
							   }
							   uint64_t since = 0;
							   if (params.contains("sinceRevision"))
							   {
								   std::optional<uint64_t> revision = NonNegative(params["sinceRevision"]);
								   if (!revision)
									   return CommandResult::Fail("'sinceRevision' must be a revision from an earlier console.get (0 or more)");
								   since = *revision;
							   }

							   ConsoleSink::Snapshot snapshot = sink->GetMessagesAfter(since);
							   // Revisions only grow, so a later one comes from another log (an earlier editor session, say),
							   // and answering "nothing since" would be a lie.
							   if (since > snapshot.Revision)
								   return CommandResult::Fail("'sinceRevision' is not a revision of this log; call console.get without sinceRevision");
							   std::vector<ConsoleSink::Message>& messages = snapshot.Messages;
							   std::erase_if(messages, [minLevel](const ConsoleSink::Message& message) { return message.Level < minLevel; });
							   const size_t omitted = messages.size() > limit ? messages.size() - static_cast<size_t>(limit) : 0;
							   json entries = json::array();
							   for (size_t i = omitted; i < messages.size(); i++)
							   {
								   const ConsoleSink::Message& message = messages[i];
								   entries.push_back({{"revision", message.Revision}, {"level", LevelName(message.Level)}, {"logger", message.Logger}, {"text", message.Text}, {"time", message.Time}});
							   }
							   // complete says whether anything logged since sinceRevision is gone, which omitted cannot.
							   return CommandResult::Ok({{"messages", std::move(entries)}, {"revision", snapshot.Revision}, {"omitted", omitted}, {"complete", snapshot.Complete}});
						   }});

		registry.Register({"console.clear", "Empties the editor log shown by the Console panel and console.get.", {}, false, [sink](EditorContext&, const json&) {
							   return CommandResult::Ok({{"revision", sink->Clear()}});
						   }});
	}

}
