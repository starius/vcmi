-- Mirrors AI/Nullkiller2/Behaviors/EscapeBehavior.{h,cpp}: EscapeBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")

local EscapeBehavior = CGoal.derive("EscapeBehavior", AbstractGoal.EGoals.ESCAPE_BEHAVIOR)

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function isAlmostEqual(lhs, rhs)
	return math.abs((lhs or 0) - (rhs or 0)) < 0.000001
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function targetTile(path)
	return call(path, "targetTile") or path and (path.targetTile or path.tile)
end

local function tileEquals(lhs, rhs)
	lhs = lhs or {}
	rhs = rhs or {}
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

local function movementCost(path)
	local result = call(path, "movementCost")
	if result ~= nil then
		return result
	end
	return path and path.movementCost or math.huge
end

local function pathTurn(path)
	local result = call(path, "turn")
	if result ~= nil then
		return result
	end
	return path and (path.turn or path.turns) or 0
end

local function firstBlockedAction(path)
	local result = call(path, "getFirstBlockedAction")
	if result ~= nil then
		return result
	end
	return path and path.firstBlockedAction
end

local function totalDanger(path)
	local result = call(path, "getTotalDanger")
	if result ~= nil then
		return result
	end
	return path and path.totalDanger or 0
end

local function exchangeCount(path)
	return path and path.exchangeCount or 0
end

local function actionUsesDimensionDoor(action)
	if not action then
		return false
	end
	if action.dimensionDoor or action.type == "DimensionDoorAction" or action.kind == "DimensionDoorAction" then
		return true
	end
	for _, part in ipairs(call(action, "getParts") or action.parts or {}) do
		if actionUsesDimensionDoor(part) then
			return true
		end
	end
	return false
end

local function pathUsesDimensionDoor(path)
	for _, node in ipairs(path and path.nodes or {}) do
		if actionUsesDimensionDoor(node.specialAction) then
			return true
		end
	end
	return false
end

local function normalizedHeroStrength(hero)
	local result = call(hero, "getHeroStrength")
	if result == nil then
		result = hero and (hero.heroStrength or hero.normalizedHeroStrength)
	end
	if type(result) ~= "number" or result <= 0 or result ~= result or result == math.huge or result == -math.huge then
		return 1.0
	end
	return result
end

local function armyStrength(army)
	local result = call(army, "getArmyStrength")
	if result ~= nil then
		return result
	end
	return army and (army.armyStrength or army.totalStrength or army.strength) or 0
end

local function isSafeToVisit(hero, heroArmy, dangerStrength, safeAttackRatio)
	if not dangerStrength or dangerStrength == 0 then
		return true
	end

	local heroStrength = normalizedHeroStrength(hero) * armyStrength(heroArmy or hero)
	return heroStrength > dangerStrength * safeAttackRatio
end

local function fastestDanger(tileThreat)
	return tileThreat and (tileThreat.fastestDanger or tileThreat) or { turn = math.huge, danger = 0, threat = 0 }
end

local function threatValue(threat)
	return threat and (threat.threat or threat.danger or 0) or 0
end

local function tileThreat(aiNk, tile)
	return fastestDanger(call(aiNk and aiNk.dangerHitMap, "getTileThreat", tile) or tile and tile.threatInfo)
end

local function isHeroImmediatelyThreatened(hero, threat, safeAttackRatio)
	return (threat.turn or math.huge) < 1
		and (threat.danger or 0) ~= 0
		and not isSafeToVisit(hero, hero, threat.danger, safeAttackRatio)
end

local function isBetterEscapePath(path, score, previous)
	if score > previous.score then
		return true
	end

	if isAlmostEqual(score, previous.score) then
		return movementCost(path) < movementCost(previous.path)
	end

	return false
end

local function makeEscapePathCandidate(path, currentThreat, destinationThreat, destinationDanger, safeAttackRatio)
	local destination = targetTile(path)

	return {
		currentTileThreatensHero = true,
		sameDay = pathTurn(path) == 0,
		sameTile = tileEquals(destination, visitablePos(path.targetHero)),
		blockedAction = firstBlockedAction(path) ~= nil,
		singleHeroPath = exchangeCount(path) <= 1,
		destinationSafe = isSafeToVisit(path.targetHero, path.heroArmy, destinationDanger, safeAttackRatio),
		destinationIsSafer = threatValue(destinationThreat) < threatValue(currentThreat),
		usesDimensionDoor = pathUsesDimensionDoor(path),
		threatReduction = threatValue(currentThreat) - threatValue(destinationThreat),
		movementCost = movementCost(path)
	}
end

local function evaluateEscapePathCandidate(candidate)
	local result = {
		accepted = false,
		score = 0.0
	}

	if not candidate.currentTileThreatensHero
		or not candidate.sameDay
		or candidate.sameTile
		or candidate.blockedAction
		or not candidate.singleHeroPath
		or not candidate.destinationSafe
		or not candidate.destinationIsSafer then
		return result
	end

	result.accepted = true
	result.score = candidate.threatReduction / math.max(0.1, candidate.movementCost)
	return result
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

local function safeAttackRatio(aiNk)
	return call(aiNk and aiNk.settings, "getSafeAttackRatio")
		or aiNk and aiNk.settings and aiNk.settings.safeAttackRatio
		or aiNk and aiNk.safeAttackRatio
		or 1.1
end

local function allTilePaths(aiNk)
	if aiNk and aiNk.escapePaths then
		return { aiNk.escapePaths }
	end
	if aiNk and aiNk.tilePaths then
		local result = {}
		for _, entry in ipairs(aiNk.tilePaths) do
			table.insert(result, entry.paths or entry)
		end
		return result
	end
	return call(aiNk and aiNk.pathfinder, "getAllPathInfo") or {}
end

local function considerEscapePath(path, threatenedHeroes, aiNk, ratio, bestPaths)
	local currentThreat = threatenedHeroes[path.targetHero] or threatenedHeroes[objectID(path.targetHero)]
	if not currentThreat then
		return
	end

	local destination = targetTile(path)
	local destinationThreat = tileThreat(aiNk, destination)
	local immediateDestinationDanger = (destinationThreat.turn or math.huge) < 1 and (destinationThreat.danger or 0) or 0
	local destinationDanger = math.max(totalDanger(path), immediateDestinationDanger)
	local candidate = makeEscapePathCandidate(path, currentThreat, destinationThreat, destinationDanger, ratio)
	local evaluation = evaluateEscapePathCandidate(candidate)

	if not evaluation.accepted then
		return
	end

	local key = objectID(path.targetHero) or path.targetHero
	local previous = bestPaths[key]
	if not previous or isBetterEscapePath(path, evaluation.score, previous) then
		bestPaths[key] = {
			path = path,
			score = evaluation.score
		}
	end
end

function EscapeBehavior:init()
	self.goalType = AbstractGoal.EGoals.ESCAPE_BEHAVIOR
end

function EscapeBehavior:toString()
	return "Escape"
end

function EscapeBehavior:equalsTyped(_other)
	return true
end

function EscapeBehavior:decompose(aiNk)
	local tasks = {}
	local threatenedHeroes = {}
	local threatenedCount = 0
	local ratio = safeAttackRatio(aiNk)

	for _, hero in ipairs(heroesInfo(aiNk)) do
		if not isHeroLocked(aiNk, hero) then
			local threat = tileThreat(aiNk, visitablePos(hero))
			if isHeroImmediatelyThreatened(hero, threat, ratio) then
				threatenedHeroes[hero] = threat
				threatenedHeroes[objectID(hero)] = threat
				threatenedCount = threatenedCount + 1
			end
		end
	end

	if threatenedCount == 0 then
		return tasks
	end

	local bestPaths = {}
	for _, paths in ipairs(allTilePaths(aiNk)) do
		for _, path in ipairs(paths) do
			considerEscapePath(path, threatenedHeroes, aiNk, ratio, bestPaths)
		end
	end

	for _, bestPath in pairs(bestPaths) do
		table.insert(tasks, ExecuteHeroChain.new(bestPath.path))
	end

	return tasks
end

EscapeBehavior.evaluateEscapePathCandidate = evaluateEscapePathCandidate
EscapeBehavior.makeEscapePathCandidate = makeEscapePathCandidate
EscapeBehavior.isSafeToVisit = isSafeToVisit
EscapeBehavior.pathUsesDimensionDoor = pathUsesDimensionDoor

return EscapeBehavior
