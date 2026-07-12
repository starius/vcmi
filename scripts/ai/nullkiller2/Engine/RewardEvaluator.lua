-- Mirrors AI/Nullkiller2/Engine/PriorityEvaluator.{h,cpp}: RewardEvaluator helpers.

local RewardEvaluator = {}
RewardEvaluator.__index = RewardEvaluator

local GOLD = 6
local HERO_GOLD_COST = 2500
local MINIMUM_STRATEGICAL_VALUE_NON_TOWN = 0.3
local UNIVERSITY_GOLD_COST = 2000
local CITADEL = 2
local CASTLE = 3
local HERO_ROLE_SCOUT = 0
local HERO_ROLE_MAIN = 1

local RESOURCE_PRICES = {
	[0] = 250,
	[1] = 500,
	[2] = 250,
	[3] = 500,
	[4] = 500,
	[5] = 500,
	[6] = 1,
	wood = 250,
	mercury = 500,
	ore = 250,
	sulfur = 500,
	crystal = 500,
	gems = 500,
	gold = 1
}

local CREATURE_GENERATORS = {
	CREATURE_GENERATOR1 = true,
	CREATURE_GENERATOR2 = true,
	CREATURE_GENERATOR3 = true,
	CREATURE_GENERATOR4 = true
}

function RewardEvaluator.new(aiNk)
	return setmetatable({
		aiNk = aiNk
	}, RewardEvaluator)
end

local function aiContext(value)
	return value and value.aiNk or value
end

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function resourceID(value)
	if value == "gold" or value == "GOLD" then
		return GOLD
	end
	if type(value) == "string" and value:sub(1, 9) == "resource." then
		return value:sub(10)
	end
	return value
end

local function resourceValue(resources, resType)
	resType = resourceID(resType)
	if not resources or resType == nil then
		return 0
	end
	if resources[0] == nil and resources[7] ~= nil and type(resType) == "number" then
		return resources[resType + 1] or 0
	end
	return resources[resType] or 0
end

local function resourceTable(resType, amount)
	return {
		[resourceID(resType)] = amount or 0
	}
end

local function tileKey(tile)
	if not tile then
		return nil
	end
	return tostring(tile.x) .. "," .. tostring(tile.y) .. "," .. tostring(tile.z)
end

local function settings(aiNk)
	return aiNk and aiNk.settings or {}
end

local function getFreeResources(aiNk)
	return call(aiNk, "getFreeResources")
		or aiNk and aiNk.freeResources
		or {}
end

local function missingResourcesNow(aiNk)
	return call(aiNk and aiNk.buildAnalyzer, "getMissingResourcesNow")
		or aiNk and aiNk.missingResourcesNow
		or {}
end

local function missingResourcesInTotal(aiNk)
	return call(aiNk and aiNk.buildAnalyzer, "getMissingResourcesInTotal")
		or aiNk and aiNk.missingResourcesInTotal
		or {}
end

local function dailyIncome(aiNk)
	return call(aiNk and aiNk.buildAnalyzer, "getDailyIncome")
		or aiNk and aiNk.dailyIncome
		or {}
end

local function objectType(object)
	return object and (object.ID or object.objectType or object.type or object.typeName)
end

local function ownerOf(object)
	return object and (object.tempOwner or object.owner)
end

local function playerID(aiNk)
	return aiNk and (aiNk.playerID or aiNk.player)
end

local function isOwnedByPlayer(object, aiNk)
	return ownerOf(object) ~= nil and ownerOf(object) == playerID(aiNk)
end

local function producedResource(object)
	return resourceID(object and (object.producedResource or object.resourceID or object.resource))
end

local function relationToHero(target, hero, aiNk)
	local targetOwner = target and (target.tempOwner or target.owner)
	local heroOwner = hero and (hero.tempOwner or hero.owner)
	local relation = call(aiNk and aiNk.callback, "getPlayerRelations", targetOwner, heroOwner)
		or call(aiNk and aiNk.cc, "getPlayerRelations", targetOwner, heroOwner)
	if relation ~= nil then
		return relation
	end
	if target and target.relationToHero then
		return target.relationToHero
	end
	if target and target.enemy then
		return "ENEMIES"
	end
	return targetOwner ~= nil and heroOwner ~= nil and targetOwner ~= heroOwner and "ENEMIES" or "SAME_PLAYER"
end

local function isEnemies(relation)
	return relation == "ENEMIES" or relation == 2
end

local function canAfford(resources, cost)
	for resType = 0, GOLD do
		if resourceValue(resources, resType) < resourceValue(cost, resType) then
			return false
		end
	end
	return true
end

local function canAffordCount(resources, cost, count)
	for resType = 0, GOLD do
		if resourceValue(resources, resType) < resourceValue(cost, resType) * (count or 1) then
			return false
		end
	end
	return true
end

local function stackCount(stack)
	return call(stack, "getCount") or stack and stack.count or 0
end

local function stackMarketValue(stack)
	local value = call(stack, "getMarketValue") or stack and stack.marketValue
	if value ~= nil then
		return value
	end

	local creature = stack and stack.creature or {}
	local cost = creature.fullRecruitCost or creature.cost or stack and stack.fullRecruitCost or {}
	if type(cost) == "number" then
		return cost
	end
	if type(cost) == "table" and cost.marketValue then
		return cost.marketValue
	end
	return RewardEvaluator.getResourcesGoldReward(cost)
end

local function stacks(army)
	if not army then
		return {}
	end
	if type(army.Slots) == "function" then
		return army:Slots()
	end
	return army.slots or army.stacks or {}
end

local function creatureValue(creature, key, fallback)
	return creature and (creature[key] or call(creature, key)) or fallback
end

local function entryCreature(entry)
	local ids = entry and (entry.creatures or entry.ids or entry[2])
	if ids then
		return ids[#ids]
	end
	return entry and (entry.creature or entry[1] and type(entry[1]) == "table" and entry[1] or nil)
end

local function creatureEntries(dwelling)
	local result = {}
	for _, entry in ipairs(dwelling and dwelling.creatures or {}) do
		local creature = entryCreature(entry)
		if creature then
			table.insert(result, {
				count = entry.count or entry.available or entry[1] or 0,
				creature = creature,
				growth = entry.growth or creature.growth or 0
			})
		end
	end
	return result
end

local function creatureAIValue(creature)
	return creatureValue(creature, "aiValue", 0)
		or creatureValue(creature, "value", 0)
		or 0
end

local function creatureLevel(creature)
	return creatureValue(creature, "level", 1) or 1
end

local function creatureGrowth(creature, fallback)
	return creatureValue(creature, "growth", fallback or 0) or fallback or 0
end

local function creatureCost(creature)
	return creature and (creature.fullRecruitCost or creature.cost) or {}
end

local function marketValue(cost)
	if type(cost) == "number" then
		return cost
	end
	if type(cost) == "table" and cost.marketValue then
		return cost.marketValue
	end
	return RewardEvaluator.getResourcesGoldReward(cost)
end

local function numericValue(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.num or value.id or value.value or value[1] or 0
	end
	return 0
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

local function bonusValue(bonus)
	return bonus and (bonus.val or bonus.value or bonus.amount) or 0
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

local function resourcePrice(resType)
	resType = subtypeValue(resType)
	if RESOURCE_PRICES[resType] ~= nil then
		return RESOURCE_PRICES[resType]
	end
	return RESOURCE_PRICES[resourceID(resType)] or 0
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

local function exportedBonuses(artifact)
	return call(artifact, "getExportedBonusList")
		or artifact and (artifact.exportedBonusList or artifact.exportedBonuses or artifact.bonuses)
		or {}
end

local function constituentArtifacts(artifact)
	return call(artifact, "getConstituents")
		or artifact and (artifact.constituents or artifact.parts)
		or {}
end

local function getDwellingArmyValue(aiNk, dwelling, checkGold)
	local score = 0
	local resources = getFreeResources(aiNk)
	for _, entry in ipairs(creatureEntries(dwelling)) do
		if entry.count > 0 then
			local creature = entry.creature
			local creaturesAreFree = creatureLevel(creature) == 1
			local cost = creatureCost(creature)
			if creaturesAreFree or not checkGold or canAffordCount(resources, cost, entry.count) then
				score = score + creatureAIValue(creature) * entry.count
			end
		end
	end
	return score
end

local function dwellingsAccumulateWhenOwned(aiNk)
	local configured = call(settings(aiNk), "getBoolean", "DWELLINGS_ACCUMULATE_WHEN_OWNED")
	if configured ~= nil then
		return configured
	end
	if settings(aiNk).dwellingsAccumulateWhenOwned ~= nil then
		return settings(aiNk).dwellingsAccumulateWhenOwned
	end
	return true
end

local function dayOfWeek(aiNk)
	local calendar = aiNk and aiNk.calendar or {}
	return calendar.dayOfWeek or 1
end

local function getDwellingArmyGrowth(aiNk, dwelling, hero)
	if ownerOf(dwelling) == ownerOf(hero) then
		return 0
	end

	local score = 0
	for _, entry in ipairs(creatureEntries(dwelling)) do
		local creature = entry.creature
		score = score + creatureAIValue(creature) * creatureGrowth(creature, entry.growth)
		if not dwellingsAccumulateWhenOwned(aiNk) then
			score = score * dayOfWeek(aiNk)
		end
	end
	return score
end

local function getDwellingArmyCost(dwelling)
	local cost = 0
	for _, entry in ipairs(creatureEntries(dwelling)) do
		if entry.count > 0 then
			local creature = entry.creature
			if creatureLevel(creature) ~= 1 then
				cost = cost + marketValue(creatureCost(creature)) * entry.count
			end
		end
	end
	return cost
end

local function estimateTownIncome(target, hero, aiNk)
	local relation = relationToHero(target, hero, aiNk)
	if not isEnemies(relation) then
		return 0
	end

	if target.estimatedIncome ~= nil then
		return target.estimatedIncome
	end

	local booster = target.controlledByAI and 1 or 2
	if target.hasCapitol then
		return booster * 2000
	end

	local fortLevel = target.fortLevel or 0
	if fortLevel == 3 or target.hasCastle then
		return booster * 750
	end
	if target.hasFort and target.owner ~= "NEUTRAL" then
		return booster * booster * 500
	end
	return booster * 250
end

local function developmentInfoEmpty(aiNk)
	if aiNk and aiNk.buildAnalyzer and type(aiNk.buildAnalyzer.getDevelopmentInfo) == "function" then
		local info = aiNk.buildAnalyzer:getDevelopmentInfo()
		return not info or #info == 0
	end
	return aiNk and aiNk.developmentInfoEmpty == true
end

local function dailyGoldIncome(target)
	local income = call(target, "dailyIncome") or target and target.dailyIncome or {}
	return resourceValue(income, GOLD)
end

local function enemyHeroStrategicalValue(target)
	if target.enemyStrategicalValue ~= nil then
		return target.enemyStrategicalValue
	end
	local objectValue = target.objectValueUnderThreat or 0
	local level = target.level or 1
	return math.min(1.5, objectValue * 0.9 + (1.5 - (1.5 / (1 + level))))
end

function RewardEvaluator.getArtifactBonusScoreImpl(bonus)
	local value = bonusValue(bonus)
	local type = bonusType(bonus)
	local subtype = bonusSubtype(bonus)

	if type == "MOVEMENT" then
		if isSubtype(subtype, "heroMovementLand", "HERO_MOVEMENT_LAND", "LAND", 0) then
			return value * 20
		end
		if isSubtype(subtype, "heroMovementSea", "HERO_MOVEMENT_SEA", "SEA", 1) then
			return value * 10
		end
		return 0
	elseif type == "STACKS_SPEED" then
		return value * 8000
	elseif type == "MORALE" then
		return value * 1500
	elseif type == "LUCK" then
		return value * 1000
	elseif type == "PRIMARY_SKILL" then
		return value * 1000
	elseif type == "SURRENDER_DISCOUNT" then
		return 0
	elseif type == "WATER_WALKING" then
		return 5000
	elseif type == "FREE_SHIP_BOARDING" then
		return 10000
	elseif type == "WHIRLPOOL_PROTECTION" then
		return 5000
	elseif type == "FLYING_MOVEMENT" then
		return 20000
	elseif type == "UNDEAD_RAISE_PERCENTAGE" then
		return value * 400
	elseif type == "GENERATE_RESOURCE" then
		return value * resourcePrice(subtype) * 10
	elseif type == "SPELL_DURATION" then
		return value * 200
	elseif type == "MAGIC_RESISTANCE" then
		return value * 400
	elseif type == "PERCENTAGE_DAMAGE_BOOST" then
		if isSubtype(subtype, "damageTypeRanged", "DAMAGE_TYPE_RANGED", "RANGED", 1) then
			return value * 200
		end
		if isSubtype(subtype, "damageTypeMelee", "DAMAGE_TYPE_MELEE", "MELEE", 0) then
			return value * 500
		end
		return 0
	elseif type == "CREATURE_GROWTH" then
		return (1 + numericValue(subtype)) * value * 400
	elseif type == "MANA_PERCENTAGE_REGENERATION" then
		return value * 150
	elseif type == "MANA_REGENERATION" then
		return value * 500
	elseif type == "SPELLS_OF_SCHOOL" then
		return 20000
	elseif type == "SPELLS_OF_LEVEL" then
		return numericValue(subtype) * 6000
	elseif type == "SPELL_DAMAGE" then
		return value * 120
	elseif type == "SIGHT_RADIUS" then
		return value * 1000
	elseif type == "LEARN_BATTLE_SPELL_CHANCE" or type == "LEARN_BATTLE_SPELL_CHANCE_PRE_BATTLE" then
		return 0
	elseif type == "STACK_HEALTH" then
		return value * 5000
	elseif type == "NO_DISTANCE_PENALTY" then
		return 10000
	elseif type == "NO_WALL_PENALTY" then
		return 5000
	end

	return 0
end

function RewardEvaluator.getArtifactBonusScore(bonus)
	if isBattleWideBonus(bonus) then
		if hasLimiter(bonus) then
			return -RewardEvaluator.getArtifactBonusScoreImpl(bonus)
		end
		return 0
	end
	return RewardEvaluator.getArtifactBonusScoreImpl(bonus)
end

function RewardEvaluator.getPotentialArtifactScore(artifact)
	if not artifact then
		return 0
	end
	if artifact.ID == "SPELL_SCROLL" or artifact.id == "SPELL_SCROLL" or artifact.spellScroll then
		return 1500
	end

	local totalScore = 0
	local sawBonus = false
	for _, bonus in ipairs(exportedBonuses(artifact)) do
		sawBonus = true
		totalScore = totalScore + RewardEvaluator.getArtifactBonusScore(bonus)
	end
	for _, part in ipairs(constituentArtifacts(artifact)) do
		for _, bonus in ipairs(exportedBonuses(part)) do
			sawBonus = true
			totalScore = totalScore + RewardEvaluator.getArtifactBonusScore(bonus)
		end
	end

	if sawBonus or artifact.price then
		return math.max((artifact.price or 0) / 5, totalScore)
	end
	return artifact.potentialScore or artifact.armyValue or artifact.score or 0
end

local function spellLevel(spell)
	return spell and (spell.level or spell.spellLevel or 1) or 1
end

local function canLearnSpell(hero, spell)
	if spell and spell.canLearn ~= nil then
		return spell.canLearn
	end
	if spell and spell.known ~= nil then
		return not spell.known
	end
	return hero and hero.canLearnSpells ~= false
end

local function heroHasSkill(hero, skill)
	local skills = hero and (hero.skills or hero.secSkills) or {}
	return skills[skill] ~= nil and skills[skill] ~= 0
end

local function heroSkillSlotsFull(hero)
	if hero and hero.skillSlotsFull ~= nil then
		return hero.skillSlotsFull
	end
	local skills = hero and (hero.skills or hero.secSkills) or {}
	local count = 0
	for _ in pairs(skills) do
		count = count + 1
	end
	return hero and hero.skillLimit and count >= hero.skillLimit or false
end

local function rewardArtifactsValue(reward)
	local value = 0
	for _, artifact in ipairs(reward.grantedArtifacts or reward.artifacts or {}) do
		value = value + RewardEvaluator.getPotentialArtifactScore(artifact)
	end
	for _ in ipairs(reward.grantedScrolls or reward.scrolls or {}) do
		value = value + 1500
	end
	return value
end

local function rewardCreaturesValue(reward)
	local value = 0
	for _, stack in ipairs(reward.creatures or {}) do
		value = value + creatureAIValue(stack.creature or stack.type or stack) * (stack.count or stack[1] or 0)
	end
	return value
end

local function resolveTargetHeroArmyCheck(selfOrTarget, maybeTarget, maybeHero, maybeArmy, maybeCheckGold, maybeAiNk)
	if selfOrTarget and selfOrTarget.aiNk ~= nil then
		return maybeTarget, maybeHero, maybeArmy, maybeCheckGold, maybeAiNk or selfOrTarget.aiNk
	end
	return selfOrTarget, maybeTarget, maybeHero, maybeArmy, maybeCheckGold
end

function RewardEvaluator.getResourcesGoldReward(resources)
	local result = 0
	for resType = 0, GOLD do
		local amount = resourceValue(resources, resType)
		if amount > 0 then
			result = result + (resType == GOLD and amount or amount * 100)
		end
	end
	return result
end

function RewardEvaluator.getNowResourceRequirementStrength(selfOrAiNk, resType)
	local aiNk = aiContext(selfOrAiNk)
	local requiredResources = missingResourcesNow(aiNk)
	local income = dailyIncome(aiNk)

	if resourceValue(requiredResources, resType) == 0 then
		return 0
	end
	if resourceValue(income, resType) == 0 then
		return 1.0
	end
	return 0.8
end

function RewardEvaluator.getTotalResourceRequirementStrength(selfOrAiNk, resType)
	local aiNk = aiContext(selfOrAiNk)
	local requiredResources = missingResourcesInTotal(aiNk)
	local income = dailyIncome(aiNk)

	if resourceValue(requiredResources, resType) == 0 then
		return 0
	end
	if resourceValue(income, resType) == 0 then
		return 1.0
	end
	return 0.8
end

function RewardEvaluator.getCombinedResourceRequirementStrength(selfOrAiNk, resources)
	local aiNk = aiContext(selfOrAiNk)
	local sum = 0.0
	for resType = 0, GOLD do
		if resourceValue(resources, resType) > 0 then
			local calculation = 0.5 * RewardEvaluator.getNowResourceRequirementStrength(aiNk, resType)
				+ 0.5 * RewardEvaluator.getTotalResourceRequirementStrength(aiNk, resType)
			sum = sum + math.min(MINIMUM_STRATEGICAL_VALUE_NON_TOWN, calculation)
		end
	end
	return sum
end

function RewardEvaluator.getArmyCost(selfOrArmy, maybeArmy)
	local army = maybeArmy or selfOrArmy
	if army and army.armyCost ~= nil then
		return army.armyCost
	end
	local value = 0
	for _, stack in pairs(stacks(army)) do
		value = value + stackMarketValue(stack) * stackCount(stack)
	end
	return value
end

function RewardEvaluator.getManaRecoveryArmyReward(selfOrHero, maybeHero)
	local hero = maybeHero or selfOrHero
	local magicStrength = hero and (hero.magicStrength or call(hero, "getMagicStrength")) or 0
	local mana = hero and (hero.mana or 0) or 0
	local manaLimit = hero and (hero.manaLimit or hero.maxMana or 1) or 1
	if manaLimit <= 0 then
		return 0
	end
	return magicStrength * 10000 * (1.0 - math.sqrt(mana / manaLimit))
end

function RewardEvaluator.townArmyGrowth(selfOrTown, maybeTown)
	local town = maybeTown or selfOrTown
	local result = 0
	for _, entry in ipairs(creatureEntries(town)) do
		local creature = entry.creature
		result = result + creatureAIValue(creature) * creatureGrowth(creature, entry.growth)
	end
	return result
end

local function townFaction(town)
	return call(town, "getFactionID") or town and (town.factionID or town.faction)
end

local function isBuiltForFaction(aiNk, town, buildingID)
	local faction = townFaction(town)
	local buildAnalyzer = aiNk and aiNk.buildAnalyzer
	local built = call(buildAnalyzer, "isBuilt", faction, buildingID)
	if built ~= nil then
		return built
	end
	built = aiNk and aiNk.builtBuildingsByFaction and aiNk.builtBuildingsByFaction[faction]
	if built ~= nil then
		return built[buildingID] == true
	end
	return false
end

local function totalCreaturesAvailable(aiNk, creatureID)
	local result = call(aiNk and aiNk.armyManager, "getTotalCreaturesAvailable", creatureID)
		or aiNk and aiNk.totalCreaturesAvailableByCreature and aiNk.totalCreaturesAvailableByCreature[creatureID]
		or {}
	return {
		count = result.count or result[1] or 0,
		power = result.power or result.armyPower or result[2] or 0
	}
end

local function evaluateStackPower(aiNk, creature, count)
	local value = call(aiNk and aiNk.armyManager, "evaluateStackPower", creature, count)
	if value ~= nil then
		return value
	end
	return creatureAIValue(creature) * (count or 0)
end

function RewardEvaluator.getUpgradeArmyReward(selfOrTown, maybeTown, maybeBuildingInfo, maybeAiNk)
	local town = selfOrTown
	local buildingInfo = maybeTown or {}
	local aiNk = maybeBuildingInfo
	if selfOrTown and selfOrTown.aiNk ~= nil then
		town = maybeTown
		buildingInfo = maybeBuildingInfo or {}
		aiNk = maybeAiNk or selfOrTown.aiNk
	end

	if buildingInfo.alreadyBuiltForFaction or isBuiltForFaction(aiNk, town, buildingInfo.id) then
		return 0
	end

	local creaturesToUpgrade = totalCreaturesAvailable(aiNk, buildingInfo.baseCreatureID)
	local upgradedCreature = buildingInfo.creature or buildingInfo.upgradedCreature or buildingInfo.creatureID or {
		aiValue = buildingInfo.creatureAIValue or buildingInfo.upgradedCreatureAIValue or buildingInfo.upgradedCreaturePower
	}
	local upgradedPower = buildingInfo.upgradedPower
		or evaluateStackPower(aiNk, upgradedCreature, creaturesToUpgrade.count)

	if creaturesToUpgrade.count > 0 or upgradedPower > 0 then
		return upgradedPower - creaturesToUpgrade.power
	end
	return buildingInfo.potentialUpgradeValue or buildingInfo.upgradeArmyReward or 0
end

function RewardEvaluator.getArmyReward(selfOrTarget, maybeTarget, maybeHero, maybeArmy, maybeCheckGold, maybeAiNk)
	local target, hero, army, checkGold, aiNk = resolveTargetHeroArmyCheck(
		selfOrTarget,
		maybeTarget,
		maybeHero,
		maybeArmy,
		maybeCheckGold,
		maybeAiNk)
	if not target then
		return 0
	end

	local id = objectType(target)
	if id == "HILL_FORT" then
		return target.upgradeValue or army and army.upgradeValue or 0
	elseif CREATURE_GENERATORS[id] then
		return getDwellingArmyValue(aiNk, target, checkGold)
	elseif id == "SPELL_SCROLL" then
		return 1500
	elseif id == "ARTIFACT" then
		return RewardEvaluator.getPotentialArtifactScore(target.artifact or target)
	elseif id == "HERO" then
		return isEnemies(relationToHero(target, hero, aiNk)) and 0.5 * (target.armyStrength or target.totalStrength or 0) or 0
	elseif id == "PANDORAS_BOX" then
		return 5000
	elseif id == "MAGIC_WELL" or id == "MAGIC_SPRING" then
		return RewardEvaluator.getManaRecoveryArmyReward(hero)
	end

	local totalValue = 0
	for _, reward in ipairs(target.rewards or {}) do
		totalValue = totalValue + rewardArtifactsValue(reward) + rewardCreaturesValue(reward)
	end
	return totalValue
end

function RewardEvaluator.getArmyGrowth(selfOrTarget, maybeTarget, maybeHero, maybeArmy, maybeAiNk)
	local target = selfOrTarget
	local hero = maybeTarget
	local aiNk = maybeArmy
	if selfOrTarget and selfOrTarget.aiNk ~= nil then
		target = maybeTarget
		hero = maybeHero
		aiNk = maybeAiNk or selfOrTarget.aiNk
	end
	if not target then
		return 0
	end
	if not isEnemies(relationToHero(target, hero, aiNk)) then
		return 0
	end

	local id = objectType(target)
	if id == "TOWN" then
		local fortLevel = target.fortLevel or 0
		local neutral = ownerOf(target) == nil or ownerOf(target) == "NEUTRAL"
		local booster = (target.controlledByAI or neutral) and 1 or 2
		if fortLevel < CITADEL then
			return target.hasFort and booster * 500 or 0
		end
		return booster * (fortLevel == CASTLE and 5000 or 2000)
	elseif CREATURE_GENERATORS[id] then
		return getDwellingArmyGrowth(aiNk, target, hero)
	end
	return 0
end

function RewardEvaluator.getGoldCost(selfOrTarget, maybeTarget, maybeHero, maybeArmy, maybeAiNk)
	local target = selfOrTarget
	local army = maybeHero
	local aiNk = maybeArmy
	if selfOrTarget and selfOrTarget.aiNk ~= nil then
		target = maybeTarget
		army = maybeHero
		aiNk = maybeAiNk or selfOrTarget.aiNk
	end
	if not target then
		return 0
	end

	if target.allowsResourceSkill or target.allowsTradeResourceSkill then
		return settings(aiNk).marketsUniversityGoldCost or UNIVERSITY_GOLD_COST
	end

	local id = objectType(target)
	if id == "HILL_FORT" then
		return target.upgradeCostGold or army and army.upgradeCostGold or 0
	elseif id == "SCHOOL_OF_MAGIC" or id == "SCHOOL_OF_WAR" then
		return 1000
	elseif CREATURE_GENERATORS[id] then
		return getDwellingArmyCost(target)
	end
	return 0
end

function RewardEvaluator.getGoldReward(selfOrTarget, maybeTarget, maybeHero, maybeAiNk)
	local target = selfOrTarget
	local hero = maybeTarget
	local aiNk = maybeHero
	if selfOrTarget and selfOrTarget.aiNk ~= nil then
		target = maybeTarget
		hero = maybeHero
		aiNk = maybeAiNk or selfOrTarget.aiNk
	end

	if not target then
		return 0
	end

	local id = objectType(target)
	if id == "RESOURCE" then
		return producedResource(target) == GOLD and 600 or 100
	elseif id == "TREASURE_CHEST" then
		return 1500
	elseif id == "WATER_WHEEL" then
		return 1000
	elseif id == "TOWN" then
		return 5 * estimateTownIncome(target, hero, aiNk)
	elseif id == "MINE" or id == "ABANDONED_MINE" then
		return 5 * (producedResource(target) == GOLD and 1000 or 75)
	elseif id == "PANDORAS_BOX" then
		return 2500
	elseif id == "PRISON" then
		return HERO_GOLD_COST
	elseif id == "HERO" then
		if isEnemies(relationToHero(target, hero, aiNk)) then
			return HERO_GOLD_COST / 2 + 0.2 * RewardEvaluator.getArmyCost(target)
		end
		return 0
	end

	local goldReward = 0
	for _, reward in ipairs(target.rewards or {}) do
		goldReward = goldReward + RewardEvaluator.getResourcesGoldReward(reward.resources)
	end
	return goldReward
end

function RewardEvaluator.getStrategicalValue(selfOrTarget, maybeTarget, maybeHero, maybeAiNk)
	local target = selfOrTarget
	local hero = maybeTarget
	local aiNk = maybeHero
	if selfOrTarget and selfOrTarget.aiNk ~= nil then
		target = maybeTarget
		hero = maybeHero
		aiNk = maybeAiNk or selfOrTarget.aiNk
	end
	if not target then
		return 0
	end

	local id = objectType(target)
	if id == "MINE" then
		return 1.0 + RewardEvaluator.getCombinedResourceRequirementStrength(aiNk, resourceTable(producedResource(target), target.producedQuantity or 1))
	elseif id == "RESOURCE" then
		return RewardEvaluator.getCombinedResourceRequirementStrength(aiNk, resourceTable(producedResource(target), target.amount or target.count or 1))
	elseif id == "TOWN" then
		if developmentInfoEmpty(aiNk) then
			return 10.0
		end
		if isOwnedByPlayer(target, aiNk) then
			return math.min(1.0, math.sqrt(RewardEvaluator.townArmyGrowth(target) / 40000.0))
				+ math.min(0.3, dailyGoldIncome(target) / 10000.0)
		end
		local booster = target.controlledByAI and 0.4 or 1.0
		local fortLevel = target.fortLevel or 0
		if target.hasCapitol then
			return booster * 1.5
		end
		if fortLevel < CITADEL then
			return booster * (target.hasFort and 1.0 or 0.8)
		end
		return booster * (fortLevel == CASTLE and 1.4 or 1.2)
	elseif id == "HERO" then
		return isEnemies(relationToHero(target, hero or { owner = playerID(aiNk) }, aiNk)) and enemyHeroStrategicalValue(target) or 0
	elseif id == "KEYMASTER" then
		return 0.6
	end

	local resourceReward = 0.0
	for _, reward in ipairs(target.rewards or {}) do
		resourceReward = resourceReward + RewardEvaluator.getCombinedResourceRequirementStrength(aiNk, reward.resources)
	end
	return resourceReward
end

function RewardEvaluator.getConquestValue(selfOrTarget, maybeTarget, maybeAiNk)
	local target = selfOrTarget
	local aiNk = maybeTarget
	if selfOrTarget and selfOrTarget.aiNk ~= nil then
		target = maybeTarget
		aiNk = maybeAiNk or selfOrTarget.aiNk
	end
	if not target or isOwnedByPlayer(target, aiNk) then
		return 0
	end

	local id = objectType(target)
	if id == "TOWN" then
		if developmentInfoEmpty(aiNk) then
			return 10.0
		end
		local fortLevel = target.fortLevel or 0
		if target.hasCapitol then
			return 1.5
		end
		if fortLevel < CITADEL then
			return target.hasFort and 1.0 or 0.8
		end
		return fortLevel == CASTLE and 1.4 or 1.2
	elseif id == "HERO" then
		return isEnemies(relationToHero(target, { owner = playerID(aiNk) }, aiNk)) and enemyHeroStrategicalValue(target) or 0
	end
	return 0
end

function RewardEvaluator.evaluateWitchHutSkillScore(selfOrHut, maybeHut, maybeHero, maybeRole)
	local hut = selfOrHut
	local hero = maybeHut
	local role = maybeHero
	if selfOrHut and selfOrHut.aiNk ~= nil then
		hut = maybeHut
		hero = maybeHero
		role = maybeRole
	end

	if hut and hut.wasVisited == false then
		return role == HERO_ROLE_SCOUT and 2 or 0
	end

	local skill = hut and (hut.gainedSkill or hut.secondarySkill)
	if skill and (heroHasSkill(hero, skill) or heroSkillSlotsFull(hero)) then
		return 0
	end

	local score = hut and (hut.skillScore or hut.secondarySkillScore) or 0
	if score >= 2 then
		return role == HERO_ROLE_MAIN and 10 or 4
	end
	return score
end

function RewardEvaluator.getSkillReward(selfOrTarget, maybeTarget, maybeHero, maybeRole, maybeAiNk)
	local target = selfOrTarget
	local hero = maybeTarget
	local role = maybeHero
	local aiNk = maybeRole
	if selfOrTarget and selfOrTarget.aiNk ~= nil then
		target = maybeTarget
		hero = maybeHero
		role = maybeRole
		aiNk = maybeAiNk or selfOrTarget.aiNk
	end
	if not target then
		return 0
	end

	local id = objectType(target)
	if id == "STAR_AXIS"
		or id == "SCHOLAR"
		or id == "SCHOOL_OF_MAGIC"
		or id == "SCHOOL_OF_WAR"
		or id == "GARDEN_OF_REVELATION"
		or id == "MARLETTO_TOWER"
		or id == "MERCENARY_CAMP"
		or id == "TREE_OF_KNOWLEDGE" then
		return 1
	elseif id == "LEARNING_STONE" then
		return 1.0 / math.sqrt(hero and hero.level or 1)
	elseif id == "ARENA" then
		return 2
	elseif id == "SHRINE_OF_MAGIC_INCANTATION" then
		return 0.25
	elseif id == "SHRINE_OF_MAGIC_GESTURE" then
		return 1.0
	elseif id == "SHRINE_OF_MAGIC_THOUGHT" then
		return 2.0
	elseif id == "LIBRARY_OF_ENLIGHTENMENT" then
		return 8
	elseif id == "WITCH_HUT" then
		return RewardEvaluator.evaluateWitchHutSkillScore(target, hero, role)
	elseif id == "PANDORAS_BOX" then
		return 2.5
	elseif id == "HERO" then
		return isEnemies(relationToHero(target, hero or { owner = playerID(aiNk) }, aiNk)) and 0.5 * (target.level or 0) or 0
	end

	local totalValue = 0.0
	for _, reward in ipairs(target.rewards or {}) do
		local spells = reward.spells or {}
		if #spells > 0 then
			local rewardValue = 0.0
			for _, spell in ipairs(spells) do
				if canLearnSpell(hero, spell) then
					rewardValue = rewardValue + math.sqrt(spellLevel(spell)) / 4.0
				end
			end
			totalValue = totalValue + rewardValue / #spells
		end

		for _, value in ipairs(reward.primary or {}) do
			totalValue = totalValue + value
		end
	end

	return totalValue
end

function RewardEvaluator.getEnemyHeroDanger(selfOrTile, maybeTile, maybeTurn, maybeAiNk)
	local tile = selfOrTile
	local turn = maybeTile or 0
	local aiNk = maybeTurn
	if selfOrTile and selfOrTile.aiNk ~= nil then
		tile = maybeTile
		turn = maybeTurn or 0
		aiNk = maybeAiNk or selfOrTile.aiNk
	end

	local threatNode = call(aiNk and aiNk.dangerHitMap, "getTileThreat", tile)
		or aiNk and aiNk.enemyHeroDangerByTile and aiNk.enemyHeroDangerByTile[tileKey(tile)]
		or tile and tile.enemyHeroDanger
		or {}
	local maximumDanger = threatNode.maximumDanger or threatNode.maximum or threatNode
	local fastestDanger = threatNode.fastestDanger or threatNode.fastest or {}

	if (maximumDanger.danger or 0) == 0 then
		return {
			danger = 0,
			threat = 0,
			turn = math.huge
		}
	end
	if (maximumDanger.turn or 0) <= turn then
		return maximumDanger
	end
	if (fastestDanger.turn or math.huge) <= turn then
		return fastestDanger
	end
	return {
		danger = 0,
		threat = 0,
		turn = math.huge
	}
end

RewardEvaluator.GOLD = GOLD
RewardEvaluator.HERO_GOLD_COST = HERO_GOLD_COST
RewardEvaluator.MINIMUM_STRATEGICAL_VALUE_NON_TOWN = MINIMUM_STRATEGICAL_VALUE_NON_TOWN
RewardEvaluator.HERO_ROLE_SCOUT = HERO_ROLE_SCOUT
RewardEvaluator.HERO_ROLE_MAIN = HERO_ROLE_MAIN

return RewardEvaluator
