-- Mirrors AI/Nullkiller2/Goals/BuildBoat.{h,cpp}: BuildBoat.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local BuildBoat = CGoal.derive("BuildBoat", AbstractGoal.EGoals.BUILD_BOAT, { elementar = true })

function BuildBoat:init(shipyard)
	self.shipyard = shipyard
end

function BuildBoat:equalsTyped(other)
	return self.shipyard == other.shipyard
end

function BuildBoat:toString()
	return "BuildBoat"
end

function BuildBoat:accept(aiGw)
	if aiGw and type(aiGw.buildBoat) == "function" then
		return aiGw:buildBoat(self.shipyard)
	end

	error("Can not build boat", 2)
end

return BuildBoat
