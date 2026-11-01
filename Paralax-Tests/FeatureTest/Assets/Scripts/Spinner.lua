-- Spins its entity about +Y; gives the feature scene some motion and another script instance to talk to.
local Spinner = { speed = 1.5 }

function Spinner:OnUpdate(dt)
	local transform = self.entity:GetComponent("Transform")
	transform.Rotation = quat.AngleAxis(self.speed * dt, vec3(0, 1, 0)) * transform.Rotation
end

return Spinner
