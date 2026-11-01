-- Follows the ball from behind and above, easing towards it so the motion reads smoothly.
local FollowCamera = { offset = vec3(0, 6, 9), ease = 4.0 }

function FollowCamera:OnCreate()
	self.ball = Scene.FindEntityByName("Ball")
	local transform = self.entity:GetComponent("Transform")
	transform.Rotation = quat.FromEuler(vec3(-0.55, 0, 0))
	transform.Translation = self.ball:GetWorldPosition() + self.offset
end

function FollowCamera:OnUpdate(dt)
	local transform = self.entity:GetComponent("Transform")
	-- The same fraction of the way per second at any frame rate.
	local t = 1 - math.exp(-self.ease * dt)
	transform.Translation = transform.Translation:Lerp(self.ball:GetWorldPosition() + self.offset, t)
end

return FollowCamera
