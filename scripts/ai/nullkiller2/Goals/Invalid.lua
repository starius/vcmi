-- Mirrors AI/Nullkiller2/Goals/Invalid.h: Invalid.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local Invalid = CGoal.derive("Invalid", AbstractGoal.EGoals.INVALID, { elementar = true })

function Invalid:init()
	self.priority = AbstractGoal.LOW_PR
end

function Invalid:decompose(_aiNk)
	return {}
end

function Invalid:equalsTyped(_other)
	return true
end

function Invalid:toString()
	return "Invalid"
end

function Invalid:accept(_aiGw)
	error("Can not fulfill Invalid goal!", 2)
end

return Invalid
