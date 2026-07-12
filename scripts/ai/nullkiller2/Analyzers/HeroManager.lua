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
local SCOUT = PriorityEvaluator.HeroRole.SCOUT
local MAP_SIZE_LARGE = 108

local function objectID(value)
	if type(value) == "number" or type(value) == "string" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

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

local function primarySkill(hero, key, vectorIndex)
	if not hero then
		return 0
	end
	local primary = hero.primarySkills or hero.primSkills or {}
	return hero[key] or primary[key] or primary[vectorIndex] or 0
end

local function basePrimarySkillScore(hero)
	return primarySkill(hero, "attack", 1)
		+ primarySkill(hero, "defense", 2)
		+ primarySkill(hero, "spellPower", 3)
		+ primarySkill(hero, "knowledge", 4)
end

local function heroPatrolling(hero)
	if not hero then
		return false
	end
	if hero.patrolling ~= nil then
		return hero.patrolling == true
	end
	return type(hero.patrol) == "table" and hero.patrol.patrolling == true
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

local function evaluateMainSkills(hero)
	local score = 0
	for _, entry in pairs(heroSkills(hero)) do
		score = score + skillLevel(entry) * evaluateMainSkill(hero, skillID(entry))
	end
	return score
end

function HeroManager.new(aiNk)
	return setmetatable({
		aiNk = aiNk or {},
		heroToRoleMap = {},
		knownFightingStrength = {}
	}, {
		__index = HeroManager
	})
end

local function managerHeroes(manager)
	local aiNk = manager and manager.aiNk or {}
	return call(aiNk.cc, "getHeroesInfo") or aiNk.heroesInfo or {}
end

local function managerTowns(manager)
	local aiNk = manager and manager.aiNk or {}
	return call(aiNk.cc, "getTownsInfo") or aiNk.townsInfo or {}
end

local function managerCurrentDay(manager)
	local aiNk = manager and manager.aiNk or {}
	return aiNk.currentDay or aiNk.day or 0
end

local function managerMapSizeX(manager)
	local aiNk = manager and manager.aiNk or {}
	local mapSize = aiNk.mapSize or {}
	return aiNk.mapSizeX or mapSize.x or mapSize[1] or 0
end

function HeroManager.evaluateFightingStrength(hero)
	if not hero then
		return 0
	end
	if hero.evaluateFightingStrengthScore ~= nil then
		return hero.evaluateFightingStrengthScore
	end
	if hero.evaluateHeroScore ~= nil then
		return hero.evaluateHeroScore
	end
	return (hero.specialityScore or hero.specialtyScore or 0) + evaluateMainSkills(hero) + basePrimarySkillScore(hero)
end

function HeroManager.evaluateHero(selfOrHero, maybeHero)
	local hero = maybeHero or selfOrHero
	return HeroManager.evaluateFightingStrength(hero)
end

function HeroManager:update()
	local heroes = {}
	for _, hero in ipairs(managerHeroes(self)) do
		table.insert(heroes, hero)
	end

	local scores = {}
	for _, hero in ipairs(heroes) do
		local id = objectID(hero)
		local score = HeroManager.evaluateFightingStrength(hero)
		scores[id] = score
		self.knownFightingStrength[tostring(id)] = hero.heroStrength or hero.totalStrength or hero.armyStrength or score
	end

	table.sort(heroes, function(lhs, rhs)
		local leftScore = scores[objectID(lhs)] or 0
		local rightScore = scores[objectID(rhs)] or 0
		if leftScore == rightScore then
			return tostring(objectID(lhs) or "") < tostring(objectID(rhs) or "")
		end
		return leftScore > rightScore
	end)

	local townCount = #managerTowns(self)
	local biggerMapFactor = managerCurrentDay(self) > 21 and math.floor(managerMapSizeX(self) / MAP_SIZE_LARGE) or 0
	local globalMainCount = math.max(townCount + biggerMapFactor, 1)
	globalMainCount = math.min(globalMainCount, townCount * 2)

	self.heroToRoleMap = {}
	for _, hero in ipairs(heroes) do
		local role
		if heroPatrolling(hero) then
			role = MAIN
		else
			role = globalMainCount > 0 and MAIN or SCOUT
			globalMainCount = globalMainCount - 1
		end

		local id = objectID(hero)
		self.heroToRoleMap[tostring(id)] = role
		hero.role = role
	end
end

function HeroManager:getHeroRoleOrDefault(hero)
	local id = objectID(hero)
	local role = self.heroToRoleMap[tostring(id)]
	if role ~= nil then
		return role
	end
	return hero and hero.role or SCOUT
end

function HeroManager:getHeroRoleOrDefaultInefficient(hero)
	return self:getHeroRoleOrDefault(hero)
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

function HeroManager.selectBestSkillIndex(selfOrHero, maybeHeroOrSkills, maybeSkillsOrRole, maybeRole)
	local manager = type(selfOrHero) == "table" and selfOrHero.heroToRoleMap and selfOrHero or nil
	local hero = manager and maybeHeroOrSkills or selfOrHero
	local skills = manager and maybeSkillsOrRole or maybeHeroOrSkills
	local role = nil
	if manager then
		role = maybeRole
	else
		role = maybeSkillsOrRole
	end
	if manager and role == nil then
		role = manager:getHeroRoleOrDefault(hero)
	end

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
