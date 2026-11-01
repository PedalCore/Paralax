-- Gem Roller: the ball, and the game around it. Built through editor commands by build_gem_roller.py.
--
-- Arrow keys roll the ball (forces, applied at the physics rate in OnFixedUpdate so the ball handles the same at any
-- frame rate); Space jumps when the ball is on the ground. Collect every gem, then reach the goal at the far end; the
-- moving platform carries the ball across the gap. Falling off costs a life and puts the ball back at the start;
-- with no lives left, or after a win, the scene restarts. The best time is kept in Game across restarts.
local Roller = {
	push = 14,          -- N on the 1 kg ball.
	jumpSpeed = 5.5,    -- m/s upwards.
	killHeight = -8,    -- Below this the ball has fallen off.
	lives = 3,
}

local Start = vec3(0, 1, 2)

function Roller:OnCreate()
	RollerGame = self -- For script.eval: inspect and drive the game in tests.
	self.gemsLeft = #Scene.FindEntitiesByTag("Gem")
	self.gemsTotal = self.gemsLeft
	self.livesLeft = self.lives
	self.time = 0
	self.won, self.over, self.endTime = false, false, 0
	self.jumpQueued = false
	self.jumps = 0
	self.hud = Scene.FindEntityByName("Hud"):GetComponent("Text")
	self.message = Scene.FindEntityByName("Message"):GetComponent("Text")
	self.message.Text = ""
end

function Roller:ShowHud()
	self.hud.Text = string.format("Gems %d/%d\nLives %d\nTime %.1f", self.gemsTotal - self.gemsLeft, self.gemsTotal, self.livesLeft, self.time)
end

-- Edges of key presses are per frame, and a frame may hold any number of physics steps: the press is noted here and
-- acted on at the next step.
function Roller:OnUpdate(dt)
	if self.won or self.over then
		self.endTime = self.endTime + dt
		if self.endTime >= 3 then
			Scene.Reload()
		end
		return
	end
	self.time = self.time + dt
	self:ShowHud()
	if Input.WasKeyPressed(Key.Space) then
		self.jumpQueued = true
	end
	if self.entity:GetWorldPosition().y < self.killHeight then
		self:LoseLife()
	end
end

function Roller:OnFixedUpdate(dt)
	if self.won or self.over then
		return
	end
	local push = vec3(0, 0, 0)
	if Input.IsKeyDown(Key.Left) then push.x = push.x - 1 end
	if Input.IsKeyDown(Key.Right) then push.x = push.x + 1 end
	if Input.IsKeyDown(Key.Up) then push.z = push.z - 1 end
	if Input.IsKeyDown(Key.Down) then push.z = push.z + 1 end
	if push:Length() > 0 then
		Physics.AddForce(self.entity, push:Normalized() * self.push)
	end
	if self.jumpQueued then
		self.jumpQueued = false
		if self:OnGround() then
			local velocity = Physics.GetLinearVelocity(self.entity)
			Physics.SetLinearVelocity(self.entity, vec3(velocity.x, self.jumpSpeed, velocity.z))
			self.jumps = self.jumps + 1
			Audio.PlayOneShot("Audio/Jump.wav")
		end
	end
end

-- A short ray down from just under the ball (starting outside it, so it cannot hit the ball itself).
function Roller:OnGround()
	local bottom = self.entity:GetWorldPosition() - vec3(0, 0.51, 0)
	return Physics.Raycast(bottom, vec3(0, -1, 0), 0.12) ~= nil
end

function Roller:Teleport(position)
	self.entity:GetComponent("Transform").Translation = position
	Physics.SetLinearVelocity(self.entity, vec3(0, 0, 0))
	Physics.SetAngularVelocity(self.entity, vec3(0, 0, 0))
end

function Roller:LoseLife()
	self.livesLeft = self.livesLeft - 1
	Audio.PlayOneShot("Audio/Fall.wav")
	if self.livesLeft <= 0 then
		self.over = true
		self.message.Text = "OUT OF LIVES"
		Log.Info("Gem Roller: out of lives")
	else
		self:Teleport(Start)
		Log.Info("Gem Roller: fell off, " .. self.livesLeft .. " lives left")
	end
end

-- Called by Gem.lua when the ball touches a gem.
function Roller:Collect(gem)
	Audio.PlayOneShot("Audio/Gem.wav", gem:GetWorldPosition())
	gem:Destroy()
	self.gemsLeft = self.gemsLeft - 1
	Log.Info("Gem Roller: gem collected, " .. self.gemsLeft .. " to go")
end

-- Called by Goal.lua when the ball reaches the goal.
function Roller:ReachGoal()
	if self.won or self.over then
		return
	end
	if self.gemsLeft > 0 then
		if not self.goalHinted then
			self.goalHinted = true
			Log.Info("Gem Roller: collect the " .. self.gemsLeft .. " remaining gems first")
		end
		return
	end
	self.won = true
	local best = Game.Get("BestTime")
	if best == nil or self.time < best then
		best = self.time
		Game.Set("BestTime", best)
	end
	Game.Set("Wins", Game.Get("Wins", 0) + 1)
	Audio.PlayOneShot("Audio/Win.wav")
	self.message.Text = string.format("FINISHED\n%.1f s (best %.1f s)", self.time, best)
	Log.Info(string.format("Gem Roller: finished in %.2f s (best %.2f s)", self.time, best))
end

return Roller
