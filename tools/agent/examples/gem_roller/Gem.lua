-- A gem: collected when the ball touches it (the entity's trigger collider). Its mesh is a child that spins, so the
-- trigger itself never moves.
local Gem = { spin = 2.0 }

function Gem:OnCreate()
	self.mesh = self.entity:GetChildren()[1]
	self.angle = 0
end

function Gem:OnUpdate(dt)
	self.angle = self.angle + self.spin * dt
	if self.mesh then
		self.mesh:GetComponent("Transform").Rotation = quat.FromEuler(vec3(0.6, self.angle, 0))
	end
end

function Gem:OnTriggerEnter(other)
	if other.name == "Ball" and RollerGame then
		RollerGame:Collect(self.entity)
	end
end

return Gem
