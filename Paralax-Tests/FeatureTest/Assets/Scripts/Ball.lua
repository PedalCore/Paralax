-- Falls through the trigger zone onto the ground; records the physics callbacks (and fixed steps) for FeatureTest.lua
-- to check.
local Ball = {}

function Ball:OnFixedUpdate(dt)
	assert(math.abs(dt - 1 / 60) < 1e-6, "OnFixedUpdate should get the physics step")
	BallFixedSteps = (BallFixedSteps or 0) + 1
end

function Ball:OnCollisionEnter(other)
	if other.name == "Ground" then BallLanded = true end
end

function Ball:OnCollisionExit(other)
	BallLeft = other.name
end

function Ball:OnTriggerEnter(other)
	if other.name == "TriggerZone" then BallTriggered = true end
end

function Ball:OnTriggerExit(other)
	if other.name == "TriggerZone" then BallPassedTrigger = true end
end

return Ball
