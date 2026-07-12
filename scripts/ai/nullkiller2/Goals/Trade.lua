-- Mirrors AI/Nullkiller2/Goals/Trade.{h,cpp}: Trade.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local Trade = CGoal.derive("Trade", AbstractGoal.EGoals.TRADE)

local function resourceID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.num or value.id or value[1]
	end
	return value
end

function Trade:init(rid, val, Objid)
	if rid ~= nil then
		self.resID = resourceID(rid)
		self.value = val
		self.objid = Objid
	end
end

function Trade:equalsTyped(other)
	return self.resID == other.resID
end

return Trade
