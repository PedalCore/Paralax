#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Game/GameSession.h>
#include <Paralax/Game/GameState.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/SceneSerializer.h>
#include <Paralax/Scripting/ScriptAsset.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Paralax;

namespace {

	// A project-like asset directory with scripted scenes.
	struct SessionFixture
	{
		explicit SessionFixture(const std::string& name)
			: Directory(Paralax::Testing::CreateTestDirectory(name))
			, Assets(Directory)
		{
		}

		AssetHandle Script(const std::string& path, const std::string& source)
		{
			std::filesystem::create_directories((Directory / path).parent_path());
			REQUIRE(WriteFileAtomically(Directory / path, source));
			return Assets.ImportAsset(path);
		}

		// A scene with one scripted entity named name, saved and registered at path.
		AssetHandle SceneFile(const std::string& path, const std::string& name, AssetHandle script)
		{
			Scene scene;
			Entity entity = scene.CreateEntity(name);
			entity.AddComponent<ScriptComponent>().Script = script;
			std::filesystem::create_directories((Directory / path).parent_path());
			REQUIRE(SceneSerializer::SaveToFile(scene, Directory / path));
			return Assets.ImportAsset(path);
		}

		std::filesystem::path Directory;
		AssetManager Assets;
	};

	bool HasEntity(GameSession& game, const std::string& name)
	{
		return game.GetScene().FindEntityByName(name).has_value();
	}

}

TEST_CASE("GameState: values of the supported types are kept, within limits")
{
	GameState state;
	CHECK(state.Set("Score", 12.0));
	CHECK(state.Set("Level", int64_t(3)));
	CHECK(std::get<int64_t>(*state.Get("Level")) == 3);
	CHECK(state.Set("Name", std::string("Ada")));
	CHECK(state.Set("Won", true));
	CHECK(std::get<double>(*state.Get("Score")) == 12.0);
	CHECK(std::get<std::string>(*state.Get("Name")) == "Ada");
	CHECK(std::get<bool>(*state.Get("Won")));
	CHECK_FALSE(state.Get("Missing"));
	CHECK(state.Set("Score", 13.0)); // Replacing is always allowed.
	CHECK(state.Remove("Name"));
	CHECK_FALSE(state.Remove("Name"));

	CHECK_FALSE(state.Set("", 1.0));
	CHECK_FALSE(state.Set(std::string(GameState::MaxKeyBytes + 1, 'k'), 1.0));
	CHECK_FALSE(state.Set("NaN", std::numeric_limits<double>::quiet_NaN()));
	CHECK_FALSE(state.Set("Huge", std::string(GameState::MaxStringBytes + 1, 'x')));
	CHECK(std::get<double>(*state.Get("Score")) == 13.0); // Unchanged by the refused sets.

	// The store as a whole is capped too (it lives outside the scripts' memory limit).
	state.Clear();
	CHECK(state.GetBytes() == 0);
	const std::string big(GameState::MaxStringBytes, 'x');
	size_t stored = 0;
	while (state.Set("Big" + std::to_string(stored), big))
		stored++;
	CHECK(stored == GameState::MaxTotalBytes / (GameState::MaxStringBytes + 5)); // 64 KiB values with short keys: 15 fit in 1 MiB.
	CHECK(state.GetBytes() <= GameState::MaxTotalBytes);
	CHECK(state.Remove("Big0"));
	CHECK(state.Set("Big0", big)); // Room again.

	state.Clear();
	for (size_t i = 0; i < GameState::MaxKeys; i++)
		REQUIRE(state.Set("K" + std::to_string(i), 1.0));
	CHECK_FALSE(state.Set("OneTooMany", 1.0));
	CHECK(state.Set("K0", 2.0));
	CHECK(state.GetSize() == GameState::MaxKeys);
}

TEST_CASE("GameSession: scripts switch scenes and restart them after the update, keeping Game values")
{
	SessionFixture fixture("GameSessionSwitch");
	const AssetHandle second = fixture.SceneFile("Scenes/Second.pxscene", "SecondMarker",
												 fixture.Script("Scripts/Second.lua", R"(
		local S = {}
		function S:OnCreate() Game.Set("SecondStarted", Game.Get("SecondStarted", 0) + 1) end
		return S
	)"));
	const AssetHandle first = fixture.Script("Scripts/First.lua", R"(
		local S = {}
		function S:OnCreate()
			Game.Set("Starts", Game.Get("Starts", 0) + 1)
			self.entity:GetComponent("Transform").Translation = vec3(0, 0, 0)
		end
		function S:OnUpdate()
			local t = self.entity:GetComponent("Transform")
			t.Translation = t.Translation + vec3(1, 0, 0)
		end
		return S
	)");
	Scene start;
	Entity mover = start.CreateEntity("Mover");
	mover.AddComponent<ScriptComponent>().Script = first;

	GameSession game(fixture.Assets, start);
	game.Update(Timestep(1.0f / 60.0f));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetScene().FindEntityByName("Mover")->GetComponent<TransformComponent>().Translation.x == 2.0f);
	CHECK(start.FindEntityByName("Mover")->GetComponent<TransformComponent>().Translation.x == 0.0f); // A copy runs.

	// Reload: the level restarts as it began; the update that asked still completes first.
	std::string error;
	REQUIRE(game.GetScripts()->Evaluate("Scene.Reload()", error));
	CHECK(HasEntity(game, "Mover")); // Not yet: the switch waits for the end of the next update.
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetSceneSwitches() == 1);
	CHECK(game.GetScene().FindEntityByName("Mover")->GetComponent<TransformComponent>().Translation.x == 0.0f);
	game.Update(Timestep(1.0f / 60.0f));                           // The new scene's scripts start (OnCreate) in its first update.
	CHECK(std::get<int64_t>(*game.GetState().Get("Starts")) == 2); // Kept across the switch.

	// Load another scene by path; then Reload restarts that one, not the first.
	REQUIRE(game.GetScripts()->Evaluate("Scene.Load('Scenes/Second.pxscene')", error));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(HasEntity(game, "SecondMarker"));
	CHECK_FALSE(HasEntity(game, "Mover"));
	CHECK(game.GetSceneAsset() == second);
	game.Update(Timestep(1.0f / 60.0f));
	REQUIRE(game.GetScripts()->Evaluate("Scene.Reload()", error));
	game.Update(Timestep(1.0f / 60.0f));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(HasEntity(game, "SecondMarker"));
	CHECK(std::get<int64_t>(*game.GetState().Get("SecondStarted")) == 2);
	CHECK(game.GetSceneSwitches() == 3);

	// By handle too, and the last request of an update wins.
	REQUIRE(game.GetScripts()->Evaluate(fmt::format("Scene.Reload() Scene.Load('{}')", static_cast<uint64_t>(second)), error));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetSceneSwitches() == 4);

	// Unknown scenes and other asset types are script errors; nothing switches.
	CHECK_FALSE(game.GetScripts()->Evaluate("Scene.Load('Scenes/Missing.pxscene')", error));
	CHECK(error.find("no scene") != std::string::npos);
	CHECK_FALSE(game.GetScripts()->Evaluate("Scene.Load('Scripts/First.lua')", error));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetSceneSwitches() == 4);
}

TEST_CASE("GameSession: a scene file that fails to load keeps the game where it is, and errors outlive switches")
{
	SessionFixture fixture("GameSessionBroken");
	const AssetHandle broken = fixture.SceneFile("Scenes/Broken.pxscene", "Unused", AssetHandle(0));
	REQUIRE(WriteFileAtomically(fixture.Directory / "Scenes" / "Broken.pxscene", "{ not json"));
	const AssetHandle failing = fixture.Script("Scripts/Failing.lua", R"(
		local S = {}
		function S:OnCreate() error("first scene fails") end
		return S
	)");
	Scene start;
	start.CreateEntity("Failer").AddComponent<ScriptComponent>().Script = failing;
	GameSession game(fixture.Assets, start);
	game.Update(Timestep(1.0f / 60.0f));
	REQUIRE(game.GetScriptErrors().size() == 1);

	Paralax::Testing::ScopedLogCapture log;
	std::string error;
	REQUIRE(game.GetScripts()->Evaluate(fmt::format("Scene.Load('{}')", static_cast<uint64_t>(broken)), error));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetSceneSwitches() == 0);
	CHECK(HasEntity(game, "Failer"));
	CHECK(log.Contains(spdlog::level::err, "could not be loaded"));
	// Reported like a script error, so agents and --fail-on-script-error see it.
	const std::vector<std::string> errors = game.GetScriptErrors();
	REQUIRE(errors.size() == 2);
	CHECK(std::any_of(errors.begin(), errors.end(), [](const std::string& message) { return message.find("Scenes/Broken.pxscene") != std::string::npos; }));
	game.ClearScriptErrors();
	game.Update(Timestep(1.0f / 60.0f));

	// The failing scene's error is still reported after a reload starts fresh scripts.
	REQUIRE(game.GetScripts()->Evaluate("Scene.Reload()", error));
	game.Update(Timestep(1.0f / 60.0f));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetSceneSwitches() == 1);
	CHECK(game.GetScriptErrors().size() == 1); // The reloaded scene's run fails again.
	game.ClearScriptErrors();
	CHECK(game.GetScriptErrors().empty());

	// Pausing sound survives a switch.
	game.GetAudio()->SetPaused(true);
	REQUIRE(game.GetScripts()->Evaluate("Scene.Reload()", error));
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetAudio()->IsPaused());
}

TEST_CASE("GameSession: errors raised while a scene stops are kept, at a switch and at the end")
{
	SessionFixture fixture("GameSessionOnDestroy");
	const AssetHandle script = fixture.Script("Scripts/Leaving.lua", R"(
		local S = {}
		function S:OnDestroy()
			Scene.Reload() -- Ignored: the scene is already stopping.
			error("cannot leave")
		end
		return S
	)");
	Scene start;
	start.CreateEntity("Leaver").AddComponent<ScriptComponent>().Script = script;
	GameSession game(fixture.Assets, start);
	game.Update(Timestep(1.0f / 60.0f));
	std::string error;
	REQUIRE(game.GetScripts()->Evaluate("Scene.Reload()", error));
	game.Update(Timestep(1.0f / 60.0f)); // The old scene stops: its OnDestroy fails.
	CHECK(game.GetSceneSwitches() == 1);
	REQUIRE(game.GetScriptErrors().size() == 1);
	CHECK(game.GetScriptErrors()[0].find("cannot leave") != std::string::npos);
	game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetSceneSwitches() == 1); // The reload asked for in OnDestroy did nothing.

	game.End();
	CHECK(game.HasEnded());
	CHECK(game.GetScriptErrors().size() == 2); // The second run's OnDestroy, counted after ending.
	game.End();                                // Again: nothing.
	CHECK(game.GetScriptErrors().size() == 2);
}

TEST_CASE("GameSession: hostile Game values are refused, integers stay integers, and a scene may switch every update")
{
	SessionFixture fixture("GameSessionHostile");
	const AssetHandle restless = fixture.Script("Scripts/Restless.lua", R"(
		local S = {}
		function S:OnCreate()
			Game.Set("Starts", Game.Get("Starts", 0) + 1)
			Scene.Reload()
		end
		return S
	)");
	Scene start;
	start.CreateEntity("Restless").AddComponent<ScriptComponent>().Script = restless;
	GameSession game(fixture.Assets, start);
	for (int i = 0; i < 20; i++)
		game.Update(Timestep(1.0f / 60.0f));
	CHECK(game.GetSceneSwitches() == 20);
	CHECK(std::get<int64_t>(*game.GetState().Get("Starts")) == 20);
	CHECK(game.GetScriptErrors().empty());

	std::string error;
	CHECK(*game.GetScripts()->Evaluate("Game.Get('Starts'), math.type(Game.Get('Starts'))", error) == "20\tinteger");
	CHECK(*game.GetScripts()->Evaluate("Game.Set('Ratio', 0.5) return Game.Get('Ratio')", error) == "0.5");
	CHECK(*game.GetScripts()->Evaluate("Game.Set('Ratio', nil) return Game.Get('Ratio', 'gone')", error) == "gone");
	for (const char* hostile : {"Game.Set('Bad', 0/0)", "Game.Set('Bad', math.huge)", "Game.Set('Bad', {})", "Game.Set('Bad', print)", "Game.Set({}, 1)",
								"Game.Set('', 1)", "Game.Set('Bad', string.rep('x', 65537))", "Game.Set(string.rep('k', 257), 1)"})
	{
		CAPTURE(hostile);
		CHECK_FALSE(game.GetScripts()->Evaluate(hostile, error));
	}
	CHECK_FALSE(game.GetState().Get("Bad"));
}
