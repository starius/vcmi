-- Mirrors AI/Nullkiller2/Goals/BuildBoat.{h,cpp}: BuildBoat.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local BuildBoat = CGoal.derive("BuildBoat", AbstractGoal.EGoals.BUILD_BOAT, { elementar = true })
local GOOD_SHIPYARD = 0
local RESOURCE_COUNT = 7

local function call(object, name, ...)
	if type(object) == "table" and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function getResource(resources, resourceID)
	if not resources then
		return 0
	end
	if resources[0] == nil and resources[7] ~= nil then
		return resources[resourceID + 1] or 0
	end
	return resources[resourceID] or 0
end

local function canAfford(resources, cost)
	if not cost then
		return true
	end
	for resourceID = 0, RESOURCE_COUNT - 1 do
		if getResource(resources, resourceID) < getResource(cost, resourceID) then
			return false
		end
	end
	return true
end

local function boatCost(shipyard)
	local cost = call(shipyard, "getBoatCost")
	if cost ~= nil then
		return cost
	end
	return shipyard and (shipyard.boatCost or shipyard.cost)
end

local function freeResources(aiGw)
	if aiGw and type(aiGw.getFreeResources) == "function" then
		return aiGw:getFreeResources()
	end
	return aiGw and (aiGw.freeResources or aiGw.resources) or {}
end

local function relationIsEnemy(shipyard)
	if not shipyard then
		return false
	end
	local relation = shipyard.relation or shipyard.relationToOwner or shipyard.playerRelation
	return shipyard.enemy == true or relation == "ENEMIES" or relation == "enemy"
end

local function shipyardStatus(shipyard)
	local status = call(shipyard, "shipyardStatus")
	if status ~= nil then
		return status
	end
	return shipyard and (shipyard.shipyardStatus or shipyard.status)
end

local function shipyardReady(shipyard)
	local status = shipyardStatus(shipyard)
	return status == nil or status == GOOD_SHIPYARD or status == "GOOD"
end

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
	if not canAfford(freeResources(aiGw), boatCost(self.shipyard)) then
		error("Can not afford boat", 2)
	end

	if relationIsEnemy(self.shipyard) then
		error("Can not build boat in enemy shipyard", 2)
	end

	if not shipyardReady(self.shipyard) then
		error("Shipyard is busy.", 2)
	end

	if aiGw and type(aiGw.buildBoat) == "function" then
		return aiGw:buildBoat(self.shipyard)
	end

	error("Can not build boat", 2)
end

return BuildBoat
