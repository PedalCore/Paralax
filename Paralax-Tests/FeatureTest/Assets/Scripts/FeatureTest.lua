-- The feature test: exercises every scripting API function and checks the scene contains every component type.
-- Run headless by CTest and CI (Paralax-Runtime --fail-on-script-error); any failed assert fails the run.
-- Paralax-Tests/src/Scripting/FeatureTestCoverageTests.cpp verifies every API name appears in this file.
local Util = require("Scripts/Util")

local FeatureTest = { finishFrame = 30 }

local function near(a, b) return math.abs(a - b) < 1e-4 end
local function nearVec(a, b) return (a - b):Length() < 1e-4 end

local function TestMath()
	-- vec2
	local a2 = vec2(3, 4)
	assert(near(a2:Length(), 5) and near(a2.x, 3) and near(a2.y, 4))
	assert(near(a2:Normalized():Length(), 1) and near(a2:Dot(vec2(1, 0)), 3))
	assert(a2:Lerp(vec2(0), 0.5) == vec2(1.5, 2) and (a2 + a2) == a2 * 2 and (2 * a2) / 2 == a2 and -a2 == vec2(-3, -4))
	assert(tostring(vec2(1, 2)) == "vec2(1, 2)" and vec2() == vec2(0, 0) and vec2(7) == vec2(7, 7))
	-- vec3
	local a3 = vec3(1, 2, 2)
	assert(near(a3:Length(), 3) and near(a3:Normalized():Length(), 1))
	assert(a3:Cross(vec3(0, 0, 1)) == vec3(2, -1, 0) and near(a3:Dot(vec3(1, 1, 1)), 5) and near(a3:Distance(vec3(1, 2, 0)), 2))
	assert(a3:Lerp(vec3(3, 2, 2), 0.5) == vec3(2, 2, 2) and a3 - a3 == vec3() and a3 * vec3(2) == vec3(2, 4, 4) and -a3 == vec3(-1, -2, -2))
	local v = vec3(); v.x = 1; v.y = 2; v.z = 3
	assert(v == vec3(1, 2, 3) and near(v.z, 3))
	-- vec4
	local a4 = vec4(1, 2, 3, 4)
	assert(near(a4.w, 4) and near(a4:Dot(vec4(1)), 10) and near(vec4(2, 0, 0, 0):Length(), 2) and a4:Lerp(a4, 1) == a4)
	assert(a4 + vec4(1) == vec4(2, 3, 4, 5) and a4 - a4 == vec4() and a4 * 2 == vec4(2, 4, 6, 8) and -a4 == vec4(-1, -2, -3, -4))
	assert(tostring(vec4(0)):find("vec4") ~= nil)
	-- quat
	local q = quat.AngleAxis(math.pi / 2, vec3(0, 1, 0))
	assert(nearVec(q * vec3(1, 0, 0), vec3(0, 0, -1)), "a quarter turn about +Y maps +X to -Z")
	assert(near(quat.FromEuler(vec3(0.1, 0.2, 0.3)):ToEuler().y, 0.2))
	assert(nearVec((q * q:Inverse()) * vec3(1, 2, 3), vec3(1, 2, 3)) and near(q:Normalized().w, q.w))
	assert(nearVec(quat():Slerp(q, 1) * vec3(1, 0, 0), vec3(0, 0, -1)) and quat().w == 1 and quat(1, 0, 0, 0) == quat())
	assert(near(q.x, 0) and near(q.z, 0) and q.y > 0)
	-- math.random is deterministic for a seed.
	math.randomseed(42)
	local first = { math.random(), math.random(10), math.random(5, 9) }
	math.randomseed(42)
	assert(first[1] == math.random() and first[2] == math.random(10) and first[3] == math.random(5, 9))
	assert(first[3] >= 5 and first[3] <= 9)
	-- Modules.
	assert(Util.Clamp(5, 0, 3) == 3 and require("Scripts/Util") == Util)
end

local function TestComponents(self)
	local probe = Scene.CreateEntity("Probe")
	-- Every component type: add (or get) it and round-trip a field through the reflection layer.
	local transform = probe:GetComponent("Transform")
	transform.Translation = vec3(1, 2, 3)
	transform.Rotation = quat.FromEuler(vec3(0, 0.5, 0))
	transform.Scale = { 2, 2, 2 }
	assert(transform.Translation == vec3(1, 2, 3) and transform.Scale == vec3(2, 2, 2) and transform.type == "Transform")
	assert(probe:HasComponent("Tag") and probe:GetComponent("Tag").Name == "Probe")
	assert(probe:AddComponent("Camera", { Primary = false }).Primary == false)
	assert(probe:AddComponent("DirectionalLight", { Intensity = 2 }).Intensity == 2)
	assert(probe:AddComponent("PointLight", { Range = 7 }).Range == 7)
	assert(probe:AddComponent("SpotLight", { Color = vec3(1, 0, 0) }).Color == vec3(1, 0, 0))
	assert(probe:AddComponent("SkyLight", { Intensity = 0.5 }).Intensity == 0.5)
	local mesh = probe:AddComponent("Mesh", { Mesh = "2" })
	mesh.Materials = { "16" }
	assert(mesh.Mesh == "2" and mesh.Materials[1] == "16" and mesh.CastShadows == true)
	assert(probe:AddComponent("Script").Script == "0")
	assert(probe:RemoveComponent("PointLight") and not probe:HasComponent("PointLight") and not probe:RemoveComponent("PointLight"))
	-- Invalid values raise errors and change nothing.
	assert(not pcall(function() transform.Translation = "up" end))
	assert(not pcall(function() probe:AddComponent("PointLight", { Intensity = -1 }) end) and not probe:HasComponent("PointLight"))
	assert(not pcall(function() probe:GetComponent("NoSuchComponent") end))
	-- Every component type in the engine appears in the authored scene.
	for _, name in ipairs({ "Tag", "Transform", "Camera", "DirectionalLight", "PointLight", "SpotLight", "SkyLight", "Mesh", "Script", "PrefabInstance" }) do
		local found = false
		for _, entity in ipairs(Scene.FindEntitiesByTag("Feature")) do
			found = found or entity:HasComponent(name)
		end
		assert(found, "no Feature-tagged entity has a " .. name .. " component")
	end
	probe:Destroy()
end

local function TestScene(self)
	local root = Scene.CreateEntity("Root")
	local child = Scene.CreateEntity("Child", root)
	child.tag = "FeatureChild"
	child.name = "ChildRenamed"
	assert(child:GetParent() == root and #root:GetChildren() == 1 and root:GetChildren()[1] == child)
	assert(Scene.FindEntityByName("ChildRenamed") == child and Scene.GetEntity(child.id) == child and #Scene.FindEntitiesByTag("FeatureChild") == 1)
	child:SetParent(nil)
	assert(child:GetParent() == nil and #root:GetChildren() == 0)
	child:SetParent(root, false)
	root:GetComponent("Transform").Translation = vec3(0, 10, 0)
	assert(nearVec(child:GetWorldPosition(), vec3(0, 10, 0)))
	local count = Scene.GetEntityCount()
	Scene.Destroy(root) -- Deferred to the end of the update: both still exist now.
	assert(root:IsValid() and child:IsValid() and Scene.GetEntityCount() == count)
	self.destroyed = { root, child }

	-- Prefabs by path and by handle, with position and parent options.
	local crate = Scene.Instantiate("Prefabs/Crate.pxprefab", { position = vec3(-4, 0, 2) })
	assert(crate.name == "Crate" and crate:HasComponent("PrefabInstance") and crate:GetComponent("Transform").Translation == vec3(-4, 0, 2))
	local again = Scene.Instantiate(crate:GetComponent("PrefabInstance").Prefab, { parent = Scene.FindEntityByName("Spinner") })
	assert(again:GetParent().name == "Spinner" and again.id ~= crate.id)

	-- Another script's instance is reachable through its entity.
	local spinner = Scene.FindEntityByName("Spinner"):GetScript()
	assert(spinner ~= nil and spinner.speed == 1.5, "Spinner's script instance should be reachable")
	assert(Scene.FindEntityByName("Nobody") == nil and Scene.GetEntity("1") == nil)

	-- A scripted entity created now starts on the next update.
	local spawned = Scene.CreateEntity("Spawned")
	spawned:AddComponent("Script", { Script = self.entity:GetComponent("Script").Script })
	spawned.tag = "Spawned"
end

local function TestInputAndLog()
	-- Headless runs have no input; the API must still answer consistently.
	assert(Input.IsKeyDown(Key.W) == false and Input.WasKeyPressed(Key.Space) == false and Input.WasKeyReleased(Key.Escape) == false)
	assert(Input.IsMouseButtonDown(Mouse.Left) == false and Input.WasMouseButtonPressed(Mouse.Right) == false)
	assert(Input.WasMouseButtonReleased(Mouse.Middle) == false)
	assert(Input.GetMousePosition().x >= 0 and Input.GetMouseScrollDelta() == vec2(0, 0))
	assert(not pcall(Input.IsKeyDown, -1))
	Log.Trace("FeatureTest: trace")
	Log.Info("FeatureTest:", "info", 1, vec3(1))
	Log.Warn("FeatureTest: warn (expected)")
	Log.Error("FeatureTest: error message (expected; not a script failure)")
	print("FeatureTest: print")
	-- The sandbox holds.
	assert(io == nil and os == nil and load == nil and debug == nil and string.dump == nil)
end

function FeatureTest:OnCreate()
	if self.entity.tag == "Spawned" then
		-- A copy spawned by the main instance: only records that it started.
		Spawned = Time.frame
		return
	end
	self.startFrame = Time.frame
	TestMath()
	TestComponents(self)
	TestScene(self)
	TestInputAndLog()
end

function FeatureTest:OnUpdate(dt)
	if self.entity.tag == "Spawned" or self.done then
		return
	end
	assert(dt > 0 and near(Time.delta, dt) and Time.elapsed > 0)
	if Time.frame == self.startFrame + 1 then
		-- Destruction requested in OnCreate happened at the end of that update.
		assert(not self.destroyed[1]:IsValid() and not self.destroyed[2]:IsValid())
		assert(not pcall(function() return self.destroyed[1].name end))
		assert(Spawned == Time.frame, "an entity spawned with a script starts on the next update")
	end
	if Time.frame >= self.finishFrame then
		self.done = true
		Log.Info("FeatureTest: all checks passed at frame " .. Time.frame)
	end
end

function FeatureTest:OnDestroy()
	if self.entity.tag ~= "Spawned" then
		assert(self.done, "the feature test did not run to completion (run at least " .. self.finishFrame .. " frames)")
	end
end

return FeatureTest
