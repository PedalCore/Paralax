-- A module loaded with require() by the feature test.
local Util = {}

function Util.Clamp(value, low, high)
	return math.max(low, math.min(high, value))
end

return Util
