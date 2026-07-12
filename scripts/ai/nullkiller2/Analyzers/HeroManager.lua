-- Mirrors AI/Nullkiller2/Analyzers/HeroManager.cpp secondary skill choice.

local PriorityEvaluator = require("Engine.PriorityEvaluator")

local HeroManager = {}

local Skill = {
	PATHFINDING = 0,
	LOGISTICS = 2,
	DIPLOMACY = 4,
	NAVIGATION = 5,
	LEADERSHIP = 6,
	WISDOM = 7,
	MYSTICISM = 8,
	EAGLE_EYE = 11,
	ESTATES = 13,
	FIRE_MAGIC = 14,
	AIR_MAGIC = 15,
	WATER_MAGIC = 16,
	EARTH_MAGIC = 17,
	SCHOLAR = 18,
	LEARNING = 21,
	OFFENCE = 22,
	ARMORER = 23,
	INTELLIGENCE = 24,
	SORCERY = 25,
	RESISTANCE = 26,
	FIRST_AID = 27
}

local MAIN_SKILL_SCORES = {
	[Skill.DIPLOMACY] = 2,
	[Skill.LOGISTICS] = 2,
	[Skill.EARTH_MAGIC] = 2,
	[Skill.ARMORER] = 2,
	[Skill.OFFENCE] = 2,
	[Skill.AIR_MAGIC] = 1,
	[Skill.WISDOM] = 1,
	[Skill.LEADERSHIP] = 1,
	[Skill.INTELLIGENCE] = 1,
	[Skill.RESISTANCE] = 1,
	[Skill.MYSTICISM] = -1,
	[Skill.SORCERY] = -1,
	[Skill.ESTATES] = -1,
	[Skill.FIRST_AID] = -1,
	[Skill.LEARNING] = -1,
	[Skill.SCHOLAR] = -1,
	[Skill.EAGLE_EYE] = -1,
	[Skill.NAVIGATION] = -1
}

local SCOUT_SKILL_SCORES = {
	[Skill.LOGISTICS] = 2,
	[Skill.ESTATES] = 2,
	[Skill.PATHFINDING] = 1,
	[Skill.SCHOLAR] = 1
}

local MAGIC_SCHOOLS = {
	[Skill.AIR_MAGIC] = true,
	[Skill.EARTH_MAGIC] = true,
	[Skill.FIRE_MAGIC] = true,
	[Skill.WATER_MAGIC] = true
}

local EXPERT = 3
local MAIN = PriorityEvaluator.HeroRole.MAIN

local function skillID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.skill or value.skillID or value.id or value[1]
	end
	return value
end

local function skillLevel(value)
	if type(value) == "table" then
		return value.level or value.mastery or value[2] or 0
	end
	return 0
end

local function heroSkills(hero)
	return hero and (hero.secSkills or hero.skills or hero.secondarySkills) or {}
end

local function heroSkillLevel(hero, skill)
	for _, entry in pairs(heroSkills(hero)) do
		if skillID(entry) == skill then
			return skillLevel(entry)
		end
	end
	return 0
end

local function heroHasAnyMagic(hero)
	for skill in pairs(MAGIC_SCHOOLS) do
		if heroSkillLevel(hero, skill) > 0 then
			return true
		end
	end
	return false
end

local function applyExistingSkillRule(hero, skill, score)
	local upgradesLeft = 0
	for _, entry in pairs(heroSkills(hero)) do
		if skillID(entry) == skill then
			return score
		end
		upgradesLeft = upgradesLeft + EXPERT - skillLevel(entry)
	end

	if score >= 2 or (score >= 1 and upgradesLeft <= 1) then
		score = score + 1.5
	end
	return score
end

local function evaluateMainSkill(hero, skill)
	local score = MAIN_SKILL_SCORES[skill] or 0
	score = applyExistingSkillRule(hero, skill, score)

	if skill == Skill.WISDOM and (hero and hero.level or 0) > 10 and heroSkillLevel(hero, Skill.WISDOM) == 0 then
		score = score + 1.5
	end

	if MAGIC_SCHOOLS[skill] and not heroHasAnyMagic(hero) then
		score = score + 1
	end

	return score
end

local function evaluateScoutSkill(hero, skill)
	local score = SCOUT_SKILL_SCORES[skill] or 0
	return applyExistingSkillRule(hero, skill, score)
end

function HeroManager.evaluateSecSkill(hero, skill, role)
	skill = skillID(skill)
	if role == nil then
		role = hero and hero.role
	end

	if role == MAIN then
		return evaluateMainSkill(hero, skill)
	end
	return evaluateScoutSkill(hero, skill)
end

function HeroManager.selectBestSkillIndex(hero, skills, role)
	local result = 0
	local resultScore = -100

	for index, skill in ipairs(skills or {}) do
		local score = HeroManager.evaluateSecSkill(hero, skill, role)
		if score > resultScore then
			resultScore = score
			result = index - 1
		end
	end

	return result
end

HeroManager.Skill = Skill

return HeroManager
