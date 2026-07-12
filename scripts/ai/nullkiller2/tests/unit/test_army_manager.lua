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

assert(ArmyManager.evaluateStackPower(wyvern, 2) == 160)

local townCreature = { id = 5, factionID = 1, aiValue = 20, fullRecruitCost = { [7] = 50 } }
local betterTownCreature = { id = 6, factionID = 1, aiValue = 50, fullRecruitCost = { [7] = 100 } }
local outsider = { id = 7, factionID = 2, aiValue = 5, fullRecruitCost = { [7] = 10 } }
local fullArmy = {
	armySize = 7,
	stacksCount = 7,
	slots = {
		{ slot = 0, creature = townCreature, count = 1, marketValue = 1000 },
		{ slot = 1, creature = outsider, count = 1, marketValue = 100 },
		{ slot = 2, creature = { id = 8, factionID = 3, fullRecruitCost = { [7] = 200 } }, count = 1, marketValue = 200 },
		{ slot = 3, creature = { id = 9, factionID = 3, fullRecruitCost = { [7] = 200 } }, count = 1, marketValue = 200 },
		{ slot = 4, creature = { id = 10, factionID = 3, fullRecruitCost = { [7] = 200 } }, count = 1, marketValue = 200 },
		{ slot = 5, creature = { id = 11, factionID = 3, fullRecruitCost = { [7] = 200 } }, count = 1, marketValue = 200 },
		{ slot = 6, creature = { id = 12, factionID = 3, fullRecruitCost = { [7] = 200 } }, count = 1, marketValue = 200 }
	}
}
local dwelling = {
	factionID = 1,
	availableToBuy = {
		{ level = 0, count = 3, creature = townCreature },
		{ level = 1, count = 5, creature = betterTownCreature }
	}
}
local armyAvailable = ArmyManager.getArmyAvailableToBuy(fullArmy, dwelling, { [7] = 1000 })
assert(#armyAvailable == 2)
assert(armyAvailable[1].creature == betterTownCreature)
assert(armyAvailable[1].count == 4)
assert(armyAvailable[1].level == 1)
assert(armyAvailable[2].creature == townCreature)
assert(armyAvailable[2].count == 3)

assert(ArmyManager.howManyReinforcementsCanBuy(fullArmy, dwelling, { [7] = 1000 }) == 260)

local sourceSet = ArmyManager.getArmyAvailableToBuyAsCCreatureSet(dwelling, { [7] = 650 })
assert(sourceSet.stacksCount == 2)
assert(sourceSet.slots[1].creature == betterTownCreature)
assert(sourceSet.slots[1].count == 5)

local reinforcementDelta = ArmyManager.howManyReinforcementsCanGet(nil, {
	armyStrength = 100,
	slots = {
		{ creature = townCreature, count = 5, power = 100 }
	}
}, {
	stacksCount = 1,
	slots = {
		{ creature = betterTownCreature, count = 5, power = 250 }
	}
}, nil, {})
assert(reinforcementDelta == 250)

local totalArmy = ArmyManager.update({
	heroesInfo = {
		{
			id = 1001,
			slots = {
				{ creature = pikeman, count = 5, power = 50 },
				{ creature = archer, count = 3, power = 36 }
			}
		}
	},
	townsInfo = {
		{
			id = 1002,
			upperArmy = {
				slots = {
					{ creature = pikeman, count = 2, power = 20 },
					{ creature = griffin, count = 1, power = 60 }
				}
			}
		}
	}
})
assert(totalArmy[1].count == 7)
assert(totalArmy[1].power == 70)
assert(totalArmy[2].count == 3)
assert(totalArmy[3].count == 1)

local totalPikemen = ArmyManager.getTotalCreaturesAvailable(pikeman)
assert(totalPikemen.creature == pikeman)
assert(totalPikemen.count == 7)
assert(totalPikemen.power == 70)

local missingCreature = ArmyManager.getTotalCreaturesAvailable({ id = 9999 })
assert(missingCreature.creatureID == 9999)
assert(missingCreature.count == 0)
assert(missingCreature.power == 0)
