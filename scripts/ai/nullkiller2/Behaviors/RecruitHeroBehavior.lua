-- Mirrors AI/Nullkiller2/Behaviors/RecruitHeroBehavior.{h,cpp}: RecruitHeroBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local RecruitHero = require("Goals.RecruitHero")
local State = require("Engine.State")

local RecruitHeroBehavior = CGoal.derive("RecruitHeroBehavior", AbstractGoal.EGoals.RECRUIT_HERO_BEHAVIOR)

RecruitHeroBehavior.HERO_GOLD_COST = 2500
RecruitHeroBehavior.DEFENSIVE_EMERGENCY_RECRUIT_PRIORITY = 1000000.0

local TREASURE_OBJECTS = {
	RESOURCE = true,
	TREASURE_CHEST = true,
	CAMPFIRE = true,
	ARTIFACT = true
}

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function armyCost(hero)
	if not hero then
		return 0
	end
	return call(hero, "getArmyCost") or hero.armyCost or 0
end

local function totalStrength(hero)
	if not hero then
		return 0
	end
	return call(hero, "getTotalStrength") or hero.totalStrength or hero.strength or 0
end

local function upperArmyStrength(town)
	if not town then
		return 0
	end
	if town.upperArmyStrength then
		return town.upperArmyStrength
	end
	local upperArmy = call(town, "getUpperArmy") or town.upperArmy
	return call(upperArmy, "getArmyStrength") or upperArmy and upperArmy.armyStrength or 0
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town.visitingHero
end

local function garrisonHero(town)
	return call(town, "getGarrisonHero") or town.garrisonHero
end

local function factionID(value)
	return call(value, "getFactionID") or value.factionID
end

local function townLevel(town)
	return call(town, "getTownLevel") or town.townLevel or 0
end

local function hasCapitol(town)
	return call(town, "hasCapitol") or town.hasCapitol or false
end

local function heroesInfo(aiNk)
	return call(aiNk and aiNk.cc, "getHeroesInfo") or aiNk and aiNk.heroesInfo or {}
end

local function townsInfo(aiNk)
	return call(aiNk and aiNk.cc, "getTownsInfo") or aiNk and aiNk.townsInfo or {}
end

local function availableHeroes(aiNk, town)
	return call(aiNk and aiNk.cc, "getAvailableHeroes", town) or town.availableHeroes or {}
end

local function townThreats(aiNk, town)
	return call(aiNk and aiNk.dangerHitMap, "getTownThreats", town) or town.threats or {}
end

local function closestTown(aiNk, tile)
	return call(aiNk and aiNk.dangerHitMap, "getClosestTown", tile)
end

local function evaluateHero(heroManager, hero)
	return call(heroManager, "evaluateHero", hero) or hero.evaluateHeroScore or 0
end

local function canRecruitHero(aiNk, town)
	local value = call(aiNk and aiNk.heroManager, "canRecruitHero", town)
	if value ~= nil then
		return value
	end
	return town.canRecruitHero ~= false
end

local function freeGold(aiNk)
	if aiNk and type(aiNk.getFreeResources) == "function" then
		local resources = aiNk:getFreeResources()
		if resources[0] == nil and resources[7] ~= nil then
			return resources[7] or 0
		end
		return resources[6] or 0
	end
	local freeResources = aiNk and aiNk.freeResources or {}
	if freeResources[0] == nil and freeResources[7] ~= nil then
		return freeResources[7] or 0
	end
	return freeResources[6] or aiNk and aiNk.freeGold or 0
end

local function isGoldPressureOverMax(aiNk)
	return call(aiNk and aiNk.buildAnalyzer, "isGoldPressureOverMax") or aiNk and aiNk.goldPressureOverMax or false
end

function RecruitHeroBehavior.RecruitHeroChoice()
	return {
		score = 0,
		hero = nil,
		town = nil,
		defensiveEmergency = false
	}
end

function RecruitHeroBehavior.isDefensiveRecruitEmergency(town, hero, threat, safeAttackRatio)
	if threat.danger == 0 or threat.turn > 1 then
		return false
	end

	local currentDefence = upperArmyStrength(town)
	local recruitedDefence = totalStrength(hero)

	if threat.turn == 0 then
		return recruitedDefence >= threat.danger
	end

	local requiredDefence = threat.danger * safeAttackRatio
	if currentDefence >= requiredDefence then
		return false
	end

	if recruitedDefence < requiredDefence then
		return false
	end

	return threat.danger >= RecruitHeroBehavior.HERO_GOLD_COST / 2
end

function RecruitHeroBehavior.shouldRecruitHero(heroesCount, bestChoice, haveCapitol, treasureSourcesCount, freeGoldAmount, goldPressureOverMax)
	if bestChoice.hero == nil or math.abs(bestChoice.score) < 0.000001 then
		return false
	end

	return heroesCount == 0
		or treasureSourcesCount > heroesCount * 5
		or (armyCost(bestChoice.hero) > RecruitHeroBehavior.HERO_GOLD_COST / 2.0
			and (bestChoice.defensiveEmergency or not goldPressureOverMax))
		or (freeGoldAmount > 10000 and not goldPressureOverMax and haveCapitol)
		or (freeGoldAmount > 30000 and not goldPressureOverMax)
end

function RecruitHeroBehavior.calculateTreasureSources(nearbyObjects, _playerID, dangerHitMap, treasureSourcesCount, town)
	local count = treasureSourcesCount or 0

	for _, obj in ipairs(nearbyObjects or {}) do
		local id = obj.ID or obj.idType or obj.type
		if TREASURE_OBJECTS[id] or obj.weeklyRevisitable then
			local objTown = call(dangerHitMap, "getClosestTown", obj.visitablePos or obj.tile) or obj.closestTown
			if objTown == town then
				count = count + 1
			end
		end
	end

	return count
end

function RecruitHeroBehavior.calculateBestHero(availableHeroesList, heroManager, bestChoice, town, threats, safeAttackRatio, visitabilityRatio)
	local closestThreatTurn = math.huge
	for _, threat in ipairs(threats or {}) do
		closestThreatTurn = math.min(closestThreatTurn, threat.turn)
	end

	for _, hero in ipairs(availableHeroesList or {}) do
		local skipHero = (visitingHero(town) or garrisonHero(town))
			and closestThreatTurn < 1
			and armyCost(hero) < RecruitHeroBehavior.HERO_GOLD_COST / 3.0

		if not skipHero then
			local heroScore = evaluateHero(heroManager, hero)
			local totalScore = heroScore + armyCost(hero)

			if factionID(hero) ~= nil and factionID(hero) == factionID(town) then
				totalScore = totalScore + heroScore * 1.5
			end

			totalScore = totalScore + heroScore * townLevel(town) * (1 - visitabilityRatio)

			local defensiveEmergency = false
			for _, threat in ipairs(threats or {}) do
				if RecruitHeroBehavior.isDefensiveRecruitEmergency(town, hero, threat, safeAttackRatio) then
					defensiveEmergency = true
					break
				end
			end

			if (defensiveEmergency and not bestChoice.defensiveEmergency)
				or (defensiveEmergency == bestChoice.defensiveEmergency and totalScore > bestChoice.score) then
				bestChoice.score = totalScore
				bestChoice.hero = hero
				bestChoice.town = town
				bestChoice.defensiveEmergency = defensiveEmergency
			end
		end
	end
end

function RecruitHeroBehavior.calculateFinalDecision(aiNk, tasks, ourHeroes, bestChoice, haveCapitol, treasureSourcesCount)
	if bestChoice.hero ~= nil and math.abs(bestChoice.score) >= 0.000001 then
		if RecruitHeroBehavior.shouldRecruitHero(
			#ourHeroes,
			bestChoice,
			haveCapitol,
			treasureSourcesCount,
			freeGold(aiNk),
			isGoldPressureOverMax(aiNk)) then
			if bestChoice.defensiveEmergency then
				local composition = Composition.new()
				composition:addNextSequence({
					RecruitHero.new(bestChoice.town, bestChoice.hero),
					ExchangeSwapTownHeroes.new(bestChoice.town, bestChoice.hero, State.HeroLockedReason.DEFENCE)
				})
				composition:setpriority(RecruitHeroBehavior.DEFENSIVE_EMERGENCY_RECRUIT_PRIORITY)
				table.insert(tasks, composition)
			else
				table.insert(tasks, RecruitHero.new(bestChoice.town, bestChoice.hero):setpriority(3.0 / (#ourHeroes + 1)))
			end
		end
	end
end

function RecruitHeroBehavior:init()
	self.goalType = AbstractGoal.EGoals.RECRUIT_HERO_BEHAVIOR
end

function RecruitHeroBehavior:toString()
	return "Recruit hero"
end

function RecruitHeroBehavior:equalsTyped(_other)
	return true
end

function RecruitHeroBehavior:decompose(aiNk)
	local tasks = {}
	local ourTowns = townsInfo(aiNk)
	local ourHeroes = heroesInfo(aiNk)
	local bestChoice = RecruitHeroBehavior.RecruitHeroChoice()
	local haveCapitolValue = false
	local treasureSourcesCount = 0

	for _, town in ipairs(ourTowns) do
		if not (visitingHero(town) and garrisonHero(town)) then
			local threats = townThreats(aiNk, town)
			local visitabilityRatio = 0
			for _, hero in ipairs(ourHeroes) do
				if closestTown(aiNk, hero.visitablePos or hero.tile) == town then
					visitabilityRatio = visitabilityRatio + 1.0 / #ourHeroes
				end
			end

			if canRecruitHero(aiNk, town) then
				treasureSourcesCount = RecruitHeroBehavior.calculateTreasureSources(
					call(aiNk and aiNk.objectClusterizer, "getNearbyObjects") or aiNk and aiNk.nearbyObjects or {},
					aiNk and aiNk.playerID,
					aiNk and aiNk.dangerHitMap,
					treasureSourcesCount,
					town)
				RecruitHeroBehavior.calculateBestHero(
					availableHeroes(aiNk, town),
					aiNk and aiNk.heroManager,
					bestChoice,
					town,
					threats,
					aiNk and aiNk.settings and aiNk.settings.safeAttackRatio or 1.1,
					visitabilityRatio)
			end

			if hasCapitol(town) then
				haveCapitolValue = true
			end
		end
	end

	RecruitHeroBehavior.calculateFinalDecision(aiNk or {}, tasks, ourHeroes, bestChoice, haveCapitolValue, treasureSourcesCount)
	return tasks
end

return RecruitHeroBehavior
