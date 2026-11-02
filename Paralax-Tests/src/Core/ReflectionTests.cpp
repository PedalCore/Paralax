#include <doctest/doctest.h>

#include <Paralax/Core/JsonText.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Core/Reflection.h>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace Paralax;
using json = nlohmann::json;

namespace {

	enum class Gait
	{
		Walk,
		Run
	};

	struct Settings
	{
		float Speed = 1.0f;
		int Count = 0;
		std::string Label;
		Gait Movement = Gait::Walk;
	};

	const std::vector<FieldInfo>& GetSettingsFields()
	{
		static const std::vector<FieldInfo> s_Fields = {
			Reflection::MakeField<&Settings::Speed>("Speed", Reflection::FieldOptions().Range(0.0f, 10.0f)),
			Reflection::MakeField<&Settings::Count>("Count"),
			Reflection::MakeField<&Settings::Label>("Label", Reflection::FieldOptions().MaxLength(16)),
			Reflection::MakeField<&Settings::Movement>("Movement", Reflection::FieldOptions().Enum({"Walk", "Run"})),
		};
		return s_Fields;
	}

	const FieldInfo& Field(std::string_view name)
	{
		const FieldInfo* field = FindField(GetSettingsFields(), name);
		REQUIRE(field);
		return *field;
	}

	// Far more than any error needs, and far less than the values these tests send.
	constexpr size_t ShortErrorBytes = 512;
	constexpr std::string_view Replacement = "\xEF\xBF\xBD";

}

TEST_CASE("Reflection: a 16 MiB value refused by a number field gives a short error that names the field and what it expects")
{
	Settings settings;
	std::string error;
	const std::string huge(16u << 20, 'x');
	CHECK_FALSE(Reflection::SetField(Field("Speed"), &settings, huge, error));
	REQUIRE_MESSAGE(error.size() < ShortErrorBytes, error.substr(0, ShortErrorBytes));
	CHECK_MESSAGE(error.starts_with("Field 'Speed' expects a finite number, got \"xxxx"), error);
	CHECK_MESSAGE(error.ends_with("... (string of 16777216 bytes)"), error);
	CHECK(settings.Speed == 1.0f);

	// Every other shape a refused value takes is cut too.
	json elements = json::array();
	for (int i = 0; i < 100000; i++)
		elements.push_back("x");
	CHECK_FALSE(Reflection::SetField(Field("Count"), &settings, elements, error));
	REQUIRE_MESSAGE(error.size() < ShortErrorBytes, error.substr(0, ShortErrorBytes));
	CHECK_MESSAGE(error.starts_with("Field 'Count' expects an integer, got [\"x\",\"x\","), error);
	CHECK_MESSAGE(error.ends_with("... (array of 100000 elements)"), error);

	// A string refused for its length, as a name sent by an agent would be.
	CHECK_FALSE(Reflection::SetField(Field("Label"), &settings, huge, error));
	REQUIRE_MESSAGE(error.size() < ShortErrorBytes, error.substr(0, ShortErrorBytes));
	CHECK_MESSAGE(error.starts_with("Field 'Label' expects at most 16 bytes, got \"xxxx"), error);
	CHECK(settings.Label.empty());

	// A short value is still shown whole, with its type.
	CHECK_FALSE(Reflection::SetField(Field("Speed"), &settings, 11.0, error));
	CHECK(error == "Field 'Speed' expects a number in [0, 10], got 11.0 (number)");
	CHECK_FALSE(Reflection::SetField(Field("Movement"), &settings, "Fly", error));
	CHECK(error == "Field 'Movement' expects one of: Walk, Run, got \"Fly\" (string)");
}

TEST_CASE("Reflection: a string built in code that is not UTF-8 is refused without throwing and shown as U+FFFD")
{
	Settings settings;
	const json invalid = std::string("fast\xFF");
	// What it guards against: nlohmann's strict dump() throws on these bytes.
	CHECK_THROWS_AS(static_cast<void>(invalid.dump()), json::type_error);

	std::string error;
	bool accepted = true;
	CHECK_NOTHROW(accepted = Reflection::SetField(Field("Speed"), &settings, invalid, error));
	CHECK_FALSE(accepted);
	CHECK(error == "Field 'Speed' expects a finite number, got \"fast" + std::string(Replacement) + "\" (string)");
	CHECK_NOTHROW(accepted = Reflection::SetField(Field("Movement"), &settings, invalid, error));
	CHECK_FALSE(accepted);
	CHECK(error == "Field 'Movement' expects one of: Walk, Run, got \"fast" + std::string(Replacement) + "\" (string)");

	// Too long and not UTF-8: cut between replacement characters, so the error stays well formed.
	const json tooLong = std::string(1000, '\xFF');
	CHECK_NOTHROW(accepted = Reflection::SetField(Field("Label"), &settings, tooLong, error));
	CHECK_FALSE(accepted);
	REQUIRE_MESSAGE(error.size() < ShortErrorBytes, error.substr(0, ShortErrorBytes));
	CHECK(IsWellFormedUtf8(error));
	CHECK(error.ends_with("... (string of 1000 bytes)"));
	// Multibyte characters are never split either.
	std::string accents;
	for (int i = 0; i < 500; i++)
		accents += "\xC3\xA9";
	CHECK_FALSE(Reflection::SetField(Field("Label"), &settings, accents, error));
	CHECK(IsWellFormedUtf8(error));
	CHECK(error.find("\"\xC3\xA9\xC3\xA9") != std::string::npos);
}

TEST_CASE("Reflection: FromJson lists the first problems and counts the rest")
{
	json document = json::object();
	for (int i = 0; i < 10000; i++)
		document["Unknown" + std::to_string(10000 + i)] = i;
	document["Speed"] = 2.5;

	Settings settings;
	std::string error;
	CHECK_FALSE(Reflection::FromJson(GetSettingsFields(), "Settings", &settings, document, error));
	// Valid fields are still applied.
	CHECK(settings.Speed == 2.5f);
	REQUIRE_MESSAGE(error.size() < ShortErrorBytes, error.substr(0, ShortErrorBytes));
	CHECK_MESSAGE(error.starts_with("Unknown field 'Settings.Unknown10000'; Unknown field 'Settings.Unknown10001'"), error);
	CHECK(error.find("'Settings.Unknown10007'") != std::string::npos);
	CHECK(error.find("Unknown10008") == std::string::npos);
	CHECK_MESSAGE(error.ends_with("; and 9992 more"), error);

	// Exactly as many problems as are listed: nothing is left out, so nothing is counted.
	json eight = json::object();
	for (int i = 0; i < static_cast<int>(Reflection::ErrorList::MaxListed); i++)
		eight["U" + std::to_string(i)] = 1;
	CHECK_FALSE(Reflection::FromJson(GetSettingsFields(), "Settings", &settings, eight, error));
	CHECK(error.find("more") == std::string::npos);
	CHECK(error.ends_with("Unknown field 'Settings.U7'"));
	eight["Count"] = "many";
	CHECK_FALSE(Reflection::FromJson(GetSettingsFields(), "Settings", &settings, eight, error));
	CHECK(error.ends_with("; and 1 more"));

	CHECK(Reflection::FromJson(GetSettingsFields(), "Settings", &settings, json{{"Count", 3}}, error));
	CHECK(error.empty());
}

TEST_CASE("Reflection: an unknown field of any size or bytes is named in a short preview")
{
	Settings settings;
	std::string error;
	const std::string hugeKey(16u << 20, 'k');
	CHECK_FALSE(Reflection::FromJson(GetSettingsFields(), "Settings", &settings, json{{hugeKey, 1}}, error));
	REQUIRE_MESSAGE(error.size() < ShortErrorBytes, error.substr(0, ShortErrorBytes));
	CHECK(error == "Unknown field 'Settings." + std::string(MaxPreviewBytes, 'k') + "...' (a name of 16777216 bytes)");

	// Not UTF-8, and holding a quote and a line break: nothing in it can break or fake a log line.
	CHECK_FALSE(Reflection::FromJson(GetSettingsFields(), "Settings", &settings, json{{std::string("bad\xFF\"\n"), 1}}, error));
	CHECK(error == "Unknown field 'Settings.bad" + std::string(Replacement) + "\\\"\\n'");
}

TEST_CASE("Reflection: ErrorList joins the first errors and counts the rest")
{
	Reflection::ErrorList errors;
	CHECK(errors.IsEmpty());
	CHECK(errors.GetCount() == 0);
	CHECK(errors.Join().empty());

	errors.Add("first");
	CHECK_FALSE(errors.IsEmpty());
	CHECK(errors.Join() == "first");
	for (int i = 0; i < 20; i++)
		errors.Add("e" + std::to_string(i));
	CHECK(errors.GetCount() == 21);
	CHECK(errors.Join() == "first; e0; e1; e2; e3; e4; e5; e6; and 13 more");
}
