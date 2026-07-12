local RewardEvaluator = require("Engine.RewardEvaluator")

local function almostEquals(lhs, rhs)
	return math.abs(lhs - rhs) < 0.0001
end

local aiNk = {
	playerID = 1,
	missingResourcesNow = { [0] = 2, [1] = 0, [6] = 500 },
	missingResourcesInTotal = { [0] = 0, [1] = 3, [6] = 500 },
	dailyIncome = { [0] = 1, [1] = 0, [6] = 1000 },
	freeResources = { [6] = 300 },
	settings = {
		marketsUniversityGoldCost = 2000,
		dwellingsAccumulateWhenOwned = false
	},
	calendar = {
		dayOfWeek = 3
	}
}

assert(RewardEvaluator.getResourcesGoldReward({ [0] = 2, [3] = 1, [6] = 500 }) == 800)
assert(RewardEvaluator.getNowResourceRequirementStrength(aiNk, 0) == 0.8)
assert(RewardEvaluator.getNowResourceRequirementStrength(aiNk, 1) == 0)
assert(RewardEvaluator.getTotalResourceRequirementStrength(aiNk, 1) == 1.0)
assert(almostEquals(RewardEvaluator.getCombinedResourceRequirementStrength(aiNk, { [0] = 1, [1] = 1, [6] = 1 }), 0.9))

local evaluator = RewardEvaluator.new(aiNk)
assert(evaluator:getNowResourceRequirementStrength(0) == 0.8)
assert(evaluator:getTotalResourceRequirementStrength(1) == 1.0)
assert(almostEquals(evaluator:getCombinedResourceRequirementStrength({ [0] = 1, [1] = 1, [6] = 1 }), 0.9))

local army = {
	slots = {
		{ count = 10, marketValue = 60 },
		{ count = 2, creature = { fullRecruitCost = { [0] = 1, [6] = 40 } } }
	}
}
assert(RewardEvaluator.getArmyCost(army) == 880)
assert(evaluator:getArmyCost(army) == 880)
assert(almostEquals(evaluator:getManaRecoveryArmyReward({ magicStrength = 2, mana = 25, manaLimit = 100 }), 10000))

local hero = { owner = 1, tempOwner = 1 }
local dwelling = {
	ID = "CREATURE_GENERATOR1",
	owner = 2,
	creatures = {
		{ 5, { { aiValue = 20, level = 2, fullRecruitCost = { [6] = 100 }, growth = 3 } } },
		{ 2, { { aiValue = 10, level = 1, fullRecruitCost = { [6] = 50 }, growth = 4 } } }
	}
}
assert(evaluator:getArmyReward(dwelling, hero, nil, false) == 120)
assert(evaluator:getArmyReward(dwelling, hero, nil, true) == 20)
assert(evaluator:getArmyGrowth(dwelling, hero) == 660)
assert(evaluator:getGoldCost(dwelling, hero) == 500)
assert(evaluator:getGoldCost({ ID = "SCHOOL_OF_MAGIC" }, hero) == 1000)
assert(evaluator:getGoldCost({ ID = "MARKET", allowsResourceSkill = true }, hero) == 2000)

assert(RewardEvaluator.getGoldReward({ ID = "RESOURCE", resourceID = 6 }, hero, aiNk) == 600)
assert(evaluator:getGoldReward({ ID = "RESOURCE", resourceID = 6 }, hero) == 600)
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

assert(almostEquals(evaluator:getStrategicalValue({
	ID = "MINE",
	producedResource = 0,
	producedQuantity = 2
}), 1.3))
assert(almostEquals(evaluator:getStrategicalValue({
	ID = "RESOURCE",
	resourceID = 1,
	amount = 3
}), 0.3))
assert(evaluator:getStrategicalValue({
	ID = "TOWN",
	owner = 2,
	fortLevel = 3
}) == 1.4)
assert(evaluator:getConquestValue({
	ID = "TOWN",
	owner = 2,
	fortLevel = 2
}) == 1.2)
assert(evaluator:getConquestValue({
	ID = "TOWN",
	owner = 1,
	fortLevel = 3
}) == 0)

assert(almostEquals(RewardEvaluator.getGoldReward({
	ID = "HERO",
	owner = 2,
	slots = {
		{ count = 10, marketValue = 100 }
	}
}, hero, aiNk), 1450))
assert(almostEquals(evaluator:getArmyReward({
	ID = "HERO",
	owner = 2,
	armyStrength = 8000
}, hero), 4000))
assert(almostEquals(evaluator:getStrategicalValue({
	ID = "HERO",
	owner = 2,
	level = 4,
	objectValueUnderThreat = 1
}, hero), 1.5))
assert(almostEquals(evaluator:getConquestValue({
	ID = "HERO",
	owner = 2,
	level = 1
}), 0.75))

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
assert(evaluator:getArmyReward({
	ID = "REWARDABLE",
	rewards = {
		{
			grantedArtifacts = { { potentialScore = 700 } },
			grantedScrolls = { 1 },
			creatures = {
				{ creature = { aiValue = 30 }, count = 4 }
			}
		}
	}
}, hero) == 2320)
assert(almostEquals(evaluator:getStrategicalValue({
	ID = "REWARDABLE",
	rewards = {
		{ resources = { [0] = 1 } },
		{ resources = { [6] = 100 } }
	}
}, hero), 0.6))

assert(evaluator:getSkillReward({ ID = "STAR_AXIS" }, hero, RewardEvaluator.HERO_ROLE_MAIN) == 1)
assert(almostEquals(evaluator:getSkillReward({ ID = "LEARNING_STONE" }, { level = 4 }, RewardEvaluator.HERO_ROLE_SCOUT), 0.5))
assert(evaluator:getSkillReward({ ID = "ARENA" }, hero, RewardEvaluator.HERO_ROLE_MAIN) == 2)
assert(evaluator:getSkillReward({ ID = "SHRINE_OF_MAGIC_INCANTATION" }, hero, RewardEvaluator.HERO_ROLE_MAIN) == 0.25)
assert(evaluator:getSkillReward({ ID = "SHRINE_OF_MAGIC_GESTURE" }, hero, RewardEvaluator.HERO_ROLE_MAIN) == 1)
assert(evaluator:getSkillReward({ ID = "SHRINE_OF_MAGIC_THOUGHT" }, hero, RewardEvaluator.HERO_ROLE_MAIN) == 2)
assert(evaluator:getSkillReward({ ID = "LIBRARY_OF_ENLIGHTENMENT" }, hero, RewardEvaluator.HERO_ROLE_MAIN) == 8)
assert(evaluator:getSkillReward({ ID = "PANDORAS_BOX" }, hero, RewardEvaluator.HERO_ROLE_MAIN) == 2.5)
assert(evaluator:getSkillReward({
	ID = "WITCH_HUT",
	wasVisited = false
}, hero, RewardEvaluator.HERO_ROLE_SCOUT) == 2)
assert(evaluator:getSkillReward({
	ID = "WITCH_HUT",
	wasVisited = true,
	gainedSkill = 7,
	skillScore = 3
}, { skills = {} }, RewardEvaluator.HERO_ROLE_MAIN) == 10)
assert(evaluator:getSkillReward({
	ID = "WITCH_HUT",
	wasVisited = true,
	gainedSkill = 7,
	skillScore = 3
}, { skills = { [7] = 1 } }, RewardEvaluator.HERO_ROLE_MAIN) == 0)
assert(evaluator:getSkillReward({
	ID = "HERO",
	owner = 2,
	level = 12
}, hero, RewardEvaluator.HERO_ROLE_MAIN) == 6)
assert(almostEquals(evaluator:getSkillReward({
	ID = "REWARDABLE",
	rewards = {
		{
			spells = {
				{ level = 1, canLearn = true },
				{ level = 4, canLearn = true }
			},
			primary = { 1, 2 }
		}
	}
}, hero, RewardEvaluator.HERO_ROLE_MAIN), 3.375))
