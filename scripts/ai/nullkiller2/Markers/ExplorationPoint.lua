-- Mirrors AI/Nullkiller2/Markers/ExplorationPoint.{h,cpp}: ExplorationPoint.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local ExplorationPoint = CGoal.derive("ExplorationPoint", AbstractGoal.EGoals.EXPLORATION_POINT)

function ExplorationPoint:init(tile, tilesToReveal)
	self:settile(tile)
	self:setvalue(tilesToReveal or 0)
end

function ExplorationPoint:equalsTyped(_other)
	return false
end

function ExplorationPoint:toString()
	return "Explore " .. AbstractGoal._helpers.tileToString(self.tile) .. " for " .. tostring(self.value) .. " tiles"
end

return ExplorationPoint
