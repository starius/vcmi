local AbstractGoal = require("Goals.AbstractGoal")
local ArmyUpgrade = require("Markers.ArmyUpgrade")
local CGoal = require("Goals.CGoal")
local DismissHero = require("Goals.DismissHero")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local ExplorationPoint = require("Markers.ExplorationPoint")
local HeroExchange = require("Markers.HeroExchange")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local State = require("Engine.State")
local StayAtTown = require("Goals.StayAtTown")

local EvalGoal = CGoal.derive("EvalGoal", AbstractGoal.EGoals.BUY_ARMY, { elementar = true })

function EvalGoal:init(context)
	self.priority = 0
	self.evaluationContext = context
end

function EvalGoal:equalsTyped(other)
	return self == other
end

function EvalGoal:toString()
	return "EvalGoal"
end

local function almostEquals(lhs, rhs)
	return math.abs(lhs - rhs) < 0.0001
end

local function pow(base, exponent)
	return base ^ exponent
end

assert(almostEquals(PriorityEvaluator.evaluateMovement(100, 0.25), 100 / pow(0.25, 0.6)))
assert(almostEquals(PriorityEvaluator.evaluateMovement(100, 2), 100 / (0.75 + pow(2, 1.3))))
assert(almostEquals(PriorityEvaluator.evaluateArmyLossRatio(100, 0.2, PriorityEvaluator.HeroRole.MAIN), 80))
assert(almostEquals(PriorityEvaluator.evaluateArmyLossRatio(100, 0.2, PriorityEvaluator.HeroRole.SCOUT), 16))
assert(almostEquals(PriorityEvaluator.evaluateSkillReward(10, 2, 100, 0.25), 17.5))
assert(PriorityEvaluator.evaluateConquestValue(10, 1.5, 100) == 150)

local aiNk = {
	settings = {
		maxArmyLossTarget = 0.35
	},
	freeResources = { [0] = 20, [6] = 5000 },
	dailyIncome = { [0] = 1, [6] = 1000 },
	lockedResources = {},
	townsInfo = {
		{ hasBuiltResourceMarketplace = true }
	},
	calendar = {
		dayOfWeek = 2,
		daysInWeek = 7
	}
}

local evalContext = PriorityEvaluator.EvaluationContext.new(aiNk)
assert(evalContext.evaluator:getNowResourceRequirementStrength(6) == 0)

local exchangeReceiver = { id = 101, armyStrength = 1000 }
local exchangeGiver = { id = 102, role = PriorityEvaluator.HeroRole.SCOUT }
local exchangeContext = PriorityEvaluator.buildEvaluationContext(HeroExchange.new(exchangeReceiver, {
	targetHero = exchangeGiver,
	movementCost = 0.4,
	totalDanger = 200,
	reinforcementArmyStrength = 300
}), aiNk)
assert(almostEquals(exchangeContext.strategicalValue, 0.3))
assert(exchangeContext.armyGrowth == 300)
assert(exchangeContext.movementCost == 0.4)
assert(exchangeContext.danger == 200)
assert(exchangeContext.heroRole == PriorityEvaluator.HeroRole.SCOUT)
assert(exchangeContext.isExchange == true)

local upgradeContext = PriorityEvaluator.buildEvaluationContext(ArmyUpgrade.new(
	{ id = 201, armyStrength = 1000 },
	{ id = 202, name = "Hill Fort" },
	{ upgradeValue = 1000 }), aiNk)
assert(upgradeContext.armyGrowth == 200)
assert(upgradeContext.isArmyUpgrade == true)

local explorationContext = PriorityEvaluator.buildEvaluationContext(ExplorationPoint.new({ x = 1, y = 2, z = 0 }, 25), aiNk)
assert(almostEquals(explorationContext.strategicalValue, 0.75))
assert(explorationContext.explorePriority == 1)

local restHero = {
	id = 301,
	role = PriorityEvaluator.HeroRole.MAIN,
	movementPointsRemaining = 500,
	movementPointsLimit = 1000,
	mana = 25,
	manaLimit = 100,
	magicStrength = 2
}
local stayContext = PriorityEvaluator.buildEvaluationContext(StayAtTown.new({
	id = 302,
	mageGuildLevel = 1
}, {
	targetHero = restHero,
	movementCost = 0.1
}), aiNk)
assert(almostEquals(stayContext.armyReward, 10000))
assert(almostEquals(stayContext.movementCost, 0.4))
assert(almostEquals(stayContext.movementCostByRole[PriorityEvaluator.HeroRole.MAIN], 0.4))

local emptyStayContext = PriorityEvaluator.buildEvaluationContext(StayAtTown.new({
	id = 303,
	mageGuildLevel = 0
}, {
	targetHero = restHero,
	movementCost = 0.1
}), aiNk)
assert(emptyStayContext.isDefend == true)

local garrisonHero = {
	id = 401,
	role = PriorityEvaluator.HeroRole.MAIN,
	movementPointsRemaining = 300,
	movementPointsLimit = 1000,
	armyStrength = 4000
}
local swapContext = PriorityEvaluator.buildEvaluationContext(ExchangeSwapTownHeroes.new(
	{ id = 402, name = "Castle" },
	garrisonHero,
	State.HeroLockedReason.DEFENCE), aiNk)
assert(almostEquals(swapContext.movementCost, 0.3))
assert(almostEquals(swapContext.movementCostByRole[PriorityEvaluator.HeroRole.MAIN], 0.3))
assert(swapContext.heroRole == PriorityEvaluator.HeroRole.MAIN)
assert(swapContext.isDefend == true)
assert(swapContext.armyInvolvement == 4000)

local dismissedHero = {
	id = 501,
	role = PriorityEvaluator.HeroRole.SCOUT,
	movementPointsRemaining = 700,
	slots = {
		{ count = 2, marketValue = 100 }
	}
}
local dismissContext = PriorityEvaluator.buildEvaluationContext(DismissHero.new(dismissedHero), aiNk)
assert(dismissContext.movementCost == 700)
assert(dismissContext.movementCostByRole[PriorityEvaluator.HeroRole.SCOUT] == 700)
assert(dismissContext.goldCost == 2700)

local buildingScore = PriorityEvaluator.evaluate(EvalGoal.new({
	strategicalValue = 1,
	armyReward = 100,
	buildingCost = { [6] = 1000 },
	powerRatio = 1
}), PriorityEvaluator.PriorityTier.BUILDINGS, aiNk)
assert(buildingScore == 2100)

local blockedBuilding = PriorityEvaluator.evaluate(EvalGoal.new({
	strategicalValue = 1,
	buildingCost = { [0] = 10 },
	powerRatio = 1
}), PriorityEvaluator.PriorityTier.BUILDINGS, {
	settings = { maxArmyLossTarget = 0.35 },
	freeResources = { [0] = 12 },
	dailyIncome = { [0] = 0 },
	lockedResources = {},
	townsInfo = {
		{ hasBuiltResourceMarketplace = false }
	}
})
assert(blockedBuilding == 0)

local killScore = PriorityEvaluator.evaluate(EvalGoal.new({
	conquestValue = 1.2,
	armyInvolvement = 10000,
	armyLossRatio = 0.1,
	heroRole = PriorityEvaluator.HeroRole.MAIN,
	closestWayRatio = 0.5,
	movementCost = 0.25,
	powerRatio = 1
}), PriorityEvaluator.PriorityTier.KILL, aiNk)
assert(almostEquals(killScore, PriorityEvaluator.evaluateMovement(10000 * 1.2 * 0.9 * 0.5, 0.25)))

local exploreScore = PriorityEvaluator.evaluate(EvalGoal.new({
	strategicalValue = 1,
	goldReward = 600,
	heroRole = PriorityEvaluator.HeroRole.MAIN,
	closestWayRatio = 1,
	powerRatio = 1
}), PriorityEvaluator.PriorityTier.EXPLORE_AND_GATHER, aiNk)
assert(almostEquals(exploreScore, 429))

local defendScore = PriorityEvaluator.evaluate(EvalGoal.new({
	isDefend = true,
	armyInvolvement = 7500,
	closestWayRatio = 1,
	powerRatio = 1
}), PriorityEvaluator.PriorityTier.DEFEND, aiNk)
assert(defendScore == 7500)
