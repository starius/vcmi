-- Mirrors AI/Nullkiller2/Behaviors/BuyArmyBehavior.{h,cpp}: BuyArmyBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local ArmyManager = require("Analyzers.ArmyManager")
local BuyArmy = require("Goals.BuyArmy")
local CGoal = require("Goals.CGoal")
local PriorityEvaluator = require("Engine.PriorityEvaluator")

local BuyArmyBehavior = CGoal.derive("BuyArmyBehavior", AbstractGoal.EGoals.INVALID)

local CITY_HALL = 12

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function heroesInfo(aiNk)
	return call(aiNk and aiNk.cc, "getHeroesInfo") or aiNk and aiNk.heroesInfo or {}
end

local function townsInfo(aiNk)
	return call(aiNk and aiNk.cc, "getTownsInfo") or aiNk and aiNk.townsInfo or {}
end

local function closestThreatTurn(aiNk, town)
	if town.closestThreatTurn ~= nil then
		return town.closestThreatTurn
	end

	local dangerHitMap = aiNk and aiNk.dangerHitMap
	local tileThreat = dangerHitMap and type(dangerHitMap.getTileThreat) == "function"
		and dangerHitMap:getTileThreat(town.visitablePos or town.tile)
		or nil
	return tileThreat and tileThreat.fastestDanger and tileThreat.fastestDanger.turn or math.huge
end

local function isGoldPressureOverMax(aiNk)
	return call(aiNk and aiNk.buildAnalyzer, "isGoldPressureOverMax") or aiNk and aiNk.goldPressureOverMax or false
end

local function hasBuiltCityHall(town)
	if type(town.hasBuilt) == "function" then
		return town:hasBuilt(CITY_HALL)
	end
	return town.hasCityHall or false
end

local function canBuildCityHall(aiNk, town)
	if aiNk and aiNk.cc and type(aiNk.cc.canBuildStructure) == "function" then
		return aiNk.cc:canBuildStructure(town, CITY_HALL) ~= "FORBIDDEN"
	end
	return town.canBuildCityHall ~= false
end

local function heroRole(aiNk, hero)
	local heroManager = aiNk and aiNk.heroManager
	return call(heroManager, "getHeroRoleOrDefaultInefficient", hero)
		or hero.role
		or PriorityEvaluator.HeroRole.SCOUT
end

local function reinforcementAvailable(aiNk, town, hero)
	if town.reinforcementsByHero then
		return town.reinforcementsByHero[CGoal.objectID(hero)] or 0
	end

	local armyManager = aiNk and aiNk.armyManager
	if armyManager and type(armyManager.howManyReinforcementsCanGet) == "function" then
		return armyManager:howManyReinforcementsCanGet(hero, hero, town.townArmyAvailableToBuy, town.terrainId)
	end

	local sourceArmy = town.townArmyAvailableToBuy
		or ArmyManager.getArmyAvailableToBuyAsCCreatureSet(town, aiNk and aiNk.freeResources)
	return ArmyManager.howManyReinforcementsCanGet(hero, hero, sourceArmy, town.terrainId, {
		settings = aiNk and aiNk.settings,
		moraleEvaluator = town.moraleEvaluator,
		moraleByCreatureID = town.moraleByCreatureID
	})
end

local function reinforcementCanBuy(aiNk, town)
	if town.reinforcementsCanBuy ~= nil then
		return town.reinforcementsCanBuy
	end

	local armyManager = aiNk and aiNk.armyManager
	if armyManager and type(armyManager.howManyReinforcementsCanBuy) == "function" then
		return armyManager:howManyReinforcementsCanBuy(town.upperArmy, town)
	end

	return ArmyManager.howManyReinforcementsCanBuy(town.upperArmy or town, town, aiNk and aiNk.freeResources, 0, {
		settings = aiNk and aiNk.settings,
		calendar = aiNk and aiNk.calendar
	})
end

function BuyArmyBehavior:toString()
	return "Buy army"
end

function BuyArmyBehavior:equalsTyped(_other)
	return true
end

function BuyArmyBehavior:decompose(aiNk)
	local tasks = {}
	local heroes = heroesInfo(aiNk)
	if #heroes == 0 then
		return tasks
	end

	for _, town in ipairs(townsInfo(aiNk)) do
		if closestThreatTurn(aiNk, town) >= 2
			and isGoldPressureOverMax(aiNk)
			and not hasBuiltCityHall(town)
			and canBuildCityHall(aiNk, town) then
			return tasks
		end

		for _, targetHero in ipairs(heroes) do
			if heroRole(aiNk, targetHero) == PriorityEvaluator.HeroRole.MAIN then
				local reinforcement = reinforcementAvailable(aiNk, town, targetHero)

				if reinforcement and reinforcement > 0 then
					reinforcement = math.min(reinforcement, reinforcementCanBuy(aiNk, town))
				end

				if reinforcement and reinforcement > 0 then
					table.insert(tasks, BuyArmy.new(town, reinforcement):setpriority(reinforcement))
				end
			end
		end
	end

	return tasks
end

return BuyArmyBehavior
