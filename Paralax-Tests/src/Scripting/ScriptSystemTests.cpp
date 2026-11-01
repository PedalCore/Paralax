#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Audio/AudioSystem.h>
#include <Paralax/Core/Input.h>
#include <Paralax/Events/KeyEvent.h>
#include <Paralax/Physics/PhysicsSystem.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Prefab.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scripting/ScriptAsset.h>
#include <Paralax/Scripting/ScriptSystem.h>

#include <chrono>
#include <fstream>

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
	ScriptFixture fixture({}, directory);
	for (const char* name : {"Scripts/Util.lua", "Scripts/A.lua", "Scripts/B.lua"})
		REQUIRE(fixture.Assets.ImportAsset(name).IsValid());
	fixture.Start();

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

TEST_CASE("ScriptSystem: string-key iteration order is the same in every run")
{
	auto order = [] {
		ScriptFixture fixture;
		fixture.Start();
		return fixture.Eval("local t = {} for i = 1, 40 do t['k' .. i] = i end local s = '' for k in pairs(t) do s = s .. k .. ',' end return s");
	};
	CHECK(order() == order());
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
	fails("Scene.CreateEntity(string.rep('n', 300))", "limited to 256 bytes");
	fails("local e = Scene.CreateEntity() e.tag = string.rep('t', 300)", "limited to 256 bytes");
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
