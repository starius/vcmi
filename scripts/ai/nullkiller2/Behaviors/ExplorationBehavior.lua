-- Mirrors AI/Nullkiller2/Behaviors/ExplorationBehavior.{h,cpp}: ExplorationBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local CaptureObject = require("Goals.CaptureObject")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local ExplorationHelper = require("Helpers.ExplorationHelper")
local ExplorationPoint = require("Markers.ExplorationPoint")
local ExploreNeighbourTile = require("Goals.ExploreNeighbourTile")
local State = require("Engine.State")

local ExplorationBehavior = CGoal.derive("ExplorationBehavior", AbstractGoal.EGoals.EXPLORATION_BEHAVIOR)

local Obj = {
	BOAT = 8,
	MONOLITH_ONE_WAY_ENTRANCE = 43,
	MONOLITH_TWO_WAY = 45,
	REDWOOD_OBSERVATORY = 58,
	PILLAR_OF_FIRE = 60,
	SUBTERRANEAN_GATE = 103,
	WHIRLPOOL = 111
}

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return type(value) == "table" and (value.id or value.objectID or value.objectId or value.num or value[1]) or value
end

local function objectType(object)
	return object and (object.ID or object.idType or object.type)
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function isVisible(aiNk, tile)
	local result = call(aiNk and aiNk.cc, "isVisible", tile)
	if result ~= nil then
		return result
	end
	return tile and tile.visible == true
end

local function heroesInfo(aiNk)
	return call(aiNk and aiNk.cc, "getHeroesInfo") or aiNk and aiNk.heroesInfo or {}
end

local function isHeroLocked(aiNk, hero)
	local result = call(aiNk, "isHeroLocked", hero)
	if result ~= nil then
		return result
	end
	local lockedHeroes = aiNk and aiNk.lockedHeroes or {}
	return lockedHeroes[objectID(hero)] ~= nil
end

local function scanDepth(aiNk)
	return call(aiNk, "getScanDepth") or aiNk and aiNk.scanDepth
end

local function visitableObjects(aiNk)
	return aiNk and aiNk.visitableObjects
		or call(aiNk and aiNk.memory, "visitableIdsToObjsVector", aiNk and aiNk.cc)
		or {}
end

local function topObject(aiNk, tile)
	return call(aiNk and aiNk.cc, "getTopObj", tile) or tile and tile.topObject
end

local function evaluateBoatExplorationCandidate(candidate)
	local evaluation = {
		accepted = false,
		explorationValue = 0
	}

	if not candidate.available or (candidate.hiddenTilesDiscovered or 0) <= 0 then
		return evaluation
	end

	evaluation.accepted = true
	evaluation.explorationValue = candidate.hiddenTilesDiscovered
	return evaluation
end

local function getBoatExplorationValue(aiNk, object)
	if objectType(object) ~= Obj.BOAT and objectType(object) ~= "BOAT" then
		return nil
	end

	local topObj = topObject(aiNk, visitablePos(object))
	local available = object.available
	if available == nil then
		available = topObj and objectID(topObj) == objectID(object)
	end

	local candidate = {
		available = available == true,
		hiddenTilesDiscovered = object.hiddenTilesDiscovered or object.boatExplorationValue or 0
	}
	local evaluation = evaluateBoatExplorationCandidate(candidate)
	return evaluation.accepted and evaluation.explorationValue or nil
end

local function addExplorationCaptureTask(tasks, object, value)
	local composition = Composition.new()
	composition:addNext(ExplorationPoint.new(visitablePos(object), value))
	composition:addNext(CaptureObject.new(object))
	table.insert(tasks, composition)
end

local function wasScouted(aiNk, object)
	local result = call(object, "wasScouted", aiNk and aiNk.playerID)
	if result ~= nil then
		return result
	end
	return object and object.wasScouted == true
end

local function teleportExits(aiNk, object)
	return call(aiNk and aiNk.cc, "getTeleportChannelExits", object and object.channel) or object and object.exits or {}
end

local function objectByID(aiNk, id)
	return call(aiNk and aiNk.cc, "getObjInstance", id) or aiNk and aiNk.objectsByID and aiNk.objectsByID[id]
end

function ExplorationBehavior:init()
	self.goalType = AbstractGoal.EGoals.EXPLORATION_BEHAVIOR
end

function ExplorationBehavior:toString()
	return "Explore"
end

function ExplorationBehavior:equalsTyped(_other)
	return true
end

function ExplorationBehavior:decompose(aiNk)
	local tasks = {}

	for _, object in ipairs(visitableObjects(aiNk)) do
		local typeID = objectType(object)
		if typeID == Obj.BOAT or typeID == "BOAT" then
			local explorationValue = getBoatExplorationValue(aiNk, object)
			if explorationValue then
				addExplorationCaptureTask(tasks, object, explorationValue)
			end
		elseif typeID == Obj.REDWOOD_OBSERVATORY or typeID == Obj.PILLAR_OF_FIRE
			or typeID == "REDWOOD_OBSERVATORY" or typeID == "PILLAR_OF_FIRE" then
			if not wasScouted(aiNk, object) then
				addExplorationCaptureTask(tasks, object, 200)
			end
		elseif typeID == Obj.MONOLITH_ONE_WAY_ENTRANCE
			or typeID == Obj.MONOLITH_TWO_WAY
			or typeID == Obj.SUBTERRANEAN_GATE
			or typeID == Obj.WHIRLPOOL
			or typeID == "MONOLITH_ONE_WAY_ENTRANCE"
			or typeID == "MONOLITH_TWO_WAY"
			or typeID == "SUBTERRANEAN_GATE"
			or typeID == "WHIRLPOOL" then
			for _, exit in ipairs(teleportExits(aiNk, object)) do
				if exit ~= objectID(object) then
					local exitObject = objectByID(aiNk, exit)
					if exitObject and not isVisible(aiNk, visitablePos(exitObject)) then
						addExplorationCaptureTask(tasks, object, 50)
					end
				end
			end
		end
	end

	for _, hero in ipairs(heroesInfo(aiNk)) do
		if not isHeroLocked(aiNk, hero) then
			local scanResult = ExplorationHelper.new(hero, aiNk)
			local canUseDimensionDoor = scanResult:canUseDimensionDoor()
			local function improveWithDimensionDoor()
				return canUseDimensionDoor and scanResult:considerDimensionDoorExplorationTargets()
			end

			local foundNearbyTarget = scanResult:scanSector(1)
			if foundNearbyTarget then
				improveWithDimensionDoor()
				table.insert(tasks, scanResult:makeComposition())
			else
				local foundSectorTarget = scanResult:scanSector(30)
				local foundDimensionDoorTarget = improveWithDimensionDoor()

				if foundSectorTarget or foundDimensionDoorTarget then
					table.insert(tasks, scanResult:makeComposition())
				elseif scanDepth(aiNk) == State.ScanDepth.ALL_FULL then
					if scanResult:scanMap() then
						table.insert(tasks, scanResult:makeComposition())
					else
						local neighbourTarget = ExploreNeighbourTile.findTarget(hero, aiNk)
						if neighbourTarget then
							local composition = Composition.new()
							composition:addNext(ExplorationPoint.new(neighbourTarget.tile, neighbourTarget.tilesDiscovered))
							composition:addNext(ExploreNeighbourTile.new(hero, 5))
							table.insert(tasks, composition)
						end
					end
				end
			end
		end
	end

	return tasks
end

ExplorationBehavior.Obj = Obj
ExplorationBehavior.evaluateBoatExplorationCandidate = evaluateBoatExplorationCandidate

return ExplorationBehavior
