#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Project/Project.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <fstream>

using namespace Paralax;

namespace {

	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary) << text;
	}

	std::string ValidProject(const std::string& name, const std::string& startScene = "0")
	{
		return R"({"Paralax": {"Type": "Project", "Version": 1}, "Name": ")" + name + R"(", "StartScene": ")" + startScene + R"("})";
	}

}

TEST_CASE("Project: Create makes the directory layout and a loadable project file")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectCreate");
	Ref<Project> project = Project::Create(root / "Game", "My Game");
	REQUIRE(project);
	CHECK(project->GetName() == "My Game");
	CHECK(std::filesystem::is_directory(project->GetAssetDirectory() / "Scenes"));
	CHECK(project->GetProjectFile().filename() == "My Game.pxproj");

	project->SetStartScene(AssetHandle(4242));
	REQUIRE(project->Save());
	Ref<Project> loaded = Project::Load(root / "Game");
	REQUIRE(loaded);
	CHECK(loaded->GetName() == "My Game");
	CHECK(loaded->GetStartScene() == AssetHandle(4242));
	CHECK(loaded->GetDirectory() == project->GetDirectory());
}

TEST_CASE("Project: Create rejects invalid names, existing projects and unusable directories")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectCreateInvalid");
	Paralax::Testing::ScopedLogCapture log;
	CHECK_FALSE(Project::Create(root / "A", ""));
	CHECK_FALSE(Project::Create(root / "A", "Bad/Name"));
	CHECK_FALSE(Project::Create(root / "A", std::string(65, 'x')));
	REQUIRE(Project::Create(root / "A", "Game"));
	CHECK_FALSE(Project::Create(root / "A", "Other")); // Already contains a project.

	WriteText(root / "File", "not a directory");
	CHECK_FALSE(Project::Create(root / "File", "Game"));
}

TEST_CASE("Project: Save writes back to the file it was loaded from")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectSaveBack");
	WriteText(root / "Foo.pxproj", ValidProject("Bar"));
	Ref<Project> project = Project::Load(root / "Foo.pxproj");
	REQUIRE(project);
	CHECK(project->GetName() == "Bar");
	CHECK(project->GetProjectFile().filename() == "Foo.pxproj");
	REQUIRE(project->Save());
	CHECK_FALSE(std::filesystem::exists(root / "Bar.pxproj"));
	// Loading the directory still finds exactly one project.
	CHECK(Project::Load(root));
}

TEST_CASE("Project: Load rejects hostile and malformed project files")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectHostile");
	Paralax::Testing::ScopedLogCapture log;
	const std::pair<const char*, std::string> cases[] = {
		{"Empty", ""},
		{"NotJson", "{nope"},
		{"Array", "[1, 2]"},
		{"NoHeader", R"({"Name": "X"})"},
		{"HeaderNotObject", R"({"Paralax": 5})"},
		{"WrongType", R"({"Paralax": {"Type": "Scene", "Version": 1}})"},
		{"TypeNotString", R"({"Paralax": {"Type": 3, "Version": 1}})"},
		{"VersionZero", R"({"Paralax": {"Type": "Project", "Version": 0}})"},
		{"VersionTooNew", R"({"Paralax": {"Type": "Project", "Version": 2}})"},
		{"VersionString", R"({"Paralax": {"Type": "Project", "Version": "1"}})"},
		// Nested 20,000 levels deep: refused before parsing, since copying or dumping it would overflow the stack.
		{"Deep", R"({"Paralax": {"Type": "Project", "Version": 1}, "Name": )" + Paralax::Testing::DeeplyNestedJson(20000) + "}"},
	};
	for (const auto& [name, text] : cases)
	{
		const std::filesystem::path file = root / name / "P.pxproj";
		WriteText(file, text);
		CAPTURE(name);
		CHECK_FALSE(Project::Load(file));
	}
	CHECK(log.Contains(spdlog::level::err, "nests too deeply"));

	CHECK_FALSE(Project::Load(root / "Missing.pxproj"));
	std::filesystem::create_directories(root / "EmptyDir");
	CHECK_FALSE(Project::Load(root / "EmptyDir"));

	// Two project files in one directory are ambiguous.
	WriteText(root / "Two" / "A.pxproj", ValidProject("A"));
	WriteText(root / "Two" / "B.pxproj", ValidProject("B"));
	CHECK_FALSE(Project::Load(root / "Two"));
	CHECK(Project::Load(root / "Two" / "A.pxproj"));
}

TEST_CASE("Project: a Version that would wrap into range, a negative one and a float are refused as written")
{
	// 4294967297 and -4294967295 read as 1 through get<int>(), which keeps only the low 32 bits.
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectHostileVersions");
	Paralax::Testing::ScopedLogCapture log;
	for (const char* version : {"4294967297", "-4294967295", "-1", "1.5"})
	{
		const std::string shownVersion = version; // doctest shows a const char* as its address.
		CAPTURE(shownVersion);
		const std::filesystem::path file = root / "Game.pxproj";
		WriteText(file, std::string(R"({"Paralax": {"Type": "Project", "Version": )") + version + R"(}, "Name": "Game", "StartScene": "0"})");
		CHECK_FALSE(Project::Load(file));
		CHECK(log.Contains(spdlog::level::err, fmt::format("Unsupported project version in '{}': expected 1-{}, got {} (number)", ToUtf8(file), Project::CurrentVersion, version)));
	}
}

TEST_CASE("Project: oversized project files are refused before reading")
{
	// A valid project, padded with whitespace past the cap: read in full, it would load.
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectLarge");
	const std::string valid = ValidProject("Huge");
	WriteText(root / "Huge.pxproj", valid);
	REQUIRE(Project::Load(root / "Huge.pxproj"));
	WriteText(root / "Huge.pxproj", valid + std::string(MaxTextFileBytes + 1 - valid.size(), ' '));
	Paralax::Testing::ScopedLogCapture log;
	CHECK_FALSE(Project::Load(root / "Huge.pxproj"));
	CHECK(log.Count(spdlog::level::err, "too large") == 1);
	// Found as the directory's one project file, and refused the same way.
	CHECK_FALSE(Project::Load(root));
	CHECK(log.Count(spdlog::level::err, "too large") == 2);
	std::filesystem::remove(root / "Huge.pxproj");
}

TEST_CASE("Project: Load ignores invalid optional fields")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectOptional");
	WriteText(root / "Game.pxproj", R"({"Paralax": {"Type": "Project", "Version": 1}, "Name": "../../evil", "StartScene": "12abc"})");
	Ref<Project> project = Project::Load(root / "Game.pxproj");
	REQUIRE(project);
	CHECK(project->GetName() == "Game"); // Falls back to the file name.
	CHECK_FALSE(project->GetStartScene().IsValid());

	WriteText(root / "Other" / "Game.pxproj", R"({"Paralax": {"Type": "Project", "Version": 1}, "Name": 7, "StartScene": 99})");
	Ref<Project> other = Project::Load(root / "Other");
	REQUIRE(other);
	CHECK(other->GetName() == "Game");
	CHECK_FALSE(other->GetStartScene().IsValid());
}

TEST_CASE("Project: a project file with a non-ASCII name loads, is named after it in UTF-8 and saves")
{
	// "Проект": on an English Windows system the code page cannot represent it, so path::string() would throw.
	const std::string stem = "\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82";
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectNonAscii");
	WriteText(root / FromUtf8(stem + Project::FileExtension), R"({"Paralax": {"Type": "Project", "Version": 1}, "StartScene": "0"})");

	Ref<Project> project = Project::Load(root);
	REQUIRE(project);
	CHECK(project->GetName() == stem);
	CHECK(ToUtf8(project->GetProjectFile().filename()) == stem + Project::FileExtension);
	REQUIRE(project->Save());
	const nlohmann::json document = nlohmann::json::parse(Paralax::Testing::ReadFileText(project->GetProjectFile()), nullptr, false);
	REQUIRE(document.is_object());
	CHECK(document["Name"] == stem);

	Ref<Project> reloaded = Project::Load(project->GetProjectFile());
	REQUIRE(reloaded);
	CHECK(reloaded->GetName() == stem);
}
