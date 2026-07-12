local RewardEvaluator = require("Engine.RewardEvaluator")

local function almostEquals(lhs, rhs)
	return math.abs(lhs - rhs) < 0.0001
end

local aiNk = {
	missingResourcesNow = { [0] = 2, [1] = 0, [6] = 500 },
	missingResourcesInTotal = { [0] = 0, [1] = 3, [6] = 500 },
	dailyIncome = { [0] = 1, [1] = 0, [6] = 1000 }
}

assert(RewardEvaluator.getResourcesGoldReward({ [0] = 2, [3] = 1, [6] = 500 }) == 800)
assert(RewardEvaluator.getNowResourceRequirementStrength(aiNk, 0) == 0.8)
assert(RewardEvaluator.getNowResourceRequirementStrength(aiNk, 1) == 0)
assert(RewardEvaluator.getTotalResourceRequirementStrength(aiNk, 1) == 1.0)
assert(almostEquals(RewardEvaluator.getCombinedResourceRequirementStrength(aiNk, { [0] = 1, [1] = 1, [6] = 1 }), 0.9))

local army = {
	slots = {
		{ count = 10, marketValue = 60 },
		{ count = 2, creature = { fullRecruitCost = { [0] = 1, [6] = 40 } } }
	}
}
assert(RewardEvaluator.getArmyCost(army) == 880)

local hero = { owner = 1, tempOwner = 1 }
assert(RewardEvaluator.getGoldReward({ ID = "RESOURCE", resourceID = 6 }, hero, aiNk) == 600)
assert(RewardEvaluator.getGoldReward({ ID = "RESOURCE", resourceID = 0 }, hero, aiNk) == 100)
assert(RewardEvaluator.getGoldReward({ ID = "TREASURE_CHEST" }, hero, aiNk) == 1500)
assert(RewardEvaluator.getGoldReward({ ID = "WATER_WHEEL" }, hero, aiNk) == 1000)
assert(RewardEvaluator.getGoldReward({ ID = "PANDORAS_BOX" }, hero, aiNk) == 2500)
assert(RewardEvaluator.getGoldReward({ ID = "PRISON" }, hero, aiNk) == 2500)

assert(RewardEvaluator.getGoldReward({
	ID = "MINE",
	producedResource = 6
}, hero, aiNk) == 5000)
assert(RewardEvaluator.getGoldReward({
	ID = "ABANDONED_MINE",
	producedResource = 0
}, hero, aiNk) == 375)

assert(RewardEvaluator.getGoldReward({
	ID = "TOWN",
	owner = 2,
	hasCapitol = true
}, hero, aiNk) == 20000)
assert(RewardEvaluator.getGoldReward({
	ID = "TOWN",
	owner = 1,
	hasCapitol = true
}, hero, aiNk) == 0)

assert(almostEquals(RewardEvaluator.getGoldReward({
	ID = "HERO",
	owner = 2,
	slots = {
		{ count = 10, marketValue = 100 }
	}
}, hero, aiNk), 1450))

assert(RewardEvaluator.getGoldReward({
	ID = "HERO",
	owner = 1,
	slots = {
		{ count = 10, marketValue = 100 }
	}
}, hero, aiNk) == 0)

assert(RewardEvaluator.getGoldReward({
	ID = "REWARDABLE",
	rewards = {
		{ resources = { [0] = 2 } },
		{ resources = { [6] = 300 } }
	}
}, hero, aiNk) == 500)
