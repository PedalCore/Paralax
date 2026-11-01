-- The feature test: exercises every scripting API function and checks the scene contains every component type.
-- Run headless by CTest and CI (Paralax-Runtime --fail-on-script-error); any failed assert fails the run.
-- Paralax-Tests/src/Scripting/FeatureTestCoverageTests.cpp verifies every API name appears in this file.
local Util = require("Scripts/Util")

local FeatureTest = { finishFrame = 45 }

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
	local volume = probe:AddComponent("GIVolume", { ProbeSpacing = 2, ProbeCountY = 3 })
	assert(volume.Enabled == true and volume.ProbeSpacing == 2 and volume.ProbeCountX == 4 and volume.ProbeCountY == 3)
	assert(not pcall(function() volume.ProbeCountZ = 13 end) and volume.ProbeCountZ == 4)
	local mesh = probe:AddComponent("Mesh", { Mesh = "2" })
	mesh.Materials = { "16" }
	assert(mesh.Mesh == "2" and mesh.Materials[1] == "16" and mesh.CastShadows == true)
	assert(probe:AddComponent("Script").Script == "0")
	assert(probe:AddComponent("Collider", { Shape = "Sphere", Size = vec3(2) }).Shape == "Sphere")
	assert(probe:AddComponent("Rigidbody", { Type = "Kinematic", Mass = 3 }).Mass == 3)
	assert(probe:AddComponent("AudioSource", { Volume = 0.5, Loop = true }).Loop == true)
	assert(probe:AddComponent("AudioListener", { Active = false }).Active == false)
	local animation = probe:AddComponent("Animation", { Loop = false, Speed = 2 })
	assert(animation.Clip == "0" and animation.Playing == true and animation.Loop == false and animation.Speed == 2 and animation.Time == 0)
	assert(probe:RemoveComponent("PointLight") and not probe:HasComponent("PointLight") and not probe:RemoveComponent("PointLight"))
	-- Invalid values raise errors and change nothing.
	assert(not pcall(function() transform.Translation = "up" end))
	assert(not pcall(function() probe:AddComponent("PointLight", { Intensity = -1 }) end) and not probe:HasComponent("PointLight"))
	assert(not pcall(function() probe:GetComponent("NoSuchComponent") end))
	-- Every component type in the engine appears in the authored scene.
	for _, name in ipairs({ "Tag", "Transform", "Camera", "DirectionalLight", "PointLight", "SpotLight", "SkyLight", "Mesh", "Rigidbody",
		"Collider", "AudioSource", "AudioListener", "Animation", "GIVolume", "Script", "PrefabInstance" }) do
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

local function TestPhysics()
	assert(near(Physics.GetGravity().y, -9.81))
	Physics.SetGravity(vec3(0, -9.81, 0))
	local ball = Scene.FindEntityByName("PhysicsBall")
	assert(Physics.AddForce(ball, vec3(0, 0, 0)) and Physics.AddImpulse(ball, vec3(0, 0, 0)))
	assert(Physics.SetLinearVelocity(ball, vec3(0, -1, 0)) and near(Physics.GetLinearVelocity(ball).y, -1))
	assert(Physics.SetAngularVelocity(ball, vec3(0, 1, 0)) and near(Physics.GetAngularVelocity(ball).y, 1))
	-- The ground is static: forces do not apply to it.
	assert(not Physics.AddImpulse(Scene.FindEntityByName("Ground"), vec3(1, 0, 0)))
	local hit = Physics.Raycast(vec3(4, 5, 4), vec3(0, -1, 0), 20)
	assert(hit and hit.entity.name == "Ground" and near(hit.distance, 5) and hit.normal.y > 0.99 and near(hit.point.y, 0), "raycast should hit the ground")
	assert(Physics.Raycast(vec3(0, 50, 0), vec3(0, 1, 0), 10) == nil)
	-- The crate prefab is one compound body: the lid's collider is part of the crate's Rigidbody.
	local crate
	for _, entity in ipairs(Scene.FindEntitiesByTag("Feature")) do
		if entity.name == "Crate" then crate = entity end -- The authored instance, not one spawned this frame.
	end
	assert(Physics.GetLinearVelocity(crate) ~= nil, "the crate should be a body")
	assert(Physics.GetLinearVelocity(crate:GetChildren()[1]) == nil, "the lid should be part of the crate's body")
end

local function TestAudio()
	-- The speaker loops its beep from the start (PlayOnStart); scripts control it like a media player.
	local speaker = Scene.FindEntityByName("Speaker")
	assert(Audio.IsPlaying(speaker), "the speaker should play from the start")
	assert(Audio.Pause(speaker) and not Audio.IsPlaying(speaker))
	assert(Audio.Play(speaker) and Audio.IsPlaying(speaker))
	assert(Audio.Stop(speaker) and not Audio.IsPlaying(speaker))
	assert(Audio.Play(speaker))
	assert(Audio.PlayOneShot("Audio/Beep.wav", vec3(0, 1, 0), 0.5) and Audio.PlayOneShot(speaker:GetComponent("AudioSource").Clip))
	assert(not pcall(Audio.PlayOneShot, "Audio/Missing.wav"))
	assert(not Audio.Play(Scene.FindEntityByName("Ground")), "an entity without an AudioSource cannot play")
	Audio.SetMasterVolume(0.8)
	assert(near(Audio.GetMasterVolume(), 0.8))
	Audio.SetMasterVolume(1)
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
	-- After the first run finishes it reloads the scene (Scene.Reload); Game values survive that, scripts do not.
	self.reloaded = Game.Get("FeatureTestRuns", 0) > 0
	if self.reloaded then
		Log.Info("FeatureTest: the reloaded scene started")
		assert(Game.Get("FeatureTestRuns") == 1 and math.type(Game.Get("FeatureTestRuns")) == "integer", "Game values should survive Scene.Reload")
		assert(Game.Get("Note") == "kept")
	end
	assert(Game.Get("NoSuchKey") == nil and Game.Get("NoSuchKey", "fallback") == "fallback")
	assert(not pcall(Game.Set, "", 1) and not pcall(Game.Set, "Bad", {}))
	assert(not pcall(Scene.Load, "Scenes/Missing.pxscene") and not pcall(Scene.Load, "Prefabs/Crate.pxprefab"))
	TestMath()
	TestComponents(self)
	TestScene(self)
	TestPhysics()
	TestAudio()
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
		-- Physics callbacks (Ball.lua): the ball passed through the trigger zone and landed on the ground.
		assert(BallTriggered and BallPassedTrigger, "the ball should have passed through the trigger zone")
		assert(BallLanded, "the ball should have landed on the ground")
		assert(BallFixedSteps and BallFixedSteps > 0, "OnFixedUpdate should run before each physics step")
		assert(Scene.FindEntityByName("PhysicsBall"):GetWorldPosition().y < 1, "gravity should have pulled the ball down")
		assert(Audio.IsPlaying(Scene.FindEntityByName("Speaker")), "the looping speaker should still be playing")
		-- The imported model's clip (Swing: up 1 m and back over 2 s, looping) plays on its own entity.
		local swing = Scene.FindEntityByName("Swing")
		local clip = swing:GetComponent("Animation")
		assert(clip.Playing and clip.Loop and clip.Time > 0.5, "the Swing clip should be playing")
		local height = swing:GetComponent("Transform").Translation.y
		assert(height > 1.2 and height < 2.0, "the Swing clip should have lifted its entity (y " .. height .. ")")
		self.done = true
		Log.Info("FeatureTest: all checks passed at frame " .. Time.frame)
		if not self.reloaded then
			-- Restart the scene once; the reloaded run checks that the Game value came through.
			Game.Set("FeatureTestRuns", 1)
			Game.Set("Note", "kept")
			Scene.Reload()
		end
	end
end

function FeatureTest:OnDestroy()
	-- The reloaded run may be cut short by the end of the run; the first one must have finished.
	if self.entity.tag ~= "Spawned" and not self.reloaded then
		assert(self.done, "the feature test did not run to completion (run at least " .. self.finishFrame .. " frames)")
	end
	if self.reloaded then
		assert(Game.Get("FeatureTestRuns") == 1 and Game.Get("Note") == "kept", "Game values should survive Scene.Reload")
	end
end

return FeatureTest
