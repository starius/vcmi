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

