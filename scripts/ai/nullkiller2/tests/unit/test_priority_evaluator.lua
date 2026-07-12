local AbstractGoal = require("Goals.AbstractGoal")
local AdventureSpellCast = require("Goals.AdventureSpellCast")
local ArmyUpgrade = require("Markers.ArmyUpgrade")
local BuildThis = require("Goals.BuildThis")
local CGoal = require("Goals.CGoal")
local DismissHero = require("Goals.DismissHero")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local ExecuteHeroChain = require("Goals.ExecuteHeroChain")
local ExplorationPoint = require("Markers.ExplorationPoint")
local DefendTown = require("Markers.DefendTown")
local HeroExchange = require("Markers.HeroExchange")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local State = require("Engine.State")
local UnlockCluster = require("Markers.UnlockCluster")
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

local spellContext = PriorityEvaluator.buildEvaluationContext(AdventureSpellCast.new({
	id = 601,
	role = PriorityEvaluator.HeroRole.MAIN,
	movementPointsRemaining = 600,
	movementPointsLimit = 1200
}, {
	name = "Dimension Door",
	dimensionDoor = true,
	movementPointsTaken = 300
}), aiNk)
assert(spellContext.heroRole == PriorityEvaluator.HeroRole.MAIN)
assert(almostEquals(spellContext.movementCost, 0.25))
assert(almostEquals(spellContext.movementCostByRole[PriorityEvaluator.HeroRole.MAIN], 0.25))

local buildTown = { id = 701, factionID = 3, townLevel = 3, creatures = { {}, {} } }
local buildContext = PriorityEvaluator.buildEvaluationContext(BuildThis.new({
	id = 20,
	name = "Creature dwelling",
	prerequisitesCount = 2,
	dailyIncome = { [6] = 100 },
	buildCost = { [6] = 500 },
	buildCostWithPrerequisites = { [0] = 2, [6] = 500 },
	creatureID = 12,
	baseCreatureID = 12,
	creatureLevel = 2,
	armyStrength = 1000
}, {
	town = buildTown,
	armyStrength = 10000
}), {
	goldPressure = 1,
	townsInfo = {
		buildTown
	}
})
assert(buildContext.goldReward == 350)
assert(buildContext.goldCost == 500)
assert(buildContext.buildingCost[0] == 2)
assert(buildContext.buildingCost[6] == 500)
assert(almostEquals(buildContext.strategicalValue, 0.35))
assert(buildContext.armyReward == 4500)
assert(buildContext.movementCostByRole[PriorityEvaluator.HeroRole.MAIN] == 2)

local missingBuildContext = PriorityEvaluator.buildEvaluationContext(BuildThis.new({
	id = 14,
	name = "Marketplace",
	prerequisitesCount = 1,
	dailyIncome = { [0] = 1, [6] = 100 },
	buildCost = { [6] = 500 },
	buildCostWithPrerequisites = { [6] = 500 },
	isMissingResources = true
}, {
	town = buildTown,
	sameTownBonus = 1
}), {
	goldPressure = 1
})
assert(missingBuildContext.isTradeBuilding == true)
assert(missingBuildContext.goldReward == 700)
assert(missingBuildContext.goldCost == 500)
assert(almostEquals(missingBuildContext.strategicalValue, 0.2 / 3))
assert(missingBuildContext.movementCostByRole[PriorityEvaluator.HeroRole.MAIN] == 6)
assert(missingBuildContext.turn == 5)

local defendTown = {
	id = 801,
	owner = 1,
	ID = "TOWN",
	fortLevel = 3,
	hasFort = true,
	dailyIncome = { [6] = 200 },
	visitablePos = {
		x = 4,
		y = 5,
		z = 0,
		enemyHeroDanger = {
			maximumDanger = { danger = 500, threat = 800, turn = 1 }
		}
	},
	creatures = {
		{ 1, { { aiValue = 100, growth = 4 } } }
	}
}
local defendContext = PriorityEvaluator.buildEvaluationContext(DefendTown.new(
	defendTown,
	{ danger = 300, turn = 1 },
	{
		targetHero = { id = 802, movementPointsLimit = 1000 },
		turn = 2,
		heroStrength = 1000
	},
	false), {
	playerID = 1,
	developmentInfos = { { town = defendTown } }
})
assert(almostEquals(defendContext.armyGrowth, 400 / (2 * 1.2)))
assert(almostEquals(defendContext.goldReward, 1000 / (2 * 1.2)))
assert(defendContext.strategicalValue > 0)
assert(defendContext.defenseValue == 3)
assert(defendContext.isDefend == true)
assert(defendContext.threatTurns == 1)
assert(defendContext.danger == 300)
assert(defendContext.threat == 800)
assert(almostEquals(defendContext.enemyHeroDangerRatio, 0.5))

local clusterHero = { id = 901, owner = 1, role = PriorityEvaluator.HeroRole.MAIN }
local clusterGoal = UnlockCluster.new({
	blocker = { id = 902, name = "Guard", visitablePos = { x = 1, y = 1, z = 0 } },
	objects = {
		{
			id = 903,
			priority = 10,
			danger = 0,
			movementCost = 2,
			turn = 4,
			object = { id = 903, ID = "RESOURCE", resourceID = 6 }
		},
		{
			id = 904,
			priority = 5,
			danger = 100,
			movementCost = 4,
			turn = 6,
			object = { id = 904, ID = "PANDORAS_BOX" }
		}
	}
}, {
	targetHero = clusterHero
})
local clusterContext = PriorityEvaluator.buildEvaluationContext(clusterGoal, {
	playerID = 1,
	missingResourcesNow = { [6] = 1 },
	missingResourcesInTotal = { [6] = 1 },
	dailyIncome = { [6] = 1 }
})
assert(clusterContext.goldReward == 600 + 2500 / 2)
assert(clusterContext.armyReward == 5000 / 2)
assert(clusterContext.skillReward == 2.5 / 2)
assert(almostEquals(clusterContext.strategicalValue, 0.3))
assert(clusterContext.movementCost == 4)
assert(clusterContext.movementCostByRole[PriorityEvaluator.HeroRole.MAIN] == 4)
assert(clusterContext.turn == 4)

local chainHero = {
	id = 1001,
	role = PriorityEvaluator.HeroRole.MAIN,
	armyStrength = 1000,
	armyCost = 600,
	slots = {
		{ creatureID = 77, power = 200 }
	}
}
local chainPath = {
	targetHero = chainHero,
	tile = {
		x = 8,
		y = 9,
		z = 0,
		enemyHeroDanger = {
			maximumDanger = { danger = 300, threat = 400, turn = 1 }
		}
	},
	movementCost = 0.5,
	totalDanger = 100,
	totalArmyLoss = 200,
	heroStrength = 1000,
	turn = 1,
	heroArmy = {
		armyStrength = 1000,
		armyCost = 600
	},
	nodes = {
		{ targetHero = chainHero, cost = 0.5, layer = "SAIL" }
	}
}
local chainContext = PriorityEvaluator.buildEvaluationContext(ExecuteHeroChain.new(chainPath, {
	id = 1002,
	ID = "PANDORAS_BOX"
}), {
	playerID = 1,
	totalCreaturesAvailableByCreature = {
		[77] = { power = 1000 }
	}
})
assert(chainContext.danger == 100)
assert(almostEquals(chainContext.movementCost, 0.5))
assert(almostEquals(chainContext.movementCostByRole[PriorityEvaluator.HeroRole.MAIN], 0.5))
assert(chainContext.involvesSailing == true)
assert(chainContext.heroRole == PriorityEvaluator.HeroRole.MAIN)
assert(almostEquals(chainContext.powerRatio, 0.2))
assert(chainContext.goldReward == 2500)
assert(chainContext.armyReward == 5000)
assert(almostEquals(chainContext.skillReward, 2.6))
assert(chainContext.armyInvolvement == 600)
assert(almostEquals(chainContext.armyLossRatio, 0.2))
assert(almostEquals(chainContext.enemyHeroDangerRatio, 0.3))
assert(chainContext.threat == 400)
assert(chainContext.turn == 1)

local expensiveChain = PriorityEvaluator.buildEvaluationContext(ExecuteHeroChain.new({
	targetHero = chainHero,
	tile = { x = 1, y = 1, z = 0 },
	movementCost = 2,
	nodes = {
		{ targetHero = { id = 1003, role = PriorityEvaluator.HeroRole.SCOUT }, cost = 2 },
		{ targetHero = { id = 1004, role = PriorityEvaluator.HeroRole.SCOUT }, cost = 2 }
	}
}), {})
assert(expensiveChain.movementCost == 2)
assert(expensiveChain.goldReward == 0)

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
