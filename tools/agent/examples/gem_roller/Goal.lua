-- The finish: a trigger zone at the far end of the course.
local Goal = {}

function Goal:OnTriggerEnter(other)
	if other.name == "Ball" and RollerGame then
		RollerGame:ReachGoal()
	end
end

return Goal
