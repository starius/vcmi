-- Mirrors AI/Nullkiller2/Goals/BuildThis.{h,cpp}: BuildThis identity, string, and action intent.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local BuildThis = CGoal.derive("BuildThis", AbstractGoal.EGoals.BUILD_STRUCTURE, { elementar = true })

local function numericID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.num or value.id or value[1]
	end
	return value
end

local function townName(town)
	return AbstractGoal._helpers.translatedName(town, tostring(CGoal.objectID(town)))
end

local function buildingName(buildingInfo, bid, town)
	if buildingInfo and buildingInfo.name then
		return buildingInfo.name
	end
	if town and town.buildings and town.buildings[bid] then
		return town.buildings[bid].name or tostring(bid)
	end
	return tostring(bid)
end

function BuildThis:init(buildingInfoOrBid, townInfoOrTown)
	if buildingInfoOrBid == nil then
		return
	end

	if type(buildingInfoOrBid) == "table" and type(townInfoOrTown) == "table" and townInfoOrTown.town then
		self.buildingInfo = buildingInfoOrBid
		self.townInfo = townInfoOrTown
		self.bid = numericID(buildingInfoOrBid.id)
		self.town = townInfoOrTown.town
		return
	end

	self.bid = numericID(buildingInfoOrBid)
	self.town = townInfoOrTown
	self.buildingInfo = {
		id = buildingInfoOrBid,
		name = buildingName(nil, self.bid, self.town)
	}
	self.townInfo = {
		town = self.town
	}
end

function BuildThis:equalsTyped(other)
	return self.town == other.town and self.bid == other.bid
end

function BuildThis:toString()
	return "Build " .. buildingName(self.buildingInfo, self.bid, self.town) .. " in " .. townName(self.town)
end

function BuildThis:accept(aiGw)
	if aiGw and type(aiGw.buildBuilding) == "function" then
		return aiGw:buildBuilding(self.town, self.bid)
	end

	error("Cannot build a given structure!", 2)
end

return BuildThis
