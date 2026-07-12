local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local PriorityEvaluator = require("Engine.PriorityEvaluator")

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
