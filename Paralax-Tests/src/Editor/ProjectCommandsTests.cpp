#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <EditorCore/EditorContext.h>

#include <Paralax/Core/EngineResources.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Project/Project.h>
#include <Paralax/Renderer/Image.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace Paralax;
using namespace Paralax::Editor;
using json = nlohmann::json;

namespace {

	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary) << text;
	}

	// A project with a saved start scene, ready to export. Engine resources come from a small stand-in directory, so
	// tests do not copy the real (large) ones.
	struct ExportFixture
	{
		std::filesystem::path Root;
		std::filesystem::path Directory;
		std::filesystem::path Resources;
		Paralax::Testing::ScopedEnvironmentVariable ResourceOverride;
		EditorContext Context;

		explicit ExportFixture(const std::string& name, const std::string& projectName = "My Game")
			: Root(Paralax::Testing::CreateTestDirectory(name))
			, Directory(Root / "Game")
			, Resources(Root / "EngineResources")
			, ResourceOverride("PARALAX_RESOURCE_DIR", Resources.string())
			, Context(Project::Create(Directory, projectName))
		{
			WriteText(Resources / "Environments" / "Sky.hdr", "not really an image");
			REQUIRE(Context.GetProject());
			REQUIRE(Context.Execute("entity.create", {{"name", "Player"}}).Success);
			REQUIRE(Context.Execute("scene.save", {{"path", "Scenes/Main"}}).Success);
			REQUIRE(Context.Execute("project.set_start_scene", {{"path", "Scenes/Main.pxscene"}}).Success);
		}

		CommandResult Export(const std::filesystem::path& output) { return Context.Execute("project.export", {{"output", ToUtf8(output)}}); }
	};

	std::string ExecutableName(const std::string& stem)
	{
		return stem + ToUtf8(GetExecutablePath().extension());
	}

}

TEST_CASE("ProjectCommands: project.info and project.set_start_scene describe and configure the project")
{
	EditorContext empty;
	CHECK_FALSE(empty.Execute("project.info").Success);
	CHECK_FALSE(empty.Execute("project.set_start_scene", {{"path", "Scenes/Main.pxscene"}}).Success);

	ExportFixture fixture("ProjectInfo");
	const json info = fixture.Context.Execute("project.info").Result;
	CHECK(info["name"] == "My Game");
	CHECK(info["startScene"] == "Scenes/Main.pxscene");
	CHECK_FALSE(fixture.Context.Execute("project.set_start_scene", {{"path", "Scenes/Missing.pxscene"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("project.set_start_scene", {{"path", "../Outside.pxscene"}}).Success);
	Ref<Project> reloaded = Project::Load(fixture.Directory);
	REQUIRE(reloaded);
	CHECK(reloaded->GetStartScene() == fixture.Context.GetProject()->GetStartScene());
}

TEST_CASE("ProjectCommands: project.export writes only the runtime, resources, project file and assets")
{
	ExportFixture fixture("ProjectExport");
	// Nothing but the project file and its assets leaves the project: not editor state, version control, secrets,
	// logs or other project files (a second .pxproj would even stop the runtime from choosing).
	WriteText(fixture.Directory / ".paralax" / "AgentServer.json", "{\"token\": \"secret\"}");
	WriteText(fixture.Directory / "EditorLayout.ini", "[Window]");
	WriteText(fixture.Directory / ".git" / "config", "[core]");
	WriteText(fixture.Directory / ".env", "KEY=secret");
	WriteText(fixture.Directory / "Logs" / "Editor.log", "log");
	WriteText(fixture.Directory / "Backup.pxproj", "{}");
	std::error_code linkError;
	std::filesystem::create_symlink(fixture.Directory / ".env", fixture.Directory / "Assets" / "Linked.txt", linkError);

	const CommandResult result = fixture.Context.Execute("project.export", {{"output", "../Build"}});
	REQUIRE_MESSAGE(result.Success, result.Error);
	const std::filesystem::path output = fixture.Root / "Build";
	CHECK(std::filesystem::equivalent(FromUtf8(result.Result["path"].get<std::string>()), output));
	CHECK(std::filesystem::is_regular_file(output / ExecutableName("My_Game")));
	CHECK(std::filesystem::is_regular_file(output / "My Game.pxproj"));
	CHECK(std::filesystem::is_regular_file(output / "Assets" / "Scenes" / "Main.pxscene"));
	CHECK(std::filesystem::is_regular_file(output / "Assets" / "AssetRegistry.json"));
	CHECK(std::filesystem::is_regular_file(output / "Resources" / "Environments" / "Sky.hdr"));
	for (const char* leaked : {".paralax", "EditorLayout.ini", ".git", ".env", "Logs", "Backup.pxproj", "Assets/Linked.txt"})
	{
		CAPTURE(leaked);
		CHECK_FALSE(std::filesystem::exists(std::filesystem::symlink_status(output / leaked)));
	}
	CHECK_FALSE(std::filesystem::exists(fixture.Root / "Build.paralax-staging"));
	CHECK_FALSE(std::filesystem::exists(fixture.Root / "Build.paralax-old"));

	// The marker lists exactly the files written.
	const std::string marker = Paralax::Testing::ReadFileText(output / ".paralax-export");
	CHECK(marker.starts_with("Paralax export\n"));
	CHECK(marker.find("Assets/Scenes/Main.pxscene\n") != std::string::npos);
	CHECK(marker.find("Resources/Environments/Sky.hdr\n") != std::string::npos);
	CHECK(result.Result["files"].get<size_t>() == static_cast<size_t>(std::count(marker.begin(), marker.end(), '\n')) - 1);

	// The exported project loads on its own, with the same start scene.
	Ref<Project> exported = Project::Load(output);
	REQUIRE(exported);
	CHECK(exported->GetStartScene() == fixture.Context.GetProject()->GetStartScene());
}

TEST_CASE("ProjectCommands: project.export replaces a previous export only if it holds nothing else")
{
	ExportFixture fixture("ProjectExportReplace");
	const std::filesystem::path output = fixture.Root / "Build";
	REQUIRE(fixture.Export(output).Success);

	// Logs written by the game running from there do not count as the user's files.
	WriteText(output / "Logs" / "Runtime.log", "log");
	CHECK(fixture.Export(output).Success);
	CHECK_FALSE(std::filesystem::exists(output / "Logs"));

	// A file the export did not write (a save game, say) is the user's: the export refuses and keeps it.
	WriteText(output / "Save.dat", "progress");
	const CommandResult refused = fixture.Export(output);
	CHECK_FALSE(refused.Success);
	CHECK(refused.Error.find("Save.dat") != std::string::npos);
	CHECK(Paralax::Testing::ReadFileText(output / "Save.dat") == "progress");
	CHECK(std::filesystem::is_regular_file(output / ExecutableName("My_Game")));
	std::filesystem::remove(output / "Save.dat");

	// What an interrupted export leaves behind (marked) is cleared; anything else in its place stops the export.
	std::filesystem::create_directories(fixture.Root / "Build.paralax-staging");
	WriteText(fixture.Root / "Build.paralax-staging" / ".paralax-export", "Paralax export\n");
	WriteText(fixture.Root / "Build.paralax-staging" / "Half.bin", "partial");
	CHECK(fixture.Export(output).Success);
	CHECK_FALSE(std::filesystem::exists(fixture.Root / "Build.paralax-staging"));
	WriteText(fixture.Root / "Build.paralax-old" / "Mine.txt", "keep");
	CHECK_FALSE(fixture.Export(output).Success);
	CHECK(Paralax::Testing::ReadFileText(fixture.Root / "Build.paralax-old" / "Mine.txt") == "keep");
}

TEST_CASE("ProjectCommands: project.export warns about unsaved changes and exports the saved files")
{
	ExportFixture fixture("ProjectExportDirty");
	REQUIRE(fixture.Context.Execute("entity.create", {{"name", "Unsaved"}}).Success);
	const CommandResult result = fixture.Export(fixture.Root / "Build");
	REQUIRE(result.Success);
	REQUIRE(result.Result.contains("warnings"));
	CHECK(result.Result["warnings"][0].get<std::string>().find("unsaved") != std::string::npos);
	CHECK(Paralax::Testing::ReadFileText(fixture.Root / "Build" / "Assets" / "Scenes" / "Main.pxscene").find("Unsaved") == std::string::npos);
}

TEST_CASE("ProjectCommands: project.export refuses destinations it could damage")
{
	ExportFixture fixture("ProjectExportRefused");
	CHECK_FALSE(fixture.Context.Execute("project.export", {{"output", ""}}).Success);
	CHECK_FALSE(fixture.Export(fixture.Directory.root_path()).Success);          // A filesystem root.
	CHECK_FALSE(fixture.Export(fixture.Directory).Success);                      // The project itself.
	CHECK_FALSE(fixture.Export(fixture.Directory / "Assets" / "Build").Success); // Inside the project.
	CHECK_FALSE(fixture.Export(fixture.Root).Success);                           // Contains the project.
	CHECK_FALSE(fixture.Context.Execute("project.export", {{"output", ".."}}).Success);
	CHECK_FALSE(fixture.Export(fixture.Resources / "Build").Success);        // Inside the engine resources.
	CHECK_FALSE(fixture.Export(fixture.Root / "Missing" / "Build").Success); // Only one directory is ever created.
	CHECK_FALSE(std::filesystem::exists(fixture.Root / "Missing"));

	// A link into the project is the project, whatever its name.
	std::error_code linkError;
	std::filesystem::create_directory_symlink(fixture.Directory / "Assets", fixture.Root / "Link", linkError);
	if (!linkError)
		CHECK_FALSE(fixture.Export(fixture.Root / "Link" / "Build").Success);
	else
		MESSAGE("Symbolic links are unavailable here (", linkError.message(), "); skipping the link check");

	// A directory with other contents is never cleared, and a file is never replaced.
	WriteText(fixture.Root / "Occupied" / "Precious.txt", "keep");
	CHECK_FALSE(fixture.Export(fixture.Root / "Occupied").Success);
	CHECK(Paralax::Testing::ReadFileText(fixture.Root / "Occupied" / "Precious.txt") == "keep");
	WriteText(fixture.Root / "File.txt", "keep");
	CHECK_FALSE(fixture.Export(fixture.Root / "File.txt").Success);
	CHECK(Paralax::Testing::ReadFileText(fixture.Root / "File.txt") == "keep");

	// An empty directory is fine.
	std::filesystem::create_directories(fixture.Root / "Empty");
	CHECK(fixture.Export(fixture.Root / "Empty").Success);
}

TEST_CASE("ProjectCommands: project.export accepts a new output directory written with a trailing separator")
{
	ExportFixture fixture("ProjectExportTrailingSlash");
	REQUIRE_FALSE(std::filesystem::exists(fixture.Root / "Build"));
	const CommandResult absolute = fixture.Context.Execute("project.export", {{"output", (fixture.Root / "Build").generic_string() + "/"}});
	CHECK_MESSAGE(absolute.Success, absolute.Error);
	CHECK(std::filesystem::is_directory(fixture.Root / "Build"));
	// By identity: the result is canonical, the temporary directory need not be (macOS /var, Windows short names).
	CHECK(std::filesystem::equivalent(std::filesystem::path(absolute.Result["path"].get<std::string>()), fixture.Root / "Build"));
	// Each export holds a copy of the runtime: removed once checked, so the temporary root stays small.
	std::error_code removeError;
	std::filesystem::remove_all(fixture.Root / "Build", removeError);

	// Relative to the project directory, as the command documents; the same again replaces the previous export.
	CHECK(fixture.Context.Execute("project.export", {{"output", "../Beside/"}}).Success);
	CHECK(std::filesystem::is_directory(fixture.Root / "Beside"));
	CHECK(fixture.Context.Execute("project.export", {{"output", "../Beside//"}}).Success);
	std::filesystem::remove_all(fixture.Root / "Beside", removeError);

	// Other spellings of a directory that does not exist yet.
	for (const char* spelling : {"Doubled//", "Dotted/.", "Dotted2/./", "Climbed/Sub/.."})
	{
		CAPTURE(spelling);
		const std::string name = std::string(spelling).substr(0, std::string(spelling).find('/'));
		REQUIRE_FALSE(std::filesystem::exists(fixture.Root / name));
		const CommandResult result = fixture.Context.Execute("project.export", {{"output", "../" + std::string(spelling)}});
		CHECK_MESSAGE(result.Success, result.Error);
		CHECK(std::filesystem::is_directory(fixture.Root / name));
		std::filesystem::remove_all(fixture.Root / name, removeError);
	}

	// A root has no name however it is written.
	CHECK_FALSE(fixture.Context.Execute("project.export", {{"output", "/"}}).Success);
	CHECK_FALSE(fixture.Context.Execute("project.export", {{"output", "//"}}).Success);
}

TEST_CASE("ProjectCommands: project.export names the executable safely")
{
	// A project named like one of the export's folders (or a reserved Windows name) gets a distinct executable.
	ExportFixture resources("ProjectExportNamed", "Resources");
	REQUIRE(resources.Export(resources.Root / "Build").Success);
	CHECK(std::filesystem::is_regular_file(resources.Root / "Build" / ExecutableName("Resources_Game")));
	CHECK(std::filesystem::is_directory(resources.Root / "Build" / "Resources"));

	ExportFixture device("ProjectExportDevice", "Con");
	REQUIRE(device.Export(device.Root / "Build").Success);
	CHECK(std::filesystem::is_regular_file(device.Root / "Build" / ExecutableName("Con_Game")));

	// Spaces (allowed in project names) become underscores.
	ExportFixture spaced("ProjectExportSpaced", "Space Game");
	REQUIRE(spaced.Export(spaced.Root / "Build").Success);
	CHECK(std::filesystem::is_regular_file(spaced.Root / "Build" / ExecutableName("Space_Game")));
}

TEST_CASE("ProjectCommands: non-ASCII names travel through commands and the export as UTF-8")
{
	// "Brick-é-日本.png" and "Niveau-é-日本.pxscene", as UTF-8 bytes.
	const std::string texture = "Textures/Brick-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.png";
	const std::string scene = "Scenes/Niveau-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.pxscene";
	ExportFixture fixture("ProjectNonAscii");
	REQUIRE(Image(4, 4).SavePNG(fixture.Directory / "Assets" / FromUtf8(texture)));
	const CommandResult imported = fixture.Context.Execute("asset.import", {{"path", texture}});
	REQUIRE_MESSAGE(imported.Success, imported.Error);
	const CommandResult saved = fixture.Context.Execute("scene.save", {{"path", scene}});
	REQUIRE_MESSAGE(saved.Success, saved.Error);
	CHECK(saved.Result["path"].get<std::string>().ends_with(scene));
	REQUIRE(fixture.Context.Execute("scene.open", {{"path", scene}}).Success);
	CHECK(fixture.Context.Execute("scene.info").Result["path"].get<std::string>().ends_with(scene));
	const CommandResult assets = fixture.Context.Execute("asset.list");
	bool listed = false;
	for (const json& asset : assets.Result["assets"])
		listed |= asset["path"] == texture && asset["handle"] == imported.Result["handle"];
	CHECK(listed);

	// The export copies them, and its marker lists them in UTF-8, so the next export recognises its own files.
	const std::filesystem::path output = fixture.Root / FromUtf8("Build-\xC3\xA9");
	const CommandResult exported = fixture.Export(output);
	REQUIRE_MESSAGE(exported.Success, exported.Error);
	CHECK(std::filesystem::equivalent(FromUtf8(exported.Result["path"].get<std::string>()), output));
	CHECK(std::filesystem::is_regular_file(output / "Assets" / FromUtf8(texture)));
	CHECK(std::filesystem::is_regular_file(output / "Assets" / FromUtf8(scene)));
	const std::string marker = Paralax::Testing::ReadFileText(output / ".paralax-export");
	CHECK(marker.find("Assets/" + texture + "\n") != std::string::npos);
	CHECK(marker.find("Assets/" + scene + "\n") != std::string::npos);
	const CommandResult again = fixture.Export(output);
	CHECK_MESSAGE(again.Success, again.Error);

	Ref<Project> exportedProject = Project::Load(output);
	REQUIRE(exportedProject);
	const AssetHandle exportedTexture = exportedProject->GetAssetManager().GetHandleForPath(FromUtf8(texture));
	CHECK(std::to_string(static_cast<uint64_t>(exportedTexture)) == imported.Result["handle"].get<std::string>());
}

TEST_CASE("ProjectCommands: project.export needs a project with a start scene")
{
	const std::filesystem::path root = Paralax::Testing::CreateTestDirectory("ProjectExportNoScene");
	EditorContext empty;
	CHECK_FALSE(empty.Execute("project.export", {{"output", ToUtf8(root / "Build")}}).Success);

	EditorContext context(Project::Create(root / "Game", "Game"));
	REQUIRE(context.GetProject());
	const CommandResult result = context.Execute("project.export", {{"output", ToUtf8(root / "Build")}});
	CHECK_FALSE(result.Success);
	CHECK(result.Error.find("start scene") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(root / "Build"));
}
