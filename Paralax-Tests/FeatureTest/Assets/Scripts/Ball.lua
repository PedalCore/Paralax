-- Falls through the trigger zone onto the ground; records the physics callbacks for FeatureTest.lua to check.
local Ball = {}

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
