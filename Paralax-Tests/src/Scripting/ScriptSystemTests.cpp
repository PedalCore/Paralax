#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Audio/AudioSystem.h>
#include <Paralax/Core/Input.h>
#include <Paralax/Core/JsonText.h>
#include <Paralax/Core/PathEncoding.h>
#include <Paralax/Events/KeyEvent.h>
#include <Paralax/Physics/PhysicsSystem.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Prefab.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scripting/ScriptAsset.h>
#include <Paralax/Scripting/ScriptSystem.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

using namespace Paralax;

namespace {

	// A running scene with a script system and in-memory script assets.
	class ScriptFixture
	{
	public:
		explicit ScriptFixture(ScriptSystemSpecification specification = {}, std::filesystem::path assetDirectory = {})
			: Assets(std::move(assetDirectory))
		{
			System = static_cast<ScriptSystem*>(World.AddSystem(CreateScope<ScriptSystem>(Assets, specification)));
		}

		~ScriptFixture() { World.OnRuntimeStop(); }

		AssetHandle AddScript(const std::string& source, const std::string& name = "Test.lua")
		{
			Ref<ScriptAsset> script = ScriptAsset::Create(name, source);
			script->Handle = AssetHandle(m_NextHandle++);
			Assets.AddMemoryAsset(script);
			return script->Handle;
		}

		Entity CreateScripted(const std::string& source, const std::string& name = "Scripted")
		{
			Entity entity = World.CreateEntity(name);
			entity.AddComponent<ScriptComponent>().Script = AddScript(source, name + ".lua");
			return entity;
		}

		void Start() { World.OnRuntimeStart(); }
		void Update(int frames = 1, float dt = 1.0f / 60.0f)
		{
			for (int i = 0; i < frames; i++)
				World.OnUpdateRuntime(Timestep(dt));
		}

		// Evaluates an expression in the script environment; fails the test on error.
		std::string Eval(const std::string& code)
		{
			std::string error;
			std::optional<std::string> result = System->Evaluate(code, error);
			REQUIRE_MESSAGE(result, error);
			return *result;
		}

		bool HasError(const std::string& substring) const
		{
			for (const std::string& error : System->GetErrors())
			{
				if (error.find(substring) != std::string::npos)
					return true;
			}
			return false;
		}

		AssetManager Assets;
		Scene World{"ScriptTest"};
		ScriptSystem* System = nullptr;

	private:
		uint64_t m_NextHandle = 5000;
	};

	std::string NameOf(Scene& scene, const std::string& name)
	{
		std::optional<Entity> entity = scene.FindEntityByName(name);
		return entity ? entity->GetName() : std::string();
	}

}

TEST_CASE("ScriptSystem: OnCreate runs once, OnUpdate every frame with dt, OnDestroy on destruction")
{
	ScriptFixture fixture;
	Entity entity = fixture.CreateScripted(R"(
		local Counter = {}
		function Counter:OnCreate() Calls = (Calls or "") .. "C"; self.updates = 0 end
		function Counter:OnUpdate(dt) Calls = Calls .. "U"; self.updates = self.updates + 1; LastDt = dt end
		function Counter:OnDestroy() Calls = Calls .. "D" end
		return Counter
	)");
	fixture.Start();
	CHECK(fixture.System->GetInstanceCount() == 0); // Instances are created by the first update.
	fixture.Update(3, 0.25f);
	CHECK(fixture.Eval("Calls") == "CUUU");
	CHECK(fixture.Eval("LastDt") == "0.25");
	CHECK(fixture.Eval("Time.frame") == "3");
	CHECK(fixture.Eval("Time.elapsed") == "0.75");
	CHECK(fixture.System->GetInstanceCount() == 1);

	fixture.World.DestroyEntity(entity);
	CHECK(fixture.Eval("Calls") == "CUUUD");
	CHECK(fixture.System->GetInstanceCount() == 0);
	CHECK(fixture.System->GetErrors().empty());
}

TEST_CASE("ScriptSystem: stopping the scene calls OnDestroy in hierarchy order and releases the scripts")
{
	ScriptFixture fixture;
	const std::string source = R"(
		local S = {}
		function S:OnDestroy() Order = (Order or "") .. self.entity.name .. ";" end
		return S
	)";
	Entity parent = fixture.CreateScripted(source, "Parent");
	Entity child = fixture.CreateScripted(source, "Child");
	fixture.World.SetParent(child, parent);
	fixture.Start();
	fixture.Update();
	// Destroying the parent destroys the child: parents are notified before children.
	fixture.World.DestroyEntity(parent);
	CHECK(fixture.Eval("Order") == "Parent;Child;");

	fixture.CreateScripted(source, "Late");
	fixture.Update();
	fixture.World.OnRuntimeStop();
	CHECK(fixture.System->GetInstanceCount() == 0);
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("1", error)); // The Lua state is gone.
	CHECK(error.find("not running") != std::string::npos);
}

TEST_CASE("ScriptSystem: scripts read and write components through reflection")
{
	ScriptFixture fixture;
	Entity entity = fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			local transform = self.entity:GetComponent("Transform")
			transform.Translation = vec3(1, 2, 3)
			transform.Scale = { 2, 2, 2 }
			self.entity.name = "Renamed"
			self.entity.tag = "Player"
			local light = self.entity:AddComponent("PointLight", { Intensity = 5, Color = vec3(1, 0.5, 0.25) })
			Read = tostring(transform.Translation) .. "|" .. light.Intensity .. "|" .. light.type
			HasMesh = self.entity:HasComponent("Mesh")
			Missing = self.entity:GetComponent("Mesh") == nil
		end
		return S
	)");
	fixture.Start();
	fixture.Update();
	REQUIRE(fixture.System->GetErrors().empty());

	const TransformComponent& transform = entity.GetComponent<TransformComponent>();
	CHECK(transform.Translation == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(transform.Scale == glm::vec3(2.0f));
	CHECK(entity.GetName() == "Renamed");
	CHECK(entity.GetComponent<TagComponent>().Tag == "Player");
	REQUIRE(entity.HasComponent<PointLightComponent>());
	CHECK(entity.GetComponent<PointLightComponent>().Intensity == 5.0f);
	CHECK(fixture.Eval("Read") == "vec3(1, 2, 3)|5.0|PointLight");
	CHECK(fixture.Eval("HasMesh") == "false");
	CHECK(fixture.Eval("Missing") == "true");

	CHECK(fixture.Eval("Scene.FindEntityByName('Renamed'):RemoveComponent('PointLight')") == "true");
	CHECK_FALSE(entity.HasComponent<PointLightComponent>());
}

TEST_CASE("ScriptSystem: invalid component access raises a script error and changes nothing")
{
	ScriptFixture fixture;
	Entity entity = fixture.World.CreateEntity("Target");
	fixture.Start();
	fixture.Update();
	const std::string id = std::to_string(static_cast<uint64_t>(entity.GetUUID()));
	auto fails = [&](const std::string& code, const std::string& expected) {
		std::string error;
		CHECK_FALSE(fixture.System->Evaluate(code, error));
		CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
	};
	const std::string target = "Scene.GetEntity('" + id + "')";
	fails(target + ":GetComponent('Transform').Translation = 5", "expects a vec3");
	fails(target + ":GetComponent('Transform').Bogus = 1", "no field 'Bogus'");
	fails(target + ":GetComponent('Nope')", "unknown component 'Nope'");
	fails(target + ":AddComponent('PointLight', { Intensity = -4 })", "PointLight.Intensity");
	fails(target + ":RemoveComponent('Transform')", "cannot be removed");
	fails(target + ".name = ''", "cannot be empty");
	fails(target + ":AddComponent('PrefabInstance')", "cannot be added");
	CHECK_FALSE(entity.HasComponent<PointLightComponent>());
	CHECK(entity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
}

namespace {

	// Evaluates code that must fail and returns its error, which must be short and well formed whatever the code
	// sent. (The Lua error adds a traceback; the message itself is a few hundred bytes at most.)
	std::string ExpectShortError(ScriptFixture& fixture, const std::string& code)
	{
		constexpr size_t ShortErrorBytes = 1024;
		std::string error;
		std::optional<std::string> result;
		CHECK_NOTHROW(result = fixture.System->Evaluate(code, error));
		CHECK_FALSE(result);
		REQUIRE_MESSAGE(error.size() < ShortErrorBytes, error.substr(0, ShortErrorBytes));
		CHECK(IsWellFormedUtf8(error));
		return error;
	}

	std::string EntityExpression(Entity entity)
	{
		return "Scene.GetEntity('" + std::to_string(static_cast<uint64_t>(entity.GetUUID())) + "')";
	}

}

TEST_CASE("ScriptSystem: a refused value of any size or bytes gives a short script error, not an exception")
{
	ScriptFixture fixture;
	Entity entity = fixture.World.CreateEntity("Target");
	entity.AddComponent<CameraComponent>();
	fixture.Start();
	fixture.Update();
	const std::string target = EntityExpression(entity);
	auto fails = [&](const std::string& code) { return ExpectShortError(fixture, code); };
	const std::string replacement = "\xEF\xBF\xBD";

	// A Lua string reaches SetField as it is, whatever its bytes: one that is not UTF-8 is shown with U+FFFD.
	std::string error = fails(target + ":GetComponent('Camera').Projection = 'Fish\\xFFeye'");
	CHECK_MESSAGE(error.find("Camera.Projection: Field 'Projection' expects one of: Perspective, Orthographic, got \"Fish" + replacement + "eye\" (string)") != std::string::npos, error);
	error = fails(target + ":AddComponent('Camera', { Projection = string.rep('\\xFF', 1 << 24) })");
	CHECK_MESSAGE(error.find("Camera.Projection: Field 'Projection' expects one of: Perspective, Orthographic, got \"" + replacement) != std::string::npos, error);
	CHECK_MESSAGE(error.find("... (string of 16777216 bytes)") != std::string::npos, error);
	CHECK(entity.GetComponent<CameraComponent>().Camera.Projection == SceneCamera::ProjectionType::Perspective);

	// A 16 MiB name, refused for its length.
	error = fails(target + ":GetComponent('Tag').Name = string.rep('n', 1 << 24)");
	CHECK_MESSAGE(error.find("Tag.Name: Field 'Name' expects at most 256 bytes, got \"nnnn") != std::string::npos, error);
	CHECK_MESSAGE(error.find("... (string of 16777216 bytes)") != std::string::npos, error);
	CHECK(entity.GetName() == "Target");
}

TEST_CASE("ScriptSystem: an unknown field or component of any size is named briefly")
{
	ScriptFixture fixture;
	Entity entity = fixture.World.CreateEntity("Target");
	fixture.Start();
	fixture.Update();
	const std::string target = EntityExpression(entity);
	auto fails = [&](const std::string& code) { return ExpectShortError(fixture, code); };

	// Writing, reading and adding a field.
	std::string error = fails(target + ":GetComponent('Transform')[string.rep('k', 1 << 20)] = 1");
	CHECK_MESSAGE(error.find("component 'Transform' has no field 'kkkk") != std::string::npos, error);
	CHECK_MESSAGE(error.find("...' (1048576 bytes)") != std::string::npos, error);
	error = fails(target + ":GetComponent('Transform')[string.rep('k', 1 << 20)]");
	CHECK_MESSAGE(error.find("component 'Transform' has no field 'kkkk") != std::string::npos, error);
	CHECK_MESSAGE(error.find("...' (1048576 bytes)") != std::string::npos, error);
	error = fails(target + ":AddComponent('Transform', { [string.rep('k', 1 << 20)] = 1 })");
	CHECK_MESSAGE(error.find("...' (1048576 bytes)") != std::string::npos, error);
	error = fails(target + ":GetComponent(string.rep('c', 1 << 20) .. '\\xFF')");
	CHECK_MESSAGE(error.find("unknown component 'cccc") != std::string::npos, error);
	CHECK_MESSAGE(error.find("...' (1048577 bytes)") != std::string::npos, error);
}

TEST_CASE("ScriptSystem: a script that sends a refused value of bytes that are not UTF-8 reports a script error")
{
	ScriptFixture fixture;
	Entity entity = fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			self.entity:AddComponent('Camera', { Projection = 'Wide\xFF' })
		end
		return S
	)");
	CHECK_NOTHROW(fixture.Start());
	CHECK_NOTHROW(fixture.Update());
	REQUIRE(fixture.System->GetErrors().size() == 1);
	const std::string& error = fixture.System->GetErrors()[0];
	CHECK(IsWellFormedUtf8(error));
	CHECK_MESSAGE(error.find("got \"Wide\xEF\xBF\xBD\" (string)") != std::string::npos, error);
	CHECK_FALSE(entity.HasComponent<CameraComponent>());
}

TEST_CASE("ScriptSystem: the Scene API creates, finds, parents and destroys entities")
{
	ScriptFixture fixture;
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			local root = Scene.CreateEntity("Root")
			local child = Scene.CreateEntity("Child", root)
			child.tag = "Enemy"
			Scene.CreateEntity("Other").tag = "Enemy"
			ChildParent = child:GetParent() == root
			Children = #root:GetChildren()
			Enemies = #Scene.FindEntitiesByTag("Enemy")
			Count = Scene.GetEntityCount()
			Same = Scene.GetEntity(root.id) == root
			Unknown = Scene.GetEntity("123") == nil and Scene.FindEntityByName("Nobody") == nil
			child:SetParent(nil)
			RootChildren = #root:GetChildren()
			root:Destroy() -- Deferred until the update finishes.
			StillThere = root:IsValid()
		end
		return S
	)");
	fixture.Start();
	fixture.Update();
	REQUIRE(fixture.System->GetErrors().empty());
	CHECK(fixture.Eval("ChildParent") == "true");
	CHECK(fixture.Eval("Children") == "1");
	CHECK(fixture.Eval("Enemies") == "2");
	CHECK(fixture.Eval("Count") == "4");
	CHECK(fixture.Eval("Same") == "true");
	CHECK(fixture.Eval("Unknown") == "true");
	CHECK(fixture.Eval("RootChildren") == "0");
	CHECK(fixture.Eval("StillThere") == "true");
	CHECK_FALSE(fixture.World.FindEntityByName("Root"));
	CHECK(fixture.World.FindEntityByName("Child"));
	CHECK(fixture.World.GetEntityCount() == 3);
}

TEST_CASE("ScriptSystem: references to destroyed entities fail clearly")
{
	ScriptFixture fixture;
	Entity target = fixture.World.CreateEntity("Doomed");
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate() Target = Scene.FindEntityByName("Doomed") end
		return S
	)");
	fixture.Start();
	fixture.Update();
	fixture.World.DestroyEntity(target);
	CHECK(fixture.Eval("Target:IsValid()") == "false");
	CHECK(fixture.Eval("tostring(Target)").find("destroyed") != std::string::npos);
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Target.name", error));
	CHECK(error.find("no longer exists") != std::string::npos);
}

TEST_CASE("ScriptSystem: prefabs are instantiated by handle or path with a parent and position")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("ScriptPrefabs");
	ScriptFixture fixture({}, directory);

	// Build a prefab (root with one child) from entities in a scratch scene and save it in the project.
	Scene source;
	Entity crate = source.CreateEntity("Crate");
	crate.GetComponent<TransformComponent>().Translation = glm::vec3(9.0f);
	Entity lid = source.CreateEntity("Lid");
	source.SetParent(lid, crate);
	Ref<PrefabAsset> prefab = PrefabAsset::CreateFromEntity(source, crate);
	REQUIRE(prefab->SaveToFile(directory / "Crate.pxprefab"));
	const AssetHandle handle = fixture.Assets.ImportAsset("Crate.pxprefab");
	REQUIRE(handle.IsValid());

	fixture.World.CreateEntity("Holder");
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			ByPath = Scene.Instantiate("Crate.pxprefab", { position = vec3(1, 0, 0) })
			ByHandle = Scene.Instantiate(PrefabHandle, { parent = Scene.FindEntityByName("Holder") })
		end
		return S
	)");
	fixture.Start();
	fixture.Eval("PrefabHandle = '" + std::to_string(static_cast<uint64_t>(handle)) + "'");
	fixture.Update();
	REQUIRE(fixture.System->GetErrors().empty());

	CHECK(fixture.Eval("ByPath.name") == "Crate");
	CHECK(fixture.Eval("tostring(ByPath:GetComponent('Transform').Translation)") == "vec3(1, 0, 0)");
	CHECK(fixture.Eval("#ByPath:GetChildren()") == "1");
	CHECK(fixture.Eval("ByHandle:GetParent().name") == "Holder");
	CHECK(fixture.Eval("tostring(ByHandle:GetComponent('Transform').Translation)") == "vec3(9, 9, 9)");
	CHECK(fixture.Eval("ByPath:GetComponent('PrefabInstance').Prefab") == std::to_string(static_cast<uint64_t>(handle)));
	CHECK(fixture.Eval("ByPath.id ~= ByHandle.id") == "true");

	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Scene.Instantiate('Missing.pxprefab')", error));
	CHECK(error.find("no prefab") != std::string::npos);
	CHECK_FALSE(fixture.System->Evaluate("Scene.Instantiate('Crate.pxprefab', { position = 3 })", error));
	CHECK(error.find("position must be a vec3") != std::string::npos);
	// The position is written past reflection, so the binding applies reflection's finiteness rule itself; nothing is
	// instantiated when it fails.
	const std::string count = fixture.Eval("Scene.GetEntityCount()");
	for (const char* position : {"vec3(0/0)", "vec3(math.huge)", "vec3(0, -math.huge, 0)", "vec3(0, 0, 0/0)"})
	{
		CAPTURE(position);
		CHECK_FALSE(fixture.System->Evaluate(std::string("Scene.Instantiate('Crate.pxprefab', { position = ") + position + " })", error));
		CHECK(error.find("position must be finite") != std::string::npos);
	}
	CHECK(fixture.Eval("Scene.GetEntityCount()") == count);

	// Paths from scripts are UTF-8 on every platform ("Kiste-é.pxprefab").
	const std::string utf8Name = "Kiste-\xC3\xA9.pxprefab";
	REQUIRE(prefab->SaveToFile(directory / FromUtf8(utf8Name)));
	REQUIRE(fixture.Assets.ImportAsset(FromUtf8(utf8Name)).IsValid());
	CHECK(fixture.Eval("Scene.Instantiate('" + utf8Name + "').name") == "Crate");
	// Bytes that are not UTF-8 name no file, and are an error rather than an exception.
	CHECK_FALSE(fixture.System->Evaluate("Scene.Instantiate('Kiste-\xFF.pxprefab')", error));
	CHECK(error.find("no prefab") != std::string::npos);
}

TEST_CASE("ScriptSystem: a failing callback disables its instance with a traceback naming the script")
{
	ScriptFixture fixture;
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnUpdate() Updates = (Updates or 0) + 1; if Updates == 2 then error("boom") end end
		return S
	)",
						   "Exploder");
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnUpdate() Healthy = (Healthy or 0) + 1 end
		return S
	)",
						   "Healthy");
	Paralax::Testing::ScopedLogCapture log;
	fixture.Start();
	fixture.Update(4);
	CHECK(fixture.Eval("Updates") == "2"); // Disabled after the error.
	CHECK(fixture.Eval("Healthy") == "4"); // Other scripts keep running.
	REQUIRE(fixture.System->GetErrors().size() == 1);
	const std::string& error = fixture.System->GetErrors().front();
	CHECK(error.find("Exploder.lua") != std::string::npos);
	CHECK(error.find("OnUpdate") != std::string::npos);
	CHECK(error.find("boom") != std::string::npos);
	CHECK(error.find("stack traceback") != std::string::npos);
	CHECK(log.Contains(spdlog::level::err, "boom"));
}

TEST_CASE("ScriptSystem: scripts that fail to load are reported once and their entities skipped")
{
	ScriptFixture fixture;
	fixture.CreateScripted("this is not lua", "Syntax");
	fixture.CreateScripted("return 42", "NotATable");
	fixture.CreateScripted("local x = 1", "ReturnsNothing");
	fixture.CreateScripted("error('at load')", "Throws");
	Entity missing = fixture.World.CreateEntity("Missing");
	missing.AddComponent<ScriptComponent>().Script = AssetHandle(424242);
	fixture.Start();
	fixture.Update(3);
	CHECK(fixture.HasError("Syntax.lua"));
	CHECK(fixture.HasError("NotATable.lua: a script must return a table"));
	CHECK(fixture.HasError("ReturnsNothing.lua: a script must return a table"));
	CHECK(fixture.HasError("at load"));
	CHECK(fixture.HasError("script asset 424242 not found"));
	CHECK(fixture.System->GetErrors().size() == 5); // Each once, despite three updates.
}

TEST_CASE("ScriptSystem: the sandbox removes file, OS and code-loading access")
{
	ScriptFixture fixture;
	fixture.Start();
	for (const char* name : {"io", "os", "debug", "package", "load", "loadfile", "dofile", "collectgarbage", "string.dump", "rawset"})
	{
		CAPTURE(name);
		CHECK(fixture.Eval(std::string("type(") + name + ")") == "nil");
	}
	// The safe standard libraries remain.
	CHECK(fixture.Eval("string.upper('ok') .. math.floor(2.5) .. #table.pack(1, 2) .. utf8.char(65)") == "OK22A");
}

TEST_CASE("ScriptSystem: an endless loop is stopped by the instruction budget")
{
	ScriptSystemSpecification specification;
	specification.InstructionBudget = 1'000'000;
	ScriptFixture fixture(specification);
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnUpdate() while true do end end
		return S
	)",
						   "Spin");
	fixture.Start();
	fixture.Update();
	CHECK(fixture.HasError("instruction budget"));
	// The budget is per callback: later code runs normally.
	CHECK(fixture.Eval("1 + 1") == "2");
}

TEST_CASE("ScriptSystem: allocations beyond the memory limit fail inside the script")
{
	ScriptSystemSpecification specification;
	specification.MemoryLimitBytes = 4 * 1024 * 1024;
	ScriptFixture fixture(specification);
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate() local t = {} for i = 1, 10000000 do t[i] = string.rep("x", 64) .. i end end
		return S
	)",
						   "Hog");
	fixture.Start();
	fixture.Update();
	CHECK(fixture.HasError("not enough memory"));
	CHECK(fixture.Eval("'still alive'") == "still alive");
}

TEST_CASE("ScriptSystem: math.random is deterministic for a seed")
{
	auto sequence = [](uint64_t seed) {
		ScriptSystemSpecification specification;
		specification.RandomSeed = seed;
		ScriptFixture fixture(specification);
		fixture.Start();
		return fixture.Eval("math.random(1, 1000000), math.random(6), math.floor(math.random() * 1e9)");
	};
	CHECK(sequence(7) == sequence(7));
	CHECK(sequence(7) != sequence(8));

	ScriptFixture fixture;
	fixture.Start();
	const std::string first = fixture.Eval("(function() math.randomseed(99) return math.random(1, 1000000) end)()");
	CHECK(fixture.Eval("(function() math.randomseed(99) return math.random(1, 1000000) end)()") == first);
	for (int i = 0; i < 200; i++)
	{
		const int value = std::stoi(fixture.Eval("math.random(3, 5)"));
		CHECK((value >= 3 && value <= 5));
	}
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("math.random(5, 3)", error));
	CHECK_FALSE(fixture.System->Evaluate("math.random(1.5)", error));
}

TEST_CASE("ScriptSystem: require loads project scripts once and detects cycles")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("ScriptRequire");
	auto write = [&](const std::string& name, const std::string& source) {
		std::filesystem::create_directories((directory / name).parent_path());
		std::ofstream(directory / name) << source;
	};
	write("Scripts/Util.lua", "Loads = (Loads or 0) + 1\nreturn { Double = function(x) return x * 2 end }");
	write("Scripts/A.lua", "return require('Scripts/B')");
	write("Scripts/B.lua", "return require('Scripts/A')");
	// A module named in UTF-8 ("Scripts/Hilfe-é.lua"), whatever the platform's narrow encoding.
	const std::string utf8Module = "Scripts/Hilfe-\xC3\xA9";
	std::ofstream(directory / FromUtf8(utf8Module + ".lua")) << "return 7";
	ScriptFixture fixture({}, directory);
	for (const char* name : {"Scripts/Util.lua", "Scripts/A.lua", "Scripts/B.lua"})
		REQUIRE(fixture.Assets.ImportAsset(name).IsValid());
	REQUIRE(fixture.Assets.ImportAsset(FromUtf8(utf8Module + ".lua")).IsValid());
	fixture.Start();
	CHECK(fixture.Eval("require('" + utf8Module + "')") == "7");

	CHECK(fixture.Eval("require('Scripts/Util').Double(21)") == "42");
	CHECK(fixture.Eval("require('Scripts/Util.lua').Double(1)") == "2");
	CHECK(fixture.Eval("Loads") == "1");
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("require('Scripts/A')", error));
	CHECK(error.find("circular") != std::string::npos);
	CHECK_FALSE(fixture.System->Evaluate("require('Scripts/Nope')", error));
	CHECK(error.find("no script") != std::string::npos);
}

TEST_CASE("ScriptSystem: Evaluate returns expression results and reports errors")
{
	ScriptFixture fixture;
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("1", error)); // Not running yet.
	fixture.Start();
	CHECK(fixture.Eval("1 + 2, 'x', nil") == "3\tx\tnil");
	CHECK(fixture.Eval("Value = 5") == "");
	CHECK(fixture.Eval("Value") == "5");
	CHECK_FALSE(fixture.System->Evaluate("(", error));
	CHECK_FALSE(fixture.System->Evaluate("error('bad')", error));
	CHECK(error.find("bad") != std::string::npos);
}

TEST_CASE("ScriptSystem: math types support arithmetic and conversions")
{
	ScriptFixture fixture;
	fixture.Start();
	CHECK(fixture.Eval("tostring(vec3(1, 2, 3) + vec3(1) * 2)") == "vec3(3, 4, 5)");
	CHECK(fixture.Eval("tostring(2 * vec2(1, 2) - vec2(1, 1))") == "vec2(1, 3)");
	CHECK(fixture.Eval("vec3(3, 4, 0):Length()") == "5.0");
	CHECK(fixture.Eval("vec3(1, 0, 0):Dot(vec3(0, 1, 0))") == "0.0");
	CHECK(fixture.Eval("tostring(vec3(1, 0, 0):Cross(vec3(0, 1, 0)))") == "vec3(0, 0, 1)");
	CHECK(fixture.Eval("tostring(-vec4(1, 2, 3, 4))") == "vec4(-1, -2, -3, -4)");
	// Every vector type has the whole documented set: division by a vector or a scalar, and Normalized (the zero vector
	// stays zero rather than becoming NaN).
	CHECK(fixture.Eval("tostring(vec2(2, 4) / 2)") == "vec2(1, 2)");
	CHECK(fixture.Eval("tostring(vec3(2, 4, 6) / vec3(2))") == "vec3(1, 2, 3)");
	CHECK(fixture.Eval("tostring(vec4(2, 4, 6, 8) / 2)") == "vec4(1, 2, 3, 4)");
	CHECK(fixture.Eval("tostring(vec4(2, 4, 6, 8) / vec4(1, 2, 3, 4))") == "vec4(2, 2, 2, 2)");
	CHECK(fixture.Eval("tostring(vec4(0, 3, 0, 0):Normalized())") == "vec4(0, 1, 0, 0)");
	CHECK(fixture.Eval("math.abs(vec4(1, 2, 3, 4):Normalized():Length() - 1) < 1e-6") == "true");
	CHECK(fixture.Eval("tostring(vec2():Normalized()) .. tostring(vec3():Normalized()) .. tostring(vec4():Normalized())") == "vec2(0, 0)vec3(0, 0, 0)vec4(0, 0, 0, 0)");
	// Division by zero gives the IEEE result per component, as float division does: +-inf, and NaN for 0/0 (checked
	// as x ~= x, since how NaN prints differs between platforms).
	for (const char* type : {"vec2", "vec3", "vec4"})
	{
		CAPTURE(type);
		CHECK(fixture.Eval(std::string("local v = ") + type + "(-2) / 0 return v.x == -math.huge and v.y == -math.huge") == "true");
		CHECK(fixture.Eval(std::string("local v = ") + type + "(1) / " + type + "(0) return v.x == math.huge and v.y == math.huge") == "true");
		CHECK(fixture.Eval(std::string("local v = ") + type + "(0) / 0 return v.x ~= v.x and v.y ~= v.y") == "true");
	}
	// A scalar divided by a vector is not defined (only vector / vector and vector / scalar are).
	std::string divisionError;
	CHECK_FALSE(fixture.System->Evaluate("return 2 / vec4(1)", divisionError));
	CHECK_FALSE(fixture.System->Evaluate("return 2 / vec2(1)", divisionError));
	CHECK(fixture.Eval("vec3(1, 2, 3) == vec3(1, 2, 3)") == "true");
	CHECK(fixture.Eval("local v = vec3(); v.y = 7; return v.y") == "7.0");

	// A quarter turn about +Y maps +X to -Z (right-handed).
	CHECK(fixture.Eval("local r = quat.AngleAxis(math.pi / 2, vec3(0, 1, 0)) * vec3(1, 0, 0); return math.abs(r.z + 1) < 1e-5 and math.abs(r.x) < 1e-5") ==
		  "true");
	CHECK(fixture.Eval("local q = quat.FromEuler(vec3(0.1, 0.2, 0.3)); local e = q:ToEuler(); return math.abs(e.y - 0.2) < 1e-5") == "true");
	CHECK(fixture.Eval("quat().w") == "1.0");
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("quat.AngleAxis(1, vec3(0))", error));
}

TEST_CASE("ScriptSystem: vec2, vec3 and vec4 bind the same methods and operators")
{
	// The docs describe the vector types together, and the feature-test coverage check cannot tell which type a
	// ":Length(" call is on, nor see operators, so the shared surface is pinned here type by type.
	ScriptFixture fixture;
	fixture.Start();
	for (const char* type : {"vec2", "vec3", "vec4"})
	{
		CAPTURE(type);
		const std::string make = std::string("local T = ") + type + " local a, b = T(1), T(2) ";
		for (const char* method : {"Length", "Normalized", "Dot", "Lerp"})
		{
			CAPTURE(method);
			CHECK(fixture.Eval(make + "return type(a." + method + ")") == "function");
		}
		// __add, __sub, __mul (by a vector, or a scalar on either side), __div (by a vector or a scalar), __unm, __eq,
		// __tostring: each used once.
		CHECK(fixture.Eval(make + "return a + b == T(3) and b - a == T(1) and a * b == T(2) and b * 2 == T(4) and 2 * b == T(4) and b / b == T(1) and b / 2 == T(1) and -a == T(-1)") ==
			  "true");
		CHECK(fixture.Eval(make + "return a == T(1) and a ~= b") == "true");
		CHECK(fixture.Eval(make + "return tostring(a):sub(1, #'" + type + "(') == '" + type + "('") == "true");
	}
}

TEST_CASE("ScriptSystem: scripts read input state")
{
	Input::Reset();
	ScriptFixture fixture;
	fixture.Start();
	CHECK(fixture.Eval("Input.IsKeyDown(Key.W)") == "false");
	Input::OnEvent(KeyPressedEvent(Key::W));
	CHECK(fixture.Eval("Input.IsKeyDown(Key.W), Input.WasKeyPressed(Key.W)") == "true\ttrue");
	CHECK(fixture.Eval("Input.IsMouseButtonDown(Mouse.Left)") == "false");
	CHECK(fixture.Eval("tostring(Input.GetMousePosition())").starts_with("vec2("));
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Input.IsKeyDown(99999)", error));
	CHECK_FALSE(fixture.System->Evaluate("Input.IsMouseButtonDown(-1)", error));
	Input::Reset();
}

TEST_CASE("ScriptSystem: scripts reach each other's instances and entities created while running start next update")
{
	ScriptFixture fixture;
	const AssetHandle spawned = fixture.AddScript(R"(
		local S = {}
		function S:OnCreate() Spawned = Time.frame end
		return S
	)",
												  "Spawned.lua");
	fixture.CreateScripted(R"(
		local S = { health = 10 }
		function S:OnCreate() self.health = 20 end
		return S
	)",
						   "Target");
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnUpdate()
			if Time.frame == 1 then
				local target = Scene.FindEntityByName("Target")
				Health = target:GetScript().health
				Scene.CreateEntity("Child"):AddComponent("Script", { Script = SpawnedScript })
			end
		end
		return S
	)",
						   "Spawner");
	fixture.Start();
	fixture.Eval("SpawnedScript = '" + std::to_string(static_cast<uint64_t>(spawned)) + "'");
	fixture.Update(2);
	REQUIRE(fixture.System->GetErrors().empty());
	CHECK(fixture.Eval("Health") == "20");
	CHECK(fixture.Eval("Spawned") == "2");
}

TEST_CASE("ScriptSystem: removing or replacing a ScriptComponent destroys the old instance")
{
	ScriptFixture fixture;
	const std::string source = R"(
		local S = {}
		function S:OnCreate() Created = (Created or 0) + 1 end
		function S:OnDestroy() Destroyed = (Destroyed or 0) + 1 end
		return S
	)";
	Entity entity = fixture.CreateScripted(source);
	fixture.Start();
	fixture.Update();
	entity.GetComponent<ScriptComponent>().Script = fixture.AddScript(source, "Other.lua");
	fixture.Update();
	CHECK(fixture.Eval("Created, Destroyed") == "2\t1");
	entity.RemoveComponent<ScriptComponent>();
	fixture.Update();
	CHECK(fixture.Eval("Created, Destroyed") == "2\t2");
	CHECK(fixture.System->GetInstanceCount() == 0);
	CHECK(NameOf(fixture.World, "Scripted") == "Scripted");
}

// --- Hostile scripts: each case below crashed, corrupted memory or hung before it was fixed. ---------------------

TEST_CASE("ScriptSystem: engine metatables and __gc metamethods cannot be abused")
{
	ScriptFixture fixture;
	fixture.Start();
	// Metatables of userdata and strings are hidden; tables keep theirs.
	CHECK(fixture.Eval("getmetatable(vec3()) == nil and getmetatable('') == nil and getmetatable(Scene.CreateEntity()) == nil") == "true");
	CHECK(fixture.Eval("local mt = {} return getmetatable(setmetatable({}, mt)) == mt") == "true");
	// The type-unchecked destructors sol2 installs are replaced by checked no-ops: junk arguments are errors.
	for (const char* call : {"vec3().__gc(Scene)", "vec2().__gc({})", "vec4().__gc(1)", "quat().__gc('x')", "Scene.CreateEntity().__gc({})",
							 "Scene.CreateEntity():GetComponent('Transform').__gc(vec3())", "vec3().__index(Scene, 'x')", "vec3().__newindex(Scene, 'x', 1)"})
	{
		CAPTURE(call);
		std::string error;
		CHECK_FALSE(fixture.System->Evaluate(call, error));
	}
	// Finalizers would run script code at arbitrary points (including engine shutdown).
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("setmetatable({}, { __gc = function() end })", error));
	CHECK(error.find("__gc") != std::string::npos);
	CHECK(fixture.Eval("1") == "1");
}

TEST_CASE("ScriptSystem: Lua errors in engine-side calls are reported, never thrown into the engine")
{
	ScriptFixture fixture;
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnUpdate()
			Time = 5                                                                   -- The engine keeps its own reference.
			setmetatable(self, { __index = function() error("hostile __index") end }) -- Callback lookup now fails.
		end
		return S
	)",
						   "Hostile");
	fixture.Start();
	CHECK_NOTHROW(fixture.Update(3));
	CHECK(fixture.HasError("hostile __index"));
	CHECK(fixture.Eval("type(Time)") == "number"); // The script's own global...
	CHECK_NOTHROW(fixture.Update());               // ...does not break the engine's Time updates.
}

TEST_CASE("ScriptSystem: running out of script memory never throws into the engine")
{
	ScriptSystemSpecification specification;
	specification.MemoryLimitBytes = 4 * 1024 * 1024;
	ScriptFixture fixture(specification);
	const AssetHandle other = fixture.AddScript("return { OnCreate = function() end }", "Other.lua");
	fixture.CreateScripted(R"(
		local S = {}
		Hoard = {}
		function S:OnUpdate()
			if Time.frame ~= 1 then return end
			for i = 1, 20 do Scene.CreateEntity("Spawned" .. i):AddComponent("Script", { Script = OtherScript }) end
			local n = 0
			pcall(function() while true do n = n + 1 Hoard[n] = {} end end)
			pcall(function() while true do n = n + 1 Hoard[n] = n .. "" end end)
		end
		return S
	)",
						   "Hog");
	fixture.Start();
	fixture.Eval("OtherScript = '" + std::to_string(static_cast<uint64_t>(other)) + "'");
	CHECK_NOTHROW(fixture.Update(3));
	CHECK(fixture.HasError("not enough memory"));
	CHECK_NOTHROW(fixture.World.OnRuntimeStop());
}

TEST_CASE("ScriptSystem: field writes never use component storage invalidated by script code")
{
	ScriptFixture fixture;
	Entity a = fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			local e = self.entity
			local mesh = e:GetComponent("Mesh")
			-- Metamethods on the assigned value must not run (they would remove the component mid-write).
			local evil = setmetatable({ "5", "6" }, { __len = function() e:RemoveComponent("Mesh") return 2 end,
			                                          __index = function(t, k) return tostring(900 + k) end })
			mesh.Materials = evil
		end
		return S
	)",
									  "Evil");
	a.AddComponent<MeshComponent>().Materials = {AssetHandle(1)};
	Entity b = fixture.World.CreateEntity("B");
	b.AddComponent<MeshComponent>().Materials = {AssetHandle(2), AssetHandle(3)};
	fixture.Start();
	fixture.Update();
	REQUIRE(fixture.System->GetErrors().empty());
	REQUIRE(a.HasComponent<MeshComponent>()); // __len did not run.
	CHECK(a.GetComponent<MeshComponent>().Materials == std::vector<AssetHandle>{AssetHandle(5), AssetHandle(6)});
	CHECK(b.GetComponent<MeshComponent>().Materials == std::vector<AssetHandle>{AssetHandle(2), AssetHandle(3)});
}

TEST_CASE("ScriptSystem: pcall and coroutines cannot escape the instruction budget")
{
	ScriptSystemSpecification specification;
	specification.InstructionBudget = 100'000;
	ScriptFixture fixture(specification);
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnUpdate()
			for i = 1, 20000 do pcall(function() while true do end end) end
			for i = 1, 100 do coroutine.resume(coroutine.create(function() while true do end end)) end
			self.entity.name = "escaped"
		end
		return S
	)",
						   "Escaper");
	fixture.Start();
	const auto start = std::chrono::steady_clock::now();
	fixture.Update();
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
	CHECK(fixture.HasError("instruction budget"));
	CHECK(fixture.World.FindEntityByName("Escaper"));
	CHECK_FALSE(fixture.World.FindEntityByName("escaped"));
	// The budget resets for the next call.
	CHECK(fixture.Eval("1 + 1") == "2");
}

TEST_CASE("ScriptSystem: pattern matching that could backtrack for too long is refused")
{
	ScriptFixture fixture;
	fixture.Start();
	std::string error;
	const auto start = std::chrono::steady_clock::now();
	CHECK_FALSE(fixture.System->Evaluate("string.find(string.rep('a', 1500), '.-.-.-x')", error));
	CHECK(error.find("pattern too costly") != std::string::npos);
	CHECK_FALSE(fixture.System->Evaluate("(string.rep('a', 1500)):match('.-.-.-x')", error)); // Method syntax too.
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
	// Ordinary use is unaffected.
	// (Lua keeps only the first result of every call but the last in an expression list.)
	CHECK(fixture.Eval("string.find('hello world', 'o w'), ('a,b,c'):gsub(',', ';'), string.match('key=42', '(%w+)=(%d+)')") == "5\ta;b;c\tkey\t42");
	CHECK(fixture.Eval("string.find(string.rep('a', 100000) .. 'x', 'x', 1, true)") == "100001\t100001");
}

TEST_CASE("ScriptSystem: string- and integer-key iteration order is the same in every run")
{
	auto order = [](const char* fill) {
		ScriptFixture fixture;
		fixture.Start();
		return fixture.Eval(std::string("local t = {} ") + fill + " local s = '' for k in pairs(t) do s = s .. tostring(k) .. ',' end return s");
	};
	// String keys hash with the seed pinned in cmake/LuaDeterminism.h.
	CHECK(order("for i = 1, 40 do t['k' .. i] = i end") == order("for i = 1, 40 do t['k' .. i] = i end"));
	// Integer keys hash by value, including sparse and negative ones that live in the hash part rather than the array.
	const char* integers = "for i = 1, 40 do t[(i * 7919) % 1009] = i end t[-5] = 0 t[1 << 40] = 0 t[math.maxinteger] = 0";
	CHECK(order(integers) == order(integers));
	// Not pinned, by design: tables, functions and userdata (entities, components, script instances) hash by their
	// address, which ASLR and every allocation made before them decide, so pairs() over such keys has no reproducible
	// order. docs/Scripting.md (Determinism) tells scripts to keep such sets in arrays instead, which is ordered:
	const char* entities = "for i = 1, 5 do t[#t + 1] = Scene.CreateEntity('e' .. i) end t.names = '' for _, e in ipairs(t) do t.names = t.names .. e.name end";
	auto arrayOrder = [&] {
		ScriptFixture fixture;
		fixture.Start();
		return fixture.Eval(std::string("local t = {} ") + entities + " return t.names");
	};
	CHECK(arrayOrder() == "e1e2e3e4e5");
	CHECK(arrayOrder() == arrayOrder());
}

TEST_CASE("ScriptSystem: AddComponent puts an existing component back exactly when a field is refused")
{
	ScriptFixture fixture;
	// Values set in code may lie outside the field ranges, which putting the component back field by field refuses.
	fixture.World.CreateEntity("Lamp").AddComponent<PointLightComponent>().Intensity = 200000.0f;
	fixture.Start();

	// Fields are written in the table's iteration order. The out-of-range Intensity must be overwritten before the
	// refused Range, so the old value really has to be put back: restoring through the fields would refuse 200000
	// and leave 5. Should a seed or Lua change reorder the keys, this fails here instead of passing vacuously.
	const std::string fields = "{ Intensity = 5, Range = -1 }";
	REQUIRE(fixture.Eval("local keys = {} for key in pairs(" + fields + ") do keys[#keys + 1] = key end return table.concat(keys, ',')") == "Intensity,Range");
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Scene.FindEntityByName('Lamp'):AddComponent('PointLight', " + fields + ")", error));
	CHECK(error.find("Range") != std::string::npos);
	const PointLightComponent& light = fixture.World.FindEntityByName("Lamp")->GetComponent<PointLightComponent>();
	CHECK(light.Intensity == 200000.0f);
	CHECK(light.Color == glm::vec3(1.0f));
	CHECK(light.Range == 10.0f);
	CHECK_FALSE(light.CastShadows);
}

TEST_CASE("ScriptSystem: names, tags and entity creation are bounded")
{
	ScriptSystemSpecification specification;
	specification.MaxEntitiesCreatedPerCall = 50;
	ScriptFixture fixture(specification);
	fixture.Start();
	auto fails = [&](const std::string& code, const std::string& expected) {
		std::string error;
		CHECK_FALSE(fixture.System->Evaluate(code, error));
		CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
	};
	// The registry's limit and wording, whichever way the name is written.
	fails("Scene.CreateEntity(string.rep('n', 300))", "Scene.CreateEntity's name expects at most 256 bytes");
	fails("local e = Scene.CreateEntity() e.tag = string.rep('t', 300)", "entity.tag expects at most 256 bytes");
	fails("local e = Scene.CreateEntity() e.name = string.rep('n', 257)", "entity.name expects at most 256 bytes");
	CHECK(fixture.Eval("local e = Scene.CreateEntity() e.name = string.rep('n', 256) e.tag = string.rep('t', 256) return #e.name + #e.tag") == "512");
	// Component writes are bounded by the registry's MaxLength, the same rule scene files and agents meet.
	fails("Scene.CreateEntity():GetComponent('Tag').Name = string.rep('n', 257)", "at most 256 bytes");
	fails("Scene.CreateEntity():AddComponent('Tag', { Tag = string.rep('t', 257) })", "at most 256 bytes");
	CHECK(fixture.Eval("local e = Scene.CreateEntity() e:GetComponent('Tag').Name = string.rep('n', 256) return #e.name") == "256");
	fails("Scene.CreateEntity():GetComponent('Tag').Name = ''", "cannot be empty");
	fails("Scene.CreateEntity():AddComponent('Tag', { Name = '' })", "cannot be empty");
	fails("for i = 1, 100 do Scene.CreateEntity() end", "at most 50 entities");
	// The quota is per call.
	CHECK(fixture.Eval("for i = 1, 40 do Scene.CreateEntity() end return 'ok'") == "ok");
	CHECK(fixture.Eval("for i = 1, 40 do Scene.CreateEntity() end return 'ok'") == "ok");
}

TEST_CASE("ScriptSystem: destruction requested by OnDestroy outside an update happens immediately")
{
	ScriptFixture fixture;
	fixture.World.CreateEntity("Victim");
	Entity a = fixture.CreateScripted(R"(
		local S = {}
		function S:OnDestroy() local v = Scene.FindEntityByName("Victim") if v then v:Destroy() end end
		return S
	)",
									  "A");
	fixture.Start();
	fixture.Update();
	fixture.World.DestroyEntity(a);
	CHECK_FALSE(fixture.World.FindEntityByName("Victim"));
}

TEST_CASE("ScriptSystem: the specification enforces workable minimums")
{
	AssetManager assets;
	Paralax::Testing::ScopedAssertionCapture capture;
	ScriptSystemSpecification tinyMemory;
	tinyMemory.MemoryLimitBytes = 1024;
	CHECK_THROWS_AS(ScriptSystem(assets, tinyMemory), Paralax::Testing::AssertionFailedError);
	ScriptSystemSpecification tinyBudget;
	tinyBudget.InstructionBudget = 10;
	CHECK_THROWS_AS(ScriptSystem(assets, tinyBudget), Paralax::Testing::AssertionFailedError);
}

TEST_CASE("ScriptSystem: the API listing names every binding")
{
	AssetManager assets;
	ScriptSystem scripts(assets);
	const std::vector<std::string> api = scripts.GetApiNames();
	for (const char* name : {"Scene.CreateEntity", "Scene.Instantiate", "Input.IsKeyDown", "Log.Info", "Time.frame", "Entity:GetComponent", "Entity:name",
							 "vec3:Cross", "quat:Slerp", "math.random", "require"})
	{
		CAPTURE(name);
		CHECK(std::find(api.begin(), api.end(), name) != api.end());
	}
	for (const std::string& name : api)
	{
		CAPTURE(name);
		CHECK_FALSE(name.find("__") != std::string::npos);
	}
	CHECK(std::find(api.begin(), api.end(), "Key.W") == api.end()); // Constants are not API functions.

	// The listing is derived from the tables the bindings create, so every global table a script sees is either listed
	// (which makes the docs and the feature test require it) or one of the known exceptions: the Lua standard libraries,
	// the Key/Mouse constants, and Component, whose members are the reflected fields of each component type (resolved
	// by its index metamethod and covered by the component tests). A table this check finds unlisted is a function table
	// created without CreateApiTable, a usertype missing from GetApiNames' userdata list, or a library that should not
	// be in the sandbox.
	ScriptFixture fixture;
	fixture.Start();
	const std::string globals = fixture.Eval("local names = {} for name, value in pairs(_G) do if type(value) == 'table' then names[#names + 1] = name end end "
											 "table.sort(names) return table.concat(names, ' ')");
	const std::set<std::string> exceptions = {"_G", "base", "coroutine", "math", "string", "table", "utf8", "Key", "Mouse", "Component"};
	std::istringstream stream(globals);
	std::string global;
	size_t listed = 0;
	while (stream >> global)
	{
		if (exceptions.contains(global))
			continue;
		CAPTURE(global);
		const bool found = std::any_of(api.begin(), api.end(), [&](const std::string& name) { return name.starts_with(global + '.') || name.starts_with(global + ':'); });
		CHECK_MESSAGE(found, "A global table scripts can use is not listed by GetApiNames: create a function table with CreateApiTable, add a usertype to "
							 "GetApiNames' userdata list, or remove it if it is a library the sandbox must not expose");
		listed++;
	}
	CHECK(listed >= 12); // Scene, Input, Log, Time, Physics, Audio, Game, Entity, vec2, vec3, vec4, quat.
}

TEST_CASE("ScriptAsset: missing and oversized files are rejected")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("ScriptAssetHostile");
	Paralax::Testing::ScopedLogCapture log;
	CHECK_FALSE(ScriptAsset::LoadFromFile(directory / "Missing.lua"));
	{
		std::ofstream file(directory / "Huge.lua", std::ios::binary);
		file.seekp(static_cast<std::streamoff>(ScriptAsset::MaxSourceBytes));
		file.put('\n');
	}
	CHECK_FALSE(ScriptAsset::LoadFromFile(directory / "Huge.lua"));
	CHECK(log.Contains(spdlog::level::err, "too large"));
	{
		std::ofstream(directory / "Ok.lua") << "return {}";
	}
	Ref<ScriptAsset> ok = ScriptAsset::LoadFromFile(directory / "Ok.lua");
	REQUIRE(ok);
	CHECK(ok->Source == "return {}");
	CHECK(ok->Name == "Ok.lua");
}

// --- Physics from scripts -----------------------------------------------------------------------------------

namespace {

	struct PhysicsScriptFixture : ScriptFixture
	{
		PhysicsScriptFixture() { World.AddSystem(CreateScope<PhysicsSystem>()); }

		Entity Ground()
		{
			Entity ground = World.CreateEntity("Ground");
			ground.AddComponent<ColliderComponent>().Size = glm::vec3(20.0f, 1.0f, 20.0f);
			ground.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
			return ground;
		}
	};

}

TEST_CASE("ScriptSystem: scripts drive bodies through the Physics API")
{
	PhysicsScriptFixture fixture;
	fixture.Ground();
	Entity ball = fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			Physics.SetGravity(vec3(0, 0, 0))
			Gravity = Physics.GetGravity()
			Pushed = Physics.AddImpulse(self.entity, vec3(2, 0, 0))
			Forced = Physics.AddForce(self.entity, vec3(0, 0, 0))
			Spun = Physics.SetAngularVelocity(self.entity, vec3(0, 1, 0))
			Physics.SetLinearVelocity(self.entity, vec3(0, 0, 0))
			Physics.AddImpulse(self.entity, vec3(2, 0, 0))
		end
		function S:OnUpdate()
			Velocity = Physics.GetLinearVelocity(self.entity)
			Angular = Physics.GetAngularVelocity(self.entity)
			GroundIsStatic = not Physics.AddImpulse(Scene.FindEntityByName("Ground"), vec3(1, 0, 0))
			NoBody = Physics.GetLinearVelocity(Scene.CreateEntity("Bodiless")) == nil
			Hit = Physics.Raycast(vec3(0, 10, 0), vec3(0, -1, 0), 100)
			Miss = Physics.Raycast(vec3(0, 10, 0), vec3(0, 1, 0), 100)
		end
		return S
	)",
										 "Ball");
	ball.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 3.0f, 0.0f);
	ball.AddComponent<ColliderComponent>().Shape = ColliderShape::Sphere;
	ball.AddComponent<RigidbodyComponent>();
	fixture.Start();
	fixture.Update(2);
	REQUIRE(fixture.System->GetErrors().empty());
	CHECK(fixture.Eval("Gravity == vec3(0, 0, 0), Pushed, Forced, Spun") == "true\ttrue\ttrue\ttrue");
	CHECK(fixture.Eval("math.abs(Velocity.x - 2) < 0.05 and math.abs(Velocity.y) < 1e-4") == "true");
	CHECK(fixture.Eval("GroundIsStatic, NoBody, Miss == nil") == "true\ttrue\ttrue");
	CHECK(fixture.Eval("Hit.entity.name, math.abs(Hit.distance - 6.5) < 0.01, Hit.normal.y > 0.99") == "Ball\ttrue\ttrue");
	fixture.Update(30);
	CHECK(ball.GetComponent<TransformComponent>().Translation.x > 0.5f); // The impulse moved it.

	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Physics.SetGravity(vec3(0, 0/0, 0))", error));
	CHECK_FALSE(fixture.System->Evaluate("Physics.AddForce(Scene.GetEntity('1'), vec3(1))", error)); // Not an entity.
}

TEST_CASE("ScriptSystem: OnFixedUpdate runs after OnUpdate, once before each physics step, with the step as dt")
{
	PhysicsScriptFixture fixture;
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate() Calls = "C" end
		function S:OnUpdate() Calls = Calls .. "U" end
		function S:OnFixedUpdate(dt)
			Calls = Calls .. "F"
			FixedDt = dt
			-- Entities created here get their instance (and OnCreate) at the next update, like those created in OnUpdate.
			if not Spawned then
				Spawned = true
				Scene.CreateEntity("Spawned"):AddComponent("Script", { Script = SpawnedScript })
			end
		end
		return S
	)",
						   "Stepper");
	const AssetHandle spawned = fixture.AddScript(R"(
		return { OnCreate = function() Calls = Calls .. "c" end, OnFixedUpdate = function() Calls = Calls .. "f" end }
	)",
												  "Spawned.lua");
	fixture.Start();
	fixture.Eval("SpawnedScript = '" + std::to_string(static_cast<uint64_t>(spawned)) + "'");
	fixture.Update(1, 3.0f / 60.0f);  // Three steps.
	fixture.Update(1, 1.0f / 120.0f); // Half a step: none.
	fixture.Update(1, 1.0f / 120.0f); // The other half: one.
	REQUIRE(fixture.System->GetErrors().empty());
	CHECK(fixture.Eval("Calls") == "CUFFFcUUFf");
	CHECK(fixture.Eval("math.abs(FixedDt - 1/60) < 1e-6") == "true");
}

TEST_CASE("ScriptSystem: forces applied in OnFixedUpdate give the same motion at any frame rate")
{
	std::vector<std::string> results;
	for (const int framesPerSecond : {20, 60, 144})
	{
		CAPTURE(framesPerSecond);
		PhysicsScriptFixture fixture;
		Entity ball = fixture.CreateScripted(R"(
			local S = {}
			function S:OnCreate() Physics.SetGravity(vec3(0, 0, 0)); Steps = 0 end
			function S:OnFixedUpdate(dt)
				if Steps < 60 then Physics.AddForce(self.entity, vec3(4, 0, 0)) end
				Steps = Steps + 1
			end
			return S
		)",
											 "Ball");
		ball.AddComponent<ColliderComponent>().Shape = ColliderShape::Sphere;
		auto& rigidbody = ball.AddComponent<RigidbodyComponent>();
		rigidbody.Mass = 2.0f;
		rigidbody.LinearDamping = 0.0f;
		fixture.Start();
		const float frame = 1.0f / static_cast<float>(framesPerSecond);
		fixture.Update(1, frame);
		while (fixture.Eval("Steps <= 60") == "true") // Past the last push (several steps per update at low rates).
			fixture.Update(1, frame);
		REQUIRE(fixture.System->GetErrors().empty());
		// 4 N on 2 kg for one second of steps: 2 m/s.
		CHECK(fixture.Eval("math.abs(Physics.GetLinearVelocity(Scene.FindEntityByName('Ball')).x - 2) < 1e-3") == "true");
		results.push_back(fixture.Eval("Physics.GetLinearVelocity(Scene.FindEntityByName('Ball')).x"));
	}
	CHECK(results[0] == results[1]);
	CHECK(results[1] == results[2]);
}

TEST_CASE("ScriptSystem: a failing OnFixedUpdate disables its instance, and without physics it never runs")
{
	PhysicsScriptFixture fixture;
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnUpdate() Updates = (Updates or 0) + 1 end
		function S:OnFixedUpdate() error("fixed failure") end
		return S
	)",
						   "Failing");
	fixture.Start();
	fixture.Update(3);
	CHECK(fixture.HasError("fixed failure"));
	CHECK(fixture.HasError("Failing.lua OnFixedUpdate"));
	CHECK(fixture.Eval("Updates") == "1");

	ScriptFixture noPhysics;
	noPhysics.CreateScripted(R"(
		return { OnUpdate = function() Updates = (Updates or 0) + 1 end, OnFixedUpdate = function() Fixed = true end }
	)");
	noPhysics.Start();
	noPhysics.Update(3);
	CHECK(noPhysics.Eval("Updates, Fixed") == "3\tnil");
}

TEST_CASE("ScriptSystem: OnFixedUpdate skips entities destroyed or unscripted during the update")
{
	PhysicsScriptFixture fixture;
	fixture.CreateScripted(R"(
		local S = {}
		function S:OnFixedUpdate()
			FirstCalls = (FirstCalls or 0) + 1
			-- A one-shot effect: at a low frame rate several steps share an update, but it must happen once.
			if FirstCalls == 1 then
				Explosions = (Explosions or 0) + 1
				self.entity:Destroy()
				Scene.FindEntityByName("Second"):RemoveComponent("Script")
			end
		end
		return S
	)",
						   "First");
	fixture.CreateScripted("return { OnFixedUpdate = function() SecondCalls = (SecondCalls or 0) + 1 end }", "Second");
	fixture.Start();
	fixture.Update(1, 3.0f / 60.0f); // Three steps.
	REQUIRE(fixture.System->GetErrors().empty());
	// First runs before Second (hierarchy order), so Second's script is removed before its first call.
	CHECK(fixture.Eval("FirstCalls, Explosions, SecondCalls") == "1\t1\tnil");
	CHECK_FALSE(fixture.World.FindEntityByName("First"));
	fixture.Update(1, 3.0f / 60.0f);
	CHECK(fixture.Eval("SecondCalls") == "nil");
}

TEST_CASE("ScriptSystem: the system wants fixed updates only while a live script defines OnFixedUpdate")
{
	PhysicsScriptFixture fixture;
	Entity plain = fixture.CreateScripted("return { OnUpdate = function() end }", "Plain");
	fixture.Start();
	fixture.Update();
	CHECK_FALSE(fixture.System->WantsFixedUpdate());

	// Defined by the class, or by OnCreate on the instance; a hostile lookup disables the instance instead.
	Entity late = fixture.CreateScripted(R"(
		return { OnCreate = function(self) self.OnFixedUpdate = function() LateFixed = true end end }
	)",
										 "Late");
	fixture.Update();
	CHECK(fixture.System->WantsFixedUpdate());
	fixture.Update();
	CHECK(fixture.Eval("LateFixed") == "true");
	fixture.World.DestroyEntity(late);
	CHECK_FALSE(fixture.System->WantsFixedUpdate());

	fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			setmetatable(self, { __index = function(_, key) if key == "OnFixedUpdate" then error("hostile lookup") end end })
		end
		return S
	)",
						   "Hostile");
	fixture.Update();
	CHECK(fixture.HasError("hostile lookup"));
	CHECK_FALSE(fixture.System->WantsFixedUpdate());
	CHECK(fixture.World.IsValid(plain));
}

TEST_CASE("ScriptSystem: collisions and triggers call both entities' scripts")
{
	PhysicsScriptFixture fixture;
	const std::string listener = R"(
		local S = {}
		function S:OnCollisionEnter(other) Log_(self, "enter", other) end
		function S:OnCollisionExit(other) Log_(self, "exit", other) end
		function S:OnTriggerEnter(other) Log_(self, "trigger", other) end
		function S:OnTriggerExit(other) Log_(self, "untrigger", other) end
		return S
	)";
	Entity ground = fixture.Ground();
	ground.AddComponent<ScriptComponent>().Script = fixture.AddScript(listener, "Ground.lua");
	Entity box = fixture.CreateScripted(listener, "Box");
	box.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 4.0f, 0.0f);
	box.AddComponent<ColliderComponent>();
	box.AddComponent<RigidbodyComponent>();
	Entity zone = fixture.World.CreateEntity("Zone");
	zone.AddComponent<ColliderComponent>().IsTrigger = true;
	zone.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 2.5f, 0.0f);
	fixture.Start();
	fixture.Eval("Events = {} function Log_(self, what, other) Events[#Events + 1] = self.entity.name .. ':' .. what .. ':' .. other.name end");
	fixture.Update(150);
	REQUIRE(fixture.System->GetErrors().empty());
	const std::string events = fixture.Eval("table.concat(Events, ',')");
	CAPTURE(events);
	CHECK(events.find("Box:trigger:Zone") != std::string::npos);
	CHECK(events.find("Box:untrigger:Zone") != std::string::npos);
	CHECK(events.find("Box:enter:Ground") != std::string::npos);
	CHECK(events.find("Ground:enter:Box") != std::string::npos);
	// The trigger is passed before the ground is hit.
	CHECK(events.find("Box:trigger:Zone") < events.find("Box:enter:Ground"));
}

TEST_CASE("ScriptSystem: scripts control sound through the Audio API")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("ScriptAudio");
	{
		const std::vector<uint8_t> wav = Paralax::Testing::MakeWav(1, 48000, std::vector<float>(4800, 0.25f));
		std::filesystem::create_directories(directory / "Audio");
		std::ofstream(directory / "Audio" / "Beep.wav", std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
	}
	ScriptFixture fixture({}, directory);
	const AssetHandle beep = fixture.Assets.ImportAsset("Audio/Beep.wav");
	REQUIRE(beep.IsValid());
	AudioSystem* audio = static_cast<AudioSystem*>(fixture.World.AddSystem(CreateScope<AudioSystem>(fixture.Assets)));
	Entity speaker = fixture.CreateScripted(R"(
		local S = {}
		function S:OnCreate()
			StartedPlaying = Audio.IsPlaying(self.entity)
			Paused = Audio.Pause(self.entity)
			PausedPlaying = Audio.IsPlaying(self.entity)
			Resumed = Audio.Play(self.entity)
			Stopped = Audio.Stop(self.entity)
			StoppedPlaying = Audio.IsPlaying(self.entity)
			Audio.Play(self.entity)
			ByPath = Audio.PlayOneShot("Audio/Beep.wav")
			Placed = Audio.PlayOneShot("Audio/Beep.wav", vec3(1, 2, 3), 0.5)
			Audio.SetMasterVolume(0.5)
			Master = Audio.GetMasterVolume()
			Silent = Audio.Play(Scene.CreateEntity("Silent"))
		end
		return S
	)",
											"Speaker");
	auto& source = speaker.AddComponent<AudioSourceComponent>();
	source.Clip = beep;
	source.Loop = true;
	fixture.Start();
	fixture.Update(2);
	REQUIRE(fixture.System->GetErrors().empty());
	CHECK(fixture.Eval("StartedPlaying, Paused, PausedPlaying, Resumed, Stopped, StoppedPlaying") == "true\ttrue\tfalse\ttrue\ttrue\tfalse");
	CHECK(fixture.Eval("ByPath, Placed, Master, Silent") == "true\ttrue\t0.5\tfalse");
	CHECK(audio->IsPlaying(speaker.GetUUID()));
	CHECK(fixture.Eval(fmt::format("Audio.PlayOneShot('{}')", static_cast<uint64_t>(beep))) == "true"); // By handle.

	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Audio.PlayOneShot('Audio/Missing.wav')", error));
	CHECK(error.find("no audio clip") != std::string::npos);
	CHECK_FALSE(fixture.System->Evaluate("Audio.PlayOneShot('Audio/Beep.wav', 'here')", error));
	CHECK_FALSE(fixture.System->Evaluate("Audio.PlayOneShot('Audio/Beep.wav', nil, -1)", error));
	CHECK(error.find("non-negative") != std::string::npos);
	CHECK_FALSE(fixture.System->Evaluate("Audio.PlayOneShot('Audio/Beep.wav', nil, 'loud')", error));
	CHECK(fixture.Eval("Audio.PlayOneShot('Audio/Beep.wav', nil, 0.25)") == "true");
	CHECK_FALSE(fixture.System->Evaluate("Audio.SetMasterVolume(0/0)", error));
	CHECK_FALSE(fixture.System->Evaluate("Audio.Play(Scene.GetEntity('1'))", error)); // Not an entity.
}

TEST_CASE("ScriptSystem: Scene.Load and Game need a game session and a real scene")
{
	ScriptFixture fixture;
	fixture.Start();
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Game.Set('Score', 1)", error));
	CHECK(error.find("no game session") != std::string::npos);
	CHECK_FALSE(fixture.System->Evaluate("Game.Get('Score')", error));
	CHECK_FALSE(fixture.System->Evaluate("Scene.Load('Scenes/None.pxscene')", error));
	CHECK(error.find("no scene") != std::string::npos);
	// Reload is only a request; without a session nothing acts on it, and it is taken once.
	fixture.Eval("Scene.Reload()");
	CHECK(fixture.System->TakeSceneRequest() == AssetHandle(0));
	CHECK_FALSE(fixture.System->TakeSceneRequest());
}

TEST_CASE("ScriptSystem: the Audio API reports when the scene has no audio")
{
	ScriptFixture fixture;
	fixture.Start();
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Audio.GetMasterVolume()", error));
	CHECK(error.find("audio is not running") != std::string::npos);
}

TEST_CASE("ScriptSystem: the Physics API reports when the scene has no physics")
{
	ScriptFixture fixture;
	fixture.Start();
	std::string error;
	CHECK_FALSE(fixture.System->Evaluate("Physics.GetGravity()", error));
	CHECK(error.find("physics is not running") != std::string::npos);
}

TEST_CASE("ScriptSystem: print, Log and errors keep at most MaxMessageBytes of their text")
{
	ScriptFixture fixture;
	// 16 MiB of text, which scripts may build (it is well inside their memory limit).
	fixture.CreateScripted(R"(
		local Loud = {}
		function Loud:OnCreate()
			local text = string.rep("a", 16 * 1024 * 1024)
			print(text)
			Log.Info("x", text, "tail")
			print("short", 1, true, nil)
			error(text)
		end
		return Loud
	)");
	Paralax::Testing::ScopedLogCapture log;
	fixture.Start();
	fixture.Update();

	const std::string kept(MaxMessageBytes, 'a');
	CHECK(log.Contains(spdlog::level::info, kept + "... (16777216 bytes)"));
	// Arguments are joined by tabs: "x", tab, 16 MiB, tab, "tail".
	CHECK(log.Contains(spdlog::level::info, "x\t" + std::string(MaxMessageBytes - 2, 'a') + "... (16777223 bytes)"));
	CHECK(log.Contains(spdlog::level::info, "short\t1\ttrue\tnil"));
	for (const Paralax::Testing::ScopedLogCapture::Entry& entry : log.GetEntries())
		CHECK(entry.Message.size() < MaxMessageBytes + 64);

	// The error is recorded and logged with its context, cut to the same size.
	REQUIRE(fixture.System->GetErrors().size() == 1);
	const std::string& error = fixture.System->GetErrors()[0];
	CHECK(error.find("OnCreate: ") != std::string::npos);
	CHECK(error.size() <= MaxMessageBytes + 32);
	CHECK(error.ends_with(" bytes)"));
	CHECK(log.Contains(spdlog::level::err, "Script error: " + error));

	// Evaluated code's errors are returned cut too; a long require path is named briefly.
	std::string failure;
	CHECK_FALSE(fixture.System->Evaluate("error(string.rep('b', 16 * 1024 * 1024))", failure));
	CHECK(failure.size() <= MaxMessageBytes + 32);
	CHECK(failure.find(std::string(1000, 'b')) != std::string::npos);
	CHECK_FALSE(fixture.System->Evaluate("require(string.rep('c', 16 * 1024 * 1024))", failure));
	CHECK(failure.find("require: no script 'ccc") != std::string::npos);
	CHECK(failure.find("...' (16777220 bytes) in the project") != std::string::npos);
	CHECK(failure.size() < 1024);
}
