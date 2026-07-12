-- Mirrors AI/Nullkiller2/AIUtility.{h,cpp}: Lua-owned utility scoring used by gateway policy.

local RewardEvaluator = require("Engine.RewardEvaluator")

local AIUtility = {}

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function subtypeValue(value)
	if type(value) == "table" then
		return value.type or value.name or value.id or value.num or value[1]
	end
	return value
end

local function bonusType(bonus)
	return bonus and (bonus.type or bonus.bonusType)
end

local function bonusSubtype(bonus)
	return subtypeValue(bonus and (bonus.subtype or bonus.subType or bonus.subtypeID))
end

local function isSubtype(value, ...)
	local subtype = subtypeValue(value)
	for _, candidate in ipairs({ ... }) do
		if subtype == candidate then
			return true
		end
	end
	return false
end

local function propagatorType(propagator)
	if type(propagator) == "table" then
		return propagator.type or propagator.propagatorType or propagator[1]
	end
	return propagator
end

local function isBattleWideBonus(bonus)
	return bonus and (bonus.propagatorType == "BATTLE_WIDE"
		or bonus.propagatorType == 1
		or propagatorType(bonus.propagator) == "BATTLE_WIDE"
		or propagatorType(bonus.propagator) == 1)
end

local function hasLimiter(bonus)
	return bonus and ((bonus.limiter ~= nil and bonus.limiter ~= false) or bonus.limiters ~= nil)
end

local function heroSlots(hero)
	if not hero then
		return {}
	end
	if type(hero.Slots) == "function" then
		return hero:Slots()
	end
	return hero.slots or hero.stacks or {}
end

local function stackPower(stack)
	return call(stack, "getPower") or stack and (stack.power or stack.stackPower or stack.armyStrength) or 0
end

local function stackCreatureID(stack)
	local creature = stack and stack.creature
	return stack and (stack.creatureID or stack.id or creature and (creature.id or creature.num))
end

local function bonusTypes(object)
	return object and (object.bonusTypes or object.bonusesByType or object.hasBonuses) or {}
end

local function hasBonusOfType(object, typeName)
	local result = call(object, "hasBonusOfType", typeName)
	if result ~= nil then
		return result
	end
	local bonuses = bonusTypes(object)
	if bonuses[typeName] ~= nil then
		return bonuses[typeName] ~= false
	end
	for _, bonus in ipairs(object and object.bonuses or {}) do
		if bonusType(bonus) == typeName then
			return true
		end
	end
	return false
end

local function heroHasSpellbook(hero)
	local result = call(hero, "hasSpellbook")
	if result ~= nil then
		return result
	end
	if hero and hero.hasSpellbook ~= nil then
		return hero.hasSpellbook
	end
	return hero and (hero.spellbook ~= nil or hero.spells ~= nil or hero.spellsInSpellbook ~= nil) or false
end

local function heroHasBoat(hero)
	local boat = call(hero, "getBoat")
	if boat ~= nil then
		return boat ~= false
	end
	if not hero then
		return false
	end
	if hero.boat ~= nil then
		return hero.boat ~= false
	end
	return hero.hasBoat == true
end

local function heroKnowsSpell(hero, spell)
	local spellID = spell and (spell.id or spell.spellID or spell[1])
	if spell and spell.known ~= nil then
		return spell.known
	end
	local spells = hero and (hero.spellsInSpellbook or hero.spellbook or hero.spells) or {}
	if spellID ~= nil and spells[spellID] ~= nil then
		return spells[spellID] ~= false
	end
	return false
end

local function spellLevel(spell)
	return spell and (spell.level or spell.spellLevel) or 1
end

local function acceptedCreatureSet(bonus)
	local limiter = bonus and bonus.limiter
	if type(limiter) ~= "table" then
		limiter = {}
	end
	return bonus and (bonus.affectedCreatureIDs or bonus.creatureIDs)
		or limiter.affectedCreatureIDs
		or limiter.creatureIDs
end

local function getArmyRatioAffectedByLimiter(hero, bonus)
	if not hasLimiter(bonus) then
		return 1.0
	end
	if bonus.affectedArmyRatio ~= nil then
		return bonus.affectedArmyRatio
	end
	if bonus.limiterRatio ~= nil then
		return bonus.limiterRatio
	end

	local accepted = acceptedCreatureSet(bonus)
	local acceptedByID = {}
	for _, id in ipairs(accepted or {}) do
		acceptedByID[id] = true
	end

	local totalStrength = 0
	local affectedStrength = 0
	for _, stack in pairs(heroSlots(hero)) do
		local power = stackPower(stack)
		totalStrength = totalStrength + power
		if stack.limiterAccepted == true
			or acceptedByID[stackCreatureID(stack)]
			or (bonus.limiterBonusType and hasBonusOfType(stack, bonus.limiterBonusType)) then
			affectedStrength = affectedStrength + power
		end
	end

	if totalStrength == 0 then
		return 0.0
	end
	if accepted == nil and bonus.limiterBonusType == nil then
		return 1.0
	end
	return affectedStrength / totalStrength
end

local function getArmyPercentageWithBonus(hero, typeName)
	local totalStrength = 0
	local affectedStrength = 0
	for _, stack in pairs(heroSlots(hero)) do
		local power = stackPower(stack)
		totalStrength = totalStrength + power
		if hasBonusOfType(stack, typeName) then
			affectedStrength = affectedStrength + power
		end
	end
	if totalStrength == 0 then
		return 0.0
	end
	return affectedStrength / totalStrength
end

local function defaultAllowedSpells(hero)
	return hero and (hero.defaultAllowedSpells or hero.allowedSpells) or {}
end

local function spellHasSchool(spell, school)
	if spell.school == school or spell.spellSchool == school then
		return true
	end
	for _, entry in ipairs(spell.schools or {}) do
		if entry == school then
			return true
		end
	end
	return false
end

local function getSpellSchoolKnownSpellsFactor(hero, school)
	local factors = hero and (hero.spellSchoolUnknownFactor or hero.spellSchoolKnownSpellsFactor)
	if factors and factors[school] ~= nil then
		return factors[school]
	end

	local totalWeight = 0
	local unknownWeight = 0
	for _, spell in ipairs(defaultAllowedSpells(hero)) do
		if spellHasSchool(spell, school) then
			local level = spellLevel(spell)
			local weight = level * level
			if not heroKnowsSpell(hero, spell) then
				unknownWeight = unknownWeight + weight
			end
			totalWeight = totalWeight + weight
		end
	end
	if totalWeight == 0 then
		return 0.0
	end
	return unknownWeight / totalWeight
end

local function getSpellLevelKnownSpellsFactor(hero, level)
	local factors = hero and (hero.spellLevelUnknownFactor or hero.spellLevelKnownSpellsFactor)
	if factors and factors[level] ~= nil then
		return factors[level]
	end

	local totalWeight = 0
	local unknownWeight = 0
	for _, spell in ipairs(defaultAllowedSpells(hero)) do
		if spellLevel(spell) == level then
			if not heroKnowsSpell(hero, spell) then
				unknownWeight = unknownWeight + 1
			end
			totalWeight = totalWeight + 1
		end
	end
	if totalWeight == 0 then
		return 0.0
	end
	return unknownWeight / totalWeight
end

function AIUtility.getArtifactBonusRelevance(hero, bonus)
	if isBattleWideBonus(bonus) and hasLimiter(bonus) then
		return 1.0
	end

	local type = bonusType(bonus)
	local subtype = bonusSubtype(bonus)

	if type == "MOVEMENT" then
		if heroHasBoat(hero) and isSubtype(subtype, "heroMovementSea", "HERO_MOVEMENT_SEA", "SEA", 1) then
			return 2.0
		end
		if not heroHasBoat(hero) and isSubtype(subtype, "heroMovementLand", "HERO_MOVEMENT_LAND", "LAND", 0) then
			return 1.0
		end
		return 0.0
	elseif type == "STACKS_SPEED" or type == "STACK_HEALTH" then
		return getArmyRatioAffectedByLimiter(hero, bonus)
	elseif type == "MORALE" then
		return getArmyRatioAffectedByLimiter(hero, bonus) * (1 - getArmyPercentageWithBonus(hero, "UNDEAD"))
	elseif type == "LUCK" then
		return getArmyRatioAffectedByLimiter(hero, bonus)
	elseif type == "PRIMARY_SKILL" then
		if isSubtype(subtype, "ATTACK", 0) or isSubtype(subtype, "DEFENSE", 1) then
			return getArmyRatioAffectedByLimiter(hero, bonus)
		end
		return 1.0
	elseif type == "WATER_WALKING" or type == "FLYING_MOVEMENT" then
		return heroHasBoat(hero) and 0.0 or 1.0
	elseif type == "WHIRLPOOL_PROTECTION" then
		return heroHasBoat(hero) and 1.0 or 0.0
	elseif type == "UNDEAD_RAISE_PERCENTAGE" then
		return hasBonusOfType(hero, "IMPROVED_NECROMANCY") and 2.0 or 0.0
	elseif type == "SPELL_DAMAGE" or type == "SPELL_DURATION" then
		return heroHasSpellbook(hero) and 1.0 or 0.0
	elseif type == "PERCENTAGE_DAMAGE_BOOST" then
		if isSubtype(subtype, "damageTypeRanged", "DAMAGE_TYPE_RANGED", "RANGED", 1) then
			return 2.0 * getArmyPercentageWithBonus(hero, "SHOOTER")
		end
		if isSubtype(subtype, "damageTypeMelee", "DAMAGE_TYPE_MELEE", "MELEE", 0) then
			return 2.0 * (1 - getArmyPercentageWithBonus(hero, "SHOOTER"))
		end
		return 0.0
	elseif type == "MANA_PERCENTAGE_REGENERATION" or type == "MANA_REGENERATION" then
		return heroHasSpellbook(hero) and 1.0 or 0.0
	elseif type == "LEARN_BATTLE_SPELL_CHANCE" then
		return hasBonusOfType(hero, "LEARN_BATTLE_SPELL_LEVEL_LIMIT") and 1.0 or 0.0
	elseif type == "LEARN_BATTLE_SPELL_CHANCE_PRE_BATTLE" then
		return hasBonusOfType(hero, "LEARN_BATTLE_SPELL_LEVEL_LIMIT_PRE_BATTLE") and 1.0 or 0.0
	elseif type == "NO_DISTANCE_PENALTY" or type == "NO_WALL_PENALTY" then
		return getArmyPercentageWithBonus(hero, "SHOOTER") * 2.0
	elseif type == "SPELLS_OF_SCHOOL" then
		if not heroHasSpellbook(hero) then
			return 0.0
		end
		return 1 - getSpellSchoolKnownSpellsFactor(hero, subtype)
	elseif type == "SPELLS_OF_LEVEL" then
		if not heroHasSpellbook(hero) then
			return 0.0
		end
		return 1 - getSpellLevelKnownSpellsFactor(hero, subtypeValue(subtype))
	end

	return 1.0
end

local function artifactType(artifact)
	return artifact and (artifact.artifactType or artifact.typeInfo or artifact.type or artifact)
end

local function artifactID(artifact)
	local definition = artifactType(artifact)
	if type(definition) == "table" then
		return definition.ID or definition.id or definition.type or definition.name
	end
	return definition
end

local function exportedBonuses(artifact)
	local definition = artifactType(artifact)
	return call(definition, "getExportedBonusList")
		or type(definition) == "table" and (definition.exportedBonusList or definition.exportedBonuses or definition.bonuses)
		or artifact and (artifact.exportedBonusList or artifact.exportedBonuses or artifact.bonuses)
		or {}
end

local function constituentArtifacts(artifact)
	local definition = artifactType(artifact)
	return call(definition, "getConstituents")
		or type(definition) == "table" and (definition.constituents or definition.parts)
		or artifact and (artifact.constituents or artifact.parts)
		or {}
end

local function artifactIsScroll(artifact)
	return artifact and (artifact.isScroll == true
		or artifact.spellScroll == true
		or artifactID(artifact) == "SPELL_SCROLL")
end

local function scrollSpell(artifact)
	return artifact and (artifact.scrollSpell or artifact.spell or {
		id = artifact.scrollSpellID or artifact.spellID,
		level = artifact.spellLevel
	})
end

function AIUtility.getArtifactScoreForHero(hero, artifact)
	if artifactIsScroll(artifact) then
		local spell = scrollSpell(artifact)
		if heroKnowsSpell(hero, spell) then
			return 0
		end
		return spellLevel(spell) * 100
	end

	if artifactID(artifact) == "SPELLBOOK" then
		return 0
	end

	local totalScore = 0
	for _, bonus in ipairs(exportedBonuses(artifact)) do
		totalScore = totalScore + AIUtility.getArtifactBonusRelevance(hero, bonus)
			* RewardEvaluator.getArtifactBonusScore(bonus)
	end
	for _, part in ipairs(constituentArtifacts(artifact)) do
		for _, bonus in ipairs(exportedBonuses(part)) do
			totalScore = totalScore + AIUtility.getArtifactBonusRelevance(hero, bonus)
				* RewardEvaluator.getArtifactBonusScore(bonus)
		end
	end
	return totalScore
end

return AIUtility
