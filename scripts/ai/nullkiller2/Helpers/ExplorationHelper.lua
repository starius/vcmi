-- Mirrors AI/Nullkiller2/Helpers/ExplorationHelper.{h,cpp}: evaluator and composition policy.

local AbstractGoal = require("Goals.AbstractGoal")
local Composition = require("Goals.Composition")
local ExploreNeighbourTile = require("Goals.ExploreNeighbourTile")
local ExplorationPoint = require("Markers.ExplorationPoint")
local Invalid = require("Goals.Invalid")

local ExplorationHelper = {}
ExplorationHelper.__index = ExplorationHelper

local function objectID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function helperSnapshot(hero, aiNk)
	local byHero = aiNk and aiNk.explorationHelpers or {}
	return hero and (hero.explorationHelper or byHero[objectID(hero)]) or {}
end

local function evaluateDimensionDoorExplorationCandidate(candidate)
	local evaluation = {
		accepted = false,
		value = 0.0,
		tilesDiscovered = 0
	}

	if (candidate.tilesDiscovered or 0) == 0
		and (candidate.continuationTilesDiscovered or 0) == 0
		and (candidate.chainTilesDiscovered or 0) == 0
		and (candidate.strategicScore or 0.0) <= 0.0 then
		return evaluation
	end

	if candidate.reachableWithoutDimensionDoor then
		return evaluation
	end

	if candidate.dimensionDoorTriggersGuards then
		if not candidate.visible then
			return evaluation
		end

		if (candidate.guardedLandingDanger or 0) ~= 0 and not candidate.guardedLandingSafe then
			return evaluation
		end
	end

	local movementLimit = math.max(1, candidate.movementPointsLimit or 1)
	local movementSpent = math.min(candidate.movementPointsRemaining or 0, candidate.movementPointsTaken or 0)
	local movementCost = math.max(0.1, movementSpent / movementLimit)
	local discoveryValue = (candidate.tilesDiscovered or 0) * (candidate.tilesDiscovered or 0)
	local continuationValue = (candidate.continuationTilesDiscovered or 0) * (candidate.continuationTilesDiscovered or 0)
	local chainValue = (candidate.chainTilesDiscovered or 0) * (candidate.chainTilesDiscovered or 0)
	local value = (discoveryValue + continuationValue + chainValue + (candidate.strategicScore or 0.0) * 25.0) / movementCost

	if value <= (candidate.currentBestValue or 0.0) then
		return evaluation
	end

	evaluation.accepted = true
	evaluation.value = value
	evaluation.tilesDiscovered = math.max(1, (candidate.tilesDiscovered or 0) + (candidate.chainTilesDiscovered or 0))
	return evaluation
end

local function shouldExploreNeighbourAfterExplorationGoal(goalType)
	return goalType ~= AbstractGoal.EGoals.ADVENTURE_SPELL_CAST
end

function ExplorationHelper.new(hero, aiNk, _useCPathfinderAccessibility)
	local snapshot = helperSnapshot(hero, aiNk)
	return setmetatable({
		hero = hero,
		aiNk = aiNk,
		snapshot = snapshot,
		bestGoal = snapshot.bestGoal or Invalid.new(),
		bestTile = snapshot.bestTile,
		bestTilesDiscovered = snapshot.bestTilesDiscovered or 0
	}, ExplorationHelper)
end

function ExplorationHelper:makeComposition()
	local composition = Composition.new()
	composition:addNext(ExplorationPoint.new(self.bestTile, self.bestTilesDiscovered))
	if shouldExploreNeighbourAfterExplorationGoal(self.bestGoal.goalType) then
		composition:addNextSequence({ self.bestGoal, ExploreNeighbourTile.new(self.hero, 5) })
	else
		composition:addNext(self.bestGoal)
	end
	return composition
end

function ExplorationHelper:canUseDimensionDoor()
	return self.snapshot.canUseDimensionDoor == true
end

function ExplorationHelper:scanSector(scanRadius)
	local result = self.snapshot.scanSector and self.snapshot.scanSector[scanRadius]
	if result then
		self.bestGoal = result.bestGoal or self.bestGoal
		self.bestTile = result.bestTile or self.bestTile
		self.bestTilesDiscovered = result.bestTilesDiscovered or self.bestTilesDiscovered
		return true
	end
	return false
end

function ExplorationHelper:scanMap()
	local result = self.snapshot.scanMap
	if result then
		self.bestGoal = result.bestGoal or self.bestGoal
		self.bestTile = result.bestTile or self.bestTile
		self.bestTilesDiscovered = result.bestTilesDiscovered or self.bestTilesDiscovered
		return true
	end
	return false
end

function ExplorationHelper:considerDimensionDoorExplorationTargets()
	local result = self.snapshot.dimensionDoor
	if result then
		self.bestGoal = result.bestGoal or self.bestGoal
		self.bestTile = result.bestTile or self.bestTile
		self.bestTilesDiscovered = result.bestTilesDiscovered or self.bestTilesDiscovered
		return true
	end
	return false
end

function ExplorationHelper:howManyTilesWillBeDiscovered(pos)
	local key = pos and (tostring(pos.x) .. "," .. tostring(pos.y) .. "," .. tostring(pos.z))
	return self.snapshot.tilesDiscoveredByTile and self.snapshot.tilesDiscoveredByTile[key] or 0
end

ExplorationHelper.evaluateDimensionDoorExplorationCandidate = evaluateDimensionDoorExplorationCandidate
ExplorationHelper.shouldExploreNeighbourAfterExplorationGoal = shouldExploreNeighbourAfterExplorationGoal

return ExplorationHelper
