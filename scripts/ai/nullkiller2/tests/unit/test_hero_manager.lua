local HeroManager = require("Analyzers.HeroManager")
local PriorityEvaluator = require("Engine.PriorityEvaluator")

local Skill = HeroManager.Skill

assert(HeroManager.selectBestSkillIndex({}, {
	Skill.LOGISTICS,
	Skill.PATHFINDING
}) == 0)

assert(HeroManager.selectBestSkillIndex({
	level = 12,
	role = PriorityEvaluator.HeroRole.MAIN,
	secSkills = {}
}, {
	Skill.WISDOM,
	Skill.LOGISTICS
}) == 0)

assert(HeroManager.selectBestSkillIndex({
	secSkills = {
		{ skill = Skill.LOGISTICS, level = 1 }
	}
}, {
	Skill.LOGISTICS,
	Skill.ESTATES
}) == 1)

local manager = HeroManager.new({
	heroesInfo = {
		{ id = 1, evaluateHeroScore = 10 },
		{ id = 2, evaluateHeroScore = 30 },
		{ id = 3, evaluateHeroScore = 5, patrol = { patrolling = true } }
	},
	townsInfo = {
		{ id = 10 }
	},
	currentDay = 1,
	mapSize = { x = 108, y = 108, z = 0 }
})
manager:update()
assert(manager:getHeroRoleOrDefault({ id = 2 }) == PriorityEvaluator.HeroRole.MAIN)
assert(manager:getHeroRoleOrDefault({ id = 1 }) == PriorityEvaluator.HeroRole.SCOUT)
assert(manager:getHeroRoleOrDefault({ id = 3 }) == PriorityEvaluator.HeroRole.MAIN)

local biggerMapManager = HeroManager.new({
	heroesInfo = {
		{ id = 11, evaluateHeroScore = 40 },
		{ id = 12, evaluateHeroScore = 30 },
		{ id = 13, evaluateHeroScore = 20 }
	},
	townsInfo = {
		{ id = 20 }
	},
	currentDay = 22,
	mapSize = { x = 216, y = 216, z = 0 }
})
biggerMapManager:update()
assert(biggerMapManager:getHeroRoleOrDefault({ id = 11 }) == PriorityEvaluator.HeroRole.MAIN)
assert(biggerMapManager:getHeroRoleOrDefault({ id = 12 }) == PriorityEvaluator.HeroRole.MAIN)
assert(biggerMapManager:getHeroRoleOrDefault({ id = 13 }) == PriorityEvaluator.HeroRole.SCOUT)

assert(manager:selectBestSkillIndex({ id = 2, level = 12, secSkills = {} }, {
	Skill.EARTH_MAGIC,
	Skill.ESTATES
}) == 0)

assert(manager:selectBestSkillIndex({ id = 1, level = 12, secSkills = {} }, {
	Skill.EARTH_MAGIC,
	Skill.ESTATES
}) == 1)
