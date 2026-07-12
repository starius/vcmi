local ArmyManager = require("Analyzers.ArmyManager")

local pikeman = { id = 1, factionID = 1, level = 1, movementRange = 4, aiValue = 10 }
local archer = { id = 2, factionID = 1, level = 2, movementRange = 6, aiValue = 12 }
local griffin = { id = 3, factionID = 2, level = 4, movementRange = 7, aiValue = 30 }
local wyvern = { id = 4, factionID = 3, level = 6, movementRange = 5, aiValue = 80 }

local sorted = ArmyManager.getSortedSlots(
	{
		slots = {
			{ creature = pikeman, count = 10, power = 100 },
			{ creature = archer, count = 5, power = 60 }
		}
	},
	{
		slots = {
			{ creature = pikeman, count = 4, power = 40 },
			{ creature = griffin, count = 3, power = 180 }
		}
	})
assert(#sorted == 3)
assert(sorted[1].creature == griffin)
assert(sorted[2].creature == pikeman)
assert(sorted[2].count == 14)
assert(sorted[2].power == 140)
assert(sorted[3].creature == archer)

local bestWithoutMoralePenalty = ArmyManager.getBestArmy(nil, {
	slots = {
		{ creature = pikeman, count = 10, power = 100 }
	}
}, {
	stacksCount = 1,
	slots = {
		{ creature = griffin, count = 3, power = 180 }
	}
}, {})
assert(#bestWithoutMoralePenalty == 2)
assert(bestWithoutMoralePenalty[1].creature == griffin)
assert(bestWithoutMoralePenalty[2].creature == pikeman)

local bestWithMoralePenalty = ArmyManager.getBestArmy(nil, {
	slots = {
		{ creature = pikeman, count = 10, power = 100 }
	}
}, {
	stacksCount = 1,
	slots = {
		{ creature = griffin, count = 3, power = 180 }
	}
}, {
	settings = {
		combatBadMoraleChance = { 1 },
		combatMoraleDiceSize = 2
	},
	moraleEvaluator = function(_, army)
		return #army > 1 and -1 or 0
	end
})
assert(#bestWithMoralePenalty == 1)
assert(bestWithMoralePenalty[1].creature == griffin)

local scout, scoutIndex = ArmyManager.getBestUnitForScout({
	{ creature = wyvern, count = 2, power = 300 },
	{ creature = pikeman, count = 1, power = 20 },
	{ creature = archer, count = 3, power = 75 }
}, {
	id = "rough",
	moveCost = 150
}, {
	settings = {
		heroesMovementCostBase = 100,
		heroesMovementPointsLand = {
			[0] = 100,
			[4] = 800,
			[5] = 900,
			[6] = 1000,
			[7] = 1100
		}
	}
})
assert(scout.creature == archer)
assert(scoutIndex == 3)

local scoutRetainedArmy = ArmyManager.getBestArmy(nil, {}, {
	stacksCount = 2,
	needsLastStack = true,
	slots = {
		{ creature = wyvern, count = 2, power = 300 },
		{ creature = pikeman, count = 1, power = 20 }
	}
}, {})
assert(#scoutRetainedArmy == 1)
assert(scoutRetainedArmy[1].creature == wyvern)

local scoutSplitArmy = ArmyManager.getBestArmy(nil, {}, {
	stacksCount = 1,
	needsLastStack = true,
	slots = {
		{ creature = wyvern, count = 3, power = 300 }
	}
}, {})
assert(#scoutSplitArmy == 1)
assert(scoutSplitArmy[1].creature == wyvern)
assert(scoutSplitArmy[1].count == 2)
assert(scoutSplitArmy[1].power == 200)
