-- Mirrors AI/Nullkiller2/Engine/PriorityEvaluator.{h,cpp}: priority tiers,
-- EvaluationContext defaults, and task scoring formulas.

local AbstractGoal = require("Goals.AbstractGoal")
local RewardEvaluator = require("Engine.RewardEvaluator")

local PriorityEvaluator = {}

PriorityEvaluator.HeroRole = {
	SCOUT = 0,
	MAIN = 1
}

PriorityEvaluator.PriorityTier = {
	BUILDINGS = 0,
	INSTAKILL = 1,
	INSTADEFEND = 2,
	KILL = 3,
	ESCAPE = 4,
	EXPLORE_AND_GATHER = 5,
	DEFEND = 6,
	MAX_PRIORITY_TIER = 6
}

PriorityEvaluator.MAX_CRITICAL_VALUE = 2.0

local function isAlmostZero(value)
	return math.abs(value or 0) < 0.000001
end

local function pow(base, exponent)
	return base ^ exponent
end

local function resourceValue(resources, resourceID)
	if not resources then
		return 0
	end
	if resources[0] == nil and resources[7] ~= nil then
		return resources[resourceID + 1] or 0
	end
	return resources[resourceID] or 0
end

local function marketValue(resources)
	if not resources then
		return 0
	end
	if type(resources.marketValue) == "function" then
		return resources:marketValue()
	end
	if type(resources.marketValue) == "number" then
		return resources.marketValue
	end

	local result = 0
	for resourceID = 0, 6 do
		local amount = resourceValue(resources, resourceID)
		result = result + (resourceID == 6 and amount or amount * 100)
	end
	return result
end

local function addResources(lhs, rhs)
	local result = {}
	for resourceID = 0, 6 do
		result[resourceID] = resourceValue(lhs, resourceID) + resourceValue(rhs, resourceID)
	end
	return result
end

local function subtractResources(lhs, rhs)
	local result = {}
	for resourceID = 0, 6 do
		local amount = resourceValue(lhs, resourceID) - resourceValue(rhs, resourceID)
		result[resourceID] = amount > 0 and amount or 0
	end
	return result
end

local function canAfford(resources, cost)
	for resourceID = 0, 6 do
		if resourceValue(resources, resourceID) < resourceValue(cost, resourceID) then
			return false
		end
	end
	return true
end

local function maxPurchasableCount(needed, income)
	local result = 0
	for resourceID = 0, 6 do
		local neededAmount = resourceValue(needed, resourceID)
		if neededAmount > 0 then
			local incomeAmount = resourceValue(income, resourceID)
			if incomeAmount == 0 then
				return math.huge
			end
			result = math.max(result, math.ceil(neededAmount / incomeAmount))
		end
	end
	return result
end

PriorityEvaluator.EvaluationContext = {}
PriorityEvaluator.EvaluationContext.__index = PriorityEvaluator.EvaluationContext

function PriorityEvaluator.EvaluationContext.new(_aiNk)
	return setmetatable({
		movementCost = 0.0,
		movementCostByRole = {},
		manaCost = 0,
		danger = 0,
		closestWayRatio = 1,
		armyLossRatio = 0,
		armyReward = 0,
		armyGrowth = 0,
		goldReward = 0,
		goldCost = 0,
		skillReward = 0,
		strategicalValue = 0,
		conquestValue = 0,
		heroRole = PriorityEvaluator.HeroRole.SCOUT,
		turn = 0,
		enemyHeroDangerRatio = 0,
		threat = 0,
		armyInvolvement = 0,
		defenseValue = 0,
		isDefend = false,
		threatTurns = math.huge,
		buildingCost = {},
		involvesSailing = false,
		isTradeBuilding = false,
		isExchange = false,
		isArmyUpgrade = false,
		isHero = false,
		isEnemy = false,
		explorePriority = 0,
		powerRatio = 0,
		evaluator = RewardEvaluator.new(_aiNk)
	}, PriorityEvaluator.EvaluationContext)
end

function PriorityEvaluator.EvaluationContext:addNonCriticalStrategicalValue(value)
	self.strategicalValue = math.max(self.strategicalValue, math.min(value, PriorityEvaluator.MAX_CRITICAL_VALUE))
end

local function applyContextSnapshot(context, snapshot)
	for key, value in pairs(snapshot or {}) do
		if key == "buildingCost" then
			context.buildingCost = addResources(context.buildingCost, value)
		elseif key == "movementCostByRole" then
			for role, cost in pairs(value) do
				context.movementCostByRole[role] = (context.movementCostByRole[role] or 0) + cost
			end
		else
			context[key] = value
		end
	end
end

function PriorityEvaluator.buildEvaluationContext(goal, aiNk)
	local parts = {}
	local context = PriorityEvaluator.EvaluationContext.new(aiNk)

	if goal.goalType == AbstractGoal.EGoals.COMPOSITION then
		parts = goal:decompose(aiNk)
	else
		parts = { goal }
	end

	for _, subgoal in ipairs(parts) do
		context.goldCost = context.goldCost + (subgoal.goldCost or 0)
		context.buildingCost = addResources(context.buildingCost, subgoal.buildingCost)
		applyContextSnapshot(context, subgoal.evaluationContext)
	end

	return context
end

function PriorityEvaluator.evaluateMovement(score, movementCost)
	if movementCost > 0 then
		if movementCost < 1 then
			score = score / pow(movementCost, 0.6)
		else
			score = score / (0.75 + pow(movementCost, 1.3))
		end
	end
	return score
end

function PriorityEvaluator.evaluateArmyLossRatio(score, armyLossRatio, heroRole)
	if armyLossRatio > 0 then
		score = score - score * armyLossRatio

		if heroRole ~= PriorityEvaluator.HeroRole.MAIN then
			score = score / 5.0
		end
	end
	return score
end

function PriorityEvaluator.evaluateSkillReward(score, skillReward, armyInvolvement, armyLossRatio)
	return score + skillReward * armyInvolvement * (1 - armyLossRatio) * 0.05
end

function PriorityEvaluator.evaluateConquestValue(score, conquestValue, armyInvolvement)
	if conquestValue > 0 then
		score = armyInvolvement * conquestValue
	end
	return score
end

local function getMaxArmyLossTarget(aiNk)
	if aiNk and aiNk.settings then
		if type(aiNk.settings.getMaxArmyLossTarget) == "function" then
			return aiNk.settings:getMaxArmyLossTarget()
		end
		if aiNk.settings.maxArmyLossTarget then
			return aiNk.settings.maxArmyLossTarget
		end
	end
	return 0.35
end

local function calendar(aiNk)
	local value = aiNk and aiNk.calendar or {}
	return {
		dayOfWeek = value.dayOfWeek or 1,
		daysInWeek = value.daysInWeek or 7
	}
end

local function getLockedResources(aiNk)
	if aiNk and type(aiNk.getLockedResources) == "function" then
		return aiNk:getLockedResources()
	end
	return aiNk and aiNk.lockedResources or {}
end

local function getFreeResources(aiNk)
	if aiNk and type(aiNk.getFreeResources) == "function" then
		return aiNk:getFreeResources()
	end
	return aiNk and aiNk.freeResources or {}
end

local function getDailyIncome(aiNk)
	if aiNk and aiNk.buildAnalyzer and type(aiNk.buildAnalyzer.getDailyIncome) == "function" then
		return aiNk.buildAnalyzer:getDailyIncome()
	end
	return aiNk and aiNk.dailyIncome or {}
end

local function isGoldPressureOverMax(aiNk)
	if aiNk and aiNk.buildAnalyzer and type(aiNk.buildAnalyzer.isGoldPressureOverMax) == "function" then
		return aiNk.buildAnalyzer:isGoldPressureOverMax()
	end
	return aiNk and aiNk.goldPressureOverMax or false
end

local function allTownsHaveMarketplace(aiNk)
	for _, town in ipairs(aiNk and aiNk.townsInfo or {}) do
		if town.hasBuiltResourceMarketplace ~= true then
			return false
		end
	end
	return true
end

function PriorityEvaluator.evaluate(task, priorityTier, aiNk)
	priorityTier = priorityTier or PriorityEvaluator.PriorityTier.BUILDINGS
	local evaluationContext = PriorityEvaluator.buildEvaluationContext(task, aiNk)

	local amIWithoutCastle = aiNk and aiNk.amIWithoutCastle or false
	local score = 0

	local maxWillingToLose = amIWithoutCastle and 1
		or ((getMaxArmyLossTarget(aiNk) * evaluationContext.powerRatio) > 0
			and getMaxArmyLossTarget(aiNk) * evaluationContext.powerRatio
			or 1.0)
	local maxEnemyDangerRatio = evaluationContext.powerRatio > 0 and evaluationContext.powerRatio or 1.0
	local cal = calendar(aiNk)
	local arriveNextWeek = cal.dayOfWeek + evaluationContext.turn > cal.daysInWeek

	if priorityTier == PriorityEvaluator.PriorityTier.INSTAKILL then
		if evaluationContext.turn > 0 or evaluationContext.isExchange or evaluationContext.isDefend then
			return 0
		end
		if evaluationContext.movementCost >= 1 then
			return 0
		end
		if maxWillingToLose - evaluationContext.armyLossRatio < 0 then
			return 0
		end

		score = PriorityEvaluator.evaluateConquestValue(score, evaluationContext.conquestValue, evaluationContext.armyInvolvement)
		score = PriorityEvaluator.evaluateArmyLossRatio(score, evaluationContext.armyLossRatio, evaluationContext.heroRole)
		if isAlmostZero(score) or (evaluationContext.enemyHeroDangerRatio > maxEnemyDangerRatio and not amIWithoutCastle) then
			return 0
		end

		score = score * evaluationContext.closestWayRatio
		score = PriorityEvaluator.evaluateMovement(score, evaluationContext.movementCost)
	elseif priorityTier == PriorityEvaluator.PriorityTier.INSTADEFEND then
		if not evaluationContext.isDefend then
			return 0
		end
		if maxWillingToLose - evaluationContext.armyLossRatio < 0 then
			return 0
		end
		if evaluationContext.isEnemy and evaluationContext.turn > 0 then
			return 0
		end

		local canPrepareForNextTurnThreat = evaluationContext.turn == 0 and evaluationContext.threatTurns == 1
		if evaluationContext.threatTurns <= evaluationContext.turn or canPrepareForNextTurnThreat then
			local optimalStrength = evaluationContext.threat * 0.75
			local deviation = math.abs(evaluationContext.armyInvolvement - optimalStrength)
			local deviationPercentage = deviation / evaluationContext.threat
			score = 1.0 / (1.0 + deviationPercentage)
			score = score * evaluationContext.closestWayRatio
			score = PriorityEvaluator.evaluateMovement(score, evaluationContext.movementCost)
		end
	elseif priorityTier == PriorityEvaluator.PriorityTier.KILL then
		if evaluationContext.isDefend then
			return 0
		end
		if evaluationContext.turn > 0 and evaluationContext.isHero then
			return 0
		end
		if arriveNextWeek and evaluationContext.isEnemy then
			return 0
		end

		score = PriorityEvaluator.evaluateConquestValue(score, evaluationContext.conquestValue, evaluationContext.armyInvolvement)
		if isAlmostZero(score)
			or (evaluationContext.enemyHeroDangerRatio > maxEnemyDangerRatio
				and (evaluationContext.turn > 0 or evaluationContext.isExchange)
				and not amIWithoutCastle) then
			return 0
		end
		if maxWillingToLose - evaluationContext.armyLossRatio < 0 then
			return 0
		end

		score = PriorityEvaluator.evaluateArmyLossRatio(score, evaluationContext.armyLossRatio, evaluationContext.heroRole)
		score = score * evaluationContext.closestWayRatio
		score = PriorityEvaluator.evaluateMovement(score, evaluationContext.movementCost)
	elseif priorityTier == PriorityEvaluator.PriorityTier.EXPLORE_AND_GATHER
		or priorityTier == PriorityEvaluator.PriorityTier.ESCAPE then
		if evaluationContext.conquestValue > 0 then
			return 0
		end
		if evaluationContext.isDefend then
			return 0
		end
		if marketValue(evaluationContext.buildingCost) > 0 then
			return 0
		end
		if maxWillingToLose - evaluationContext.armyLossRatio < 0 then
			return 0
		end
		if priorityTier == PriorityEvaluator.PriorityTier.EXPLORE_AND_GATHER
			and evaluationContext.enemyHeroDangerRatio > maxEnemyDangerRatio then
			return 0
		end
		if priorityTier == PriorityEvaluator.PriorityTier.ESCAPE and evaluationContext.escapeThreatDelta and evaluationContext.escapeThreatDelta > 0 then
			score = score + evaluationContext.escapeThreatDelta
		end

		local requiresBattle = evaluationContext.armyLossRatio > 0 or evaluationContext.danger > 0
		score = score + evaluationContext.strategicalValue * 1000
		if evaluationContext.explorePriority > 0 then
			score = 600.0 / evaluationContext.explorePriority
			if evaluationContext.heroRole == PriorityEvaluator.HeroRole.MAIN and requiresBattle then
				score = score * 2
			end
		end

		if evaluationContext.goldReward > 0 then
			score = score + (evaluationContext.goldReward > 500 and evaluationContext.goldReward / 2.0 or evaluationContext.goldReward * 2.0)

			if evaluationContext.heroRole == PriorityEvaluator.HeroRole.MAIN then
				if requiresBattle then
					score = score * 2
				else
					score = score * 0.33
				end
			end
		end

		if evaluationContext.skillReward > 0 then
			if evaluationContext.heroRole == PriorityEvaluator.HeroRole.MAIN then
				score = 1000 + PriorityEvaluator.evaluateSkillReward(
					score,
					evaluationContext.skillReward,
					evaluationContext.armyInvolvement,
					evaluationContext.armyLossRatio)

				if not requiresBattle then
					score = score * 3
				end
			else
				score = math.max(1.0, score / 1000.0)
			end
		end

		score = score + (evaluationContext.heroRole == PriorityEvaluator.HeroRole.MAIN and evaluationContext.armyReward or evaluationContext.armyReward / 10.0)
		score = score + evaluationContext.armyGrowth

		if evaluationContext.goldCost > 0 then
			score = score - evaluationContext.goldCost / 4.0
		end
		score = PriorityEvaluator.evaluateArmyLossRatio(score, evaluationContext.armyLossRatio, evaluationContext.heroRole)

		score = score * evaluationContext.closestWayRatio
		score = PriorityEvaluator.evaluateMovement(score, evaluationContext.movementCost)
	elseif priorityTier == PriorityEvaluator.PriorityTier.DEFEND then
		if evaluationContext.enemyHeroDangerRatio > maxEnemyDangerRatio then
			return 0
		end
		if evaluationContext.isDefend or evaluationContext.isArmyUpgrade then
			score = evaluationContext.armyInvolvement
		end

		score = score * evaluationContext.closestWayRatio
		score = PriorityEvaluator.evaluateMovement(score, evaluationContext.movementCost)
	elseif priorityTier == PriorityEvaluator.PriorityTier.BUILDINGS then
		if maxWillingToLose - evaluationContext.armyLossRatio < 0 then
			return 0
		end
		if marketValue(getLockedResources(aiNk)) > 0 then
			return 0
		end

		score = score + evaluationContext.conquestValue * 1000
		score = score + evaluationContext.strategicalValue * 1000
		score = score + evaluationContext.goldReward
		score = PriorityEvaluator.evaluateSkillReward(
			score,
			evaluationContext.skillReward,
			evaluationContext.armyInvolvement,
			evaluationContext.armyLossRatio)
		score = score + evaluationContext.armyReward
		score = score + evaluationContext.armyGrowth

		if marketValue(evaluationContext.buildingCost) > 0 then
			if not evaluationContext.isTradeBuilding
				and resourceValue(getFreeResources(aiNk), 0) - resourceValue(evaluationContext.buildingCost, 0) < 5
				and resourceValue(getDailyIncome(aiNk), 0) < 1
				and not allTownsHaveMarketplace(aiNk) then
				return 0
			end

			score = score + 1000
			local resourcesAvailable = getFreeResources(aiNk)
			local income = getDailyIncome(aiNk)

			if isGoldPressureOverMax(aiNk) then
				score = score / marketValue(evaluationContext.buildingCost)
			end

			if not canAfford(resourcesAvailable, evaluationContext.buildingCost) then
				local needed = subtractResources(evaluationContext.buildingCost, resourcesAvailable)
				local turnsTo = maxPurchasableCount(needed, income)
				local haveEverythingButGold = true
				for resourceID = 0, 5 do
					if resourceValue(resourcesAvailable, resourceID) < resourceValue(evaluationContext.buildingCost, resourceID) then
						haveEverythingButGold = false
					end
				end

				if turnsTo == math.huge then
					return 0
				end
				if not haveEverythingButGold then
					score = score / turnsTo
				end
			end
		elseif evaluationContext.enemyHeroDangerRatio > 1
			and not evaluationContext.isDefend
			and isAlmostZero(evaluationContext.conquestValue) then
			return 0
		end
	else
		error("PriorityEvaluator.evaluate unsupported priority: " .. tostring(priorityTier), 2)
	end

	if score ~= score then
		return 0
	end
	return score
end

return PriorityEvaluator
