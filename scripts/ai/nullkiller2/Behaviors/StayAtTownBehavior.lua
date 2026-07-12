-- Mirrors AI/Nullkiller2/Behaviors/StayAtTownBehavior.{h,cpp}: StayAtTownBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")
local StayAtTown = require("Goals.StayAtTown")

local StayAtTownBehavior = CGoal.derive("StayAtTownBehavior", AbstractGoal.EGoals.STAY_AT_TOWN_BEHAVIOR)

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function sameObject(lhs, rhs)
	if lhs == rhs then
		return true
	end

	local lhsID = objectID(lhs)
	local rhsID = objectID(rhs)
	return lhsID ~= nil and lhsID == rhsID
end

local function townsInfo(aiNk)
	return call(aiNk and aiNk.cc, "getTownsInfo") or aiNk and aiNk.townsInfo or {}
end

local function visitablePos(town)
	return call(town, "visitablePos") or town and (town.visitablePos or town.tile)
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town and town.visitingHero
end

local function firstBlockedAction(path)
	local result = call(path, "getFirstBlockedAction")
	if result ~= nil then
		return result
	end
	return path and path.firstBlockedAction
end

local function exchangeCount(path)
	return path and path.exchangeCount or 0
end

local function calculatePathInfo(aiNk, town)
	local paths = {}
	local pathfinder = aiNk and aiNk.pathfinder

	if pathfinder and type(pathfinder.calculatePathInfo) == "function" then
		local result = pathfinder:calculatePathInfo(paths, visitablePos(town))
		if result ~= nil then
			return result
		end
		return paths
	end

	return town and town.paths or {}
end

function StayAtTownBehavior:init()
	self.goalType = AbstractGoal.EGoals.STAY_AT_TOWN_BEHAVIOR
end

function StayAtTownBehavior:toString()
	return "StayAtTownBehavior"
end

function StayAtTownBehavior:equalsTyped(_other)
	return true
end

function StayAtTownBehavior:decompose(aiNk)
	local tasks = {}
	local towns = townsInfo(aiNk)

	if #towns == 0 then
		return tasks
	end

	for _, town in ipairs(towns) do
		for _, path in ipairs(calculatePathInfo(aiNk, town)) do
			local currentVisitor = visitingHero(town)
			if not (currentVisitor and not sameObject(currentVisitor, path.targetHero))
				and not firstBlockedAction(path)
				and exchangeCount(path) <= 1 then
				local stayAtTown = Composition.new()
				stayAtTown:addNextSequence({
					ExecuteHeroChain.new(path),
					StayAtTown.new(town, path)
				})
				table.insert(tasks, stayAtTown)
			end
		end
	end

	return tasks
end

return StayAtTownBehavior
