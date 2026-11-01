-- A kinematic platform sliding back and forth along Z across the gap. It is moved in OnFixedUpdate, so it advances
-- the same amount every physics step and carries the ball smoothly at any frame rate.
local Platform = { range = 2.8, speed = 0.9 }

function Platform:OnCreate()
	self.centre = self.entity:GetComponent("Transform").Translation
	self.phase = 0
end

function Platform:OnFixedUpdate(dt)
	self.phase = self.phase + self.speed * dt
	self.entity:GetComponent("Transform").Translation = self.centre + vec3(0, 0, self.range * math.sin(self.phase))
end

return Platform
