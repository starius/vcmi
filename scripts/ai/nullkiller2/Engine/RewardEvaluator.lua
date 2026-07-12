-- Mirrors AI/Nullkiller2/Engine/PriorityEvaluator.{h,cpp}: RewardEvaluator helpers.

local RewardEvaluator = {}

local GOLD = 6
local HERO_GOLD_COST = 2500
local MINIMUM_STRATEGICAL_VALUE_NON_TOWN = 0.3

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

function RewardEvaluator.getNowResourceRequirementStrength(aiNk, resType)
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

function RewardEvaluator.getTotalResourceRequirementStrength(aiNk, resType)
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

function RewardEvaluator.getCombinedResourceRequirementStrength(aiNk, resources)
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

function RewardEvaluator.getArmyCost(army)
	local value = 0
	for _, stack in pairs(stacks(army)) do
		value = value + stackMarketValue(stack) * stackCount(stack)
	end
	return value
end

function RewardEvaluator.getGoldReward(target, hero, aiNk)
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

RewardEvaluator.GOLD = GOLD
RewardEvaluator.HERO_GOLD_COST = HERO_GOLD_COST
RewardEvaluator.MINIMUM_STRATEGICAL_VALUE_NON_TOWN = MINIMUM_STRATEGICAL_VALUE_NON_TOWN

return RewardEvaluator
