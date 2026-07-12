-- Mirrors AI/Nullkiller2/Behaviors/StartupBehavior.{h,cpp}: StartupBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local BuildThis = require("Goals.BuildThis")
local CGoal = require("Goals.CGoal")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local RecruitHero = require("Goals.RecruitHero")
local State = require("Engine.State")

local StartupBehavior = CGoal.derive("StartupBehavior", AbstractGoal.EGoals.STARTUP)

local BuildingID = {
	TAVERN = 5
}

local EBuildingState = {
	ALLOWED = 7
}

local EGameResID = {
	GOLD = 6
}

local Obj = {
	CAMPFIRE = 12,
	TREASURE_CHEST = 101,
	WATER_WHEEL = 109
}

local MIN_PRIORITY = 0.01

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
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

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function dist2dSQ(lhs, rhs)
	local result = call(lhs, "dist2dSQ", rhs)
	if result ~= nil then
		return result
	end
	lhs = lhs or {}
	rhs = rhs or {}
	local dx = (lhs.x or 0) - (rhs.x or 0)
	local dy = (lhs.y or 0) - (rhs.y or 0)
	return dx * dx + dy * dy
end

local function garrisonHero(town)
	return call(town, "getGarrisonHero") or town and town.garrisonHero
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town and town.visitingHero
end

local function townsInfo(aiNk)
	return call(aiNk and aiNk.cc, "getTownsInfo") or aiNk and aiNk.townsInfo or {}
end

local function heroesInfo(aiNk)
	return call(aiNk and aiNk.cc, "getHeroesInfo") or aiNk and aiNk.heroesInfo or {}
end

local function heroCount(aiNk)
	local result = call(aiNk and aiNk.cc, "getHeroCount", aiNk and aiNk.playerID, true)
	if result ~= nil then
		return result
	end
	return aiNk and aiNk.heroCount or #heroesInfo(aiNk)
end

local function mapSize(aiNk)
	return call(aiNk and aiNk.cc, "getMapSize") or aiNk and aiNk.mapSize or { x = 0 }
end

local function evaluateHero(aiNk, hero)
	return call(aiNk and aiNk.heroManager, "evaluateHero", hero) or hero and hero.evaluateHeroScore or 0
end

local function heroRole(aiNk, hero)
	return call(aiNk and aiNk.heroManager, "getHeroRoleOrDefaultInefficient", hero)
		or hero and hero.role
		or PriorityEvaluator.HeroRole.SCOUT
end

local function canRecruitHero(aiNk, town)
	local result = call(aiNk and aiNk.heroManager, "canRecruitHero", town)
	if result ~= nil then
		return result
	end
	return town and town.canRecruitHero ~= false
end

local function nearbyObjects(aiNk)
	return call(aiNk and aiNk.objectClusterizer, "getNearbyObjects") or aiNk and aiNk.nearbyObjects or {}
end

local function getPathInfo(aiNk, tile)
	return call(aiNk and aiNk.pathfinder, "getPathInfo", tile) or {}
end

local function upperArmy(town)
	return call(town, "getUpperArmy") or town and (town.upperArmy or town)
end

local function stacksCount(town)
	local result = call(town, "stacksCount")
	if result ~= nil then
		return result
	end
	return town and (town.stacksCount or town.armySize or 0) or 0
end

local function movementPointsRemaining(hero)
	return call(hero, "movementPointsRemaining") or hero and hero.movementPointsRemaining or 0
end

local function hasBuilt(town, buildingID)
	local result = call(town, "hasBuilt", buildingID)
	if result ~= nil then
		return result
	end
	if town and town.built then
		return town.built[buildingID] == true
	end
	if buildingID == BuildingID.TAVERN then
		return town and town.hasTavern == true
	end
	return false
end

local function isAllowedBuildState(value)
	return value == EBuildingState.ALLOWED or value == "ALLOWED" or value == true
end

local function canBuildStructure(aiNk, town, buildingID)
	local result = call(aiNk and aiNk.cc, "canBuildStructure", town, buildingID)
	if result ~= nil then
		return isAllowedBuildState(result)
	end
	if buildingID == BuildingID.TAVERN and town and town.canBuildTavern ~= nil then
		return town.canBuildTavern
	end
	return town and town.canBuildStructure == true
end

local function howManyReinforcementsCanGet(aiNk, ...)
	local result = call(aiNk and aiNk.armyManager, "howManyReinforcementsCanGet", ...)
	if result ~= nil then
		return result
	end
	return aiNk and aiNk.reinforcementsCanGet or 0
end

local function heroLockedReason(aiNk, hero)
	local result = call(aiNk, "getHeroLockedReason", hero)
	if result ~= nil then
		return result
	end
	local lockedHeroes = aiNk and aiNk.lockedHeroes or {}
	return lockedHeroes[objectID(hero)] or State.HeroLockedReason.NOT_LOCKED
end

local function objectType(object)
	return object and (object.ID or object.idType or object.type)
end

local function armyStrength(object)
	local result = call(object, "getArmyStrength")
	if result ~= nil then
		return result
	end
	return object and (object.armyStrength or object.strength or 0) or 0
end

local function resourceID(object)
	local result = call(object, "resourceID")
	if result ~= nil then
		return result
	end
	return object and object.resourceID
end

local function rewardGold(object)
	if object and object.rewardResources then
		return getResource(object.rewardResources, EGameResID.GOLD)
	end

	local infos = object and (object.rewardInfos or object.rewards or object.configurationInfo) or {}
	for _, info in ipairs(infos) do
		local reward = info.reward or info
		if getResource(reward.resources, EGameResID.GOLD) > 0 then
			return getResource(reward.resources, EGameResID.GOLD)
		end
	end

	return 0
end

local function isTreasureSource(object)
	if not object then
		return false
	end

	if armyStrength(object) > 0 then
		return false
	end

	local isGoldPile = resourceID(object) == EGameResID.GOLD or rewardGold(object) > 0
	local typeID = objectType(object)

	return isGoldPile
		or typeID == Obj.TREASURE_CHEST
		or typeID == "TREASURE_CHEST"
		or typeID == Obj.CAMPFIRE
		or typeID == "CAMPFIRE"
		or typeID == Obj.WATER_WHEEL
		or typeID == "WATER_WHEEL"
end

local function getShortestPath(town, paths)
	local shortestPath = nil
	local shortestScore = math.huge
	local townGarrisonHero = garrisonHero(town)

	for _, path in ipairs(paths or {}) do
		local score = movementCost(path)
		if townGarrisonHero and path.targetHero == townGarrisonHero then
			score = 1
		end

		if score < shortestScore then
			shortestPath = path
			shortestScore = score
		end
	end

	return shortestPath
end

local function getNearestHero(aiNk, town)
	local paths = getPathInfo(aiNk, visitablePos(town))
	if #paths == 0 then
		return nil
	end

	local shortestPath = getShortestPath(town, paths)
	if not shortestPath then
		return nil
	end

	local targetHero = shortestPath.targetHero
	if #((shortestPath and shortestPath.nodes) or {}) > 1
		or pathTurn(shortestPath) ~= 0
		or dist2dSQ(visitablePos(targetHero), visitablePos(town)) > 4
		or (garrisonHero(town) and targetHero == garrisonHero(town)) then
		return nil
	end

	return targetHero
end

local function needToRecruitHero(aiNk, startupTown)
	if not canRecruitHero(aiNk, startupTown) then
		return false
	end

	if not garrisonHero(startupTown) and not visitingHero(startupTown) then
		return true
	end

	local treasureSourcesCount = 0
	for _, object in ipairs(nearbyObjects(aiNk)) do
		if isTreasureSource(object) then
			treasureSourcesCount = treasureSourcesCount + 1
		end
	end

	local basicCount = #townsInfo(aiNk) + 2
	local mapSizeValue = mapSize(aiNk)
	local boost = math.min(
		math.floor((1 + (mapSizeValue.x or 0) / 50) ^ 2),
		math.floor(treasureSourcesCount / 2))

	return heroCount(aiNk) < basicCount + boost
end

function StartupBehavior:init()
	self.goalType = AbstractGoal.EGoals.STARTUP
end

function StartupBehavior:toString()
	return "Startup"
end

function StartupBehavior:equalsTyped(_other)
	return true
end

function StartupBehavior:decompose(aiNk)
	local tasks = {}
	local towns = townsInfo(aiNk)

	if #towns == 0 then
		return tasks
	end

	local startupTown = towns[1]
	if #towns > 1 then
		local bestScore = -math.huge
		for _, town in ipairs(towns) do
			local townGarrisonHero = garrisonHero(town)
			local closestHero = getNearestHero(aiNk, town)
			local score = townGarrisonHero and evaluateHero(aiNk, townGarrisonHero)
				or closestHero and evaluateHero(aiNk, closestHero)
				or 0

			if score > bestScore then
				startupTown = town
				bestScore = score
			end
		end
	end

	if not hasBuilt(startupTown, BuildingID.TAVERN)
		and canBuildStructure(aiNk, startupTown, BuildingID.TAVERN) then
		table.insert(tasks, BuildThis.new(BuildingID.TAVERN, startupTown):setpriority(100))
		return tasks
	end

	local canRecruitHeroValue = needToRecruitHero(aiNk, startupTown)
	local closestHero = getNearestHero(aiNk, startupTown)

	if closestHero then
		if not visitingHero(startupTown) then
			if howManyReinforcementsCanGet(aiNk, upperArmy(startupTown), upperArmy(startupTown), closestHero, "NONE") > 200 then
				local path = getShortestPath(startupTown, getPathInfo(aiNk, visitablePos(startupTown)))
				if path then
					table.insert(tasks, ExecuteHeroChain.new(path, startupTown):setpriority(100))
				end
			end
		else
			local startupVisitingHero = visitingHero(startupTown)
			local visitingHeroScore = evaluateHero(aiNk, startupVisitingHero)
			local startupGarrisonHero = garrisonHero(startupTown)

			if startupGarrisonHero then
				local garrisonHeroScore = evaluateHero(aiNk, startupGarrisonHero)
				local shouldSwapToVisitor = visitingHeroScore > garrisonHeroScore
					or (heroRole(aiNk, startupGarrisonHero) == PriorityEvaluator.HeroRole.SCOUT
						and heroRole(aiNk, startupVisitingHero) == PriorityEvaluator.HeroRole.MAIN)

				if shouldSwapToVisitor then
					if canRecruitHeroValue
						or howManyReinforcementsCanGet(aiNk, startupVisitingHero, startupGarrisonHero) > 200 then
						table.insert(tasks, ExchangeSwapTownHeroes.new(
							startupTown,
							startupVisitingHero,
							State.HeroLockedReason.STARTUP):setpriority(100))
					end
				elseif howManyReinforcementsCanGet(aiNk, startupGarrisonHero, startupVisitingHero) > 200 then
					table.insert(tasks, ExchangeSwapTownHeroes.new(
						startupTown,
						startupGarrisonHero,
						State.HeroLockedReason.STARTUP):setpriority(100))
				end
			elseif canRecruitHeroValue then
				local canPickTownArmy = stacksCount(startupTown) == 0
					or howManyReinforcementsCanGet(aiNk, startupVisitingHero, startupTown) > 0

				if canPickTownArmy then
					table.insert(tasks, ExchangeSwapTownHeroes.new(
						startupTown,
						startupVisitingHero,
						State.HeroLockedReason.STARTUP):setpriority(100))
				end
			end
		end
	end

	if #tasks == 0 and canRecruitHeroValue and not visitingHero(startupTown) then
		table.insert(tasks, RecruitHero.new(startupTown))
	end

	if #tasks == 0 and not visitingHero(startupTown) then
		for _, town in ipairs(towns) do
			if not visitingHero(town) and needToRecruitHero(aiNk, town) then
				table.insert(tasks, RecruitHero.new(town))
				break
			end
		end
	end

	if #tasks == 0 and #towns > 0 then
		for _, town in ipairs(towns) do
			local townGarrisonHero = garrisonHero(town)
			if townGarrisonHero
				and movementPointsRemaining(townGarrisonHero) ~= 0
				and not visitingHero(town)
				and heroLockedReason(aiNk, townGarrisonHero) ~= State.HeroLockedReason.DEFENCE then
				table.insert(tasks, ExchangeSwapTownHeroes.new(town, nil):setpriority(MIN_PRIORITY))
			end
		end
	end

	return tasks
end

StartupBehavior.BuildingID = BuildingID
StartupBehavior.EBuildingState = EBuildingState
StartupBehavior.Obj = Obj
StartupBehavior.MIN_PRIORITY = MIN_PRIORITY
StartupBehavior.getShortestPath = getShortestPath
StartupBehavior.getNearestHero = getNearestHero
StartupBehavior.needToRecruitHero = needToRecruitHero

return StartupBehavior
