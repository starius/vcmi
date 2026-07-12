-- Mirrors AI/Nullkiller2/Helpers/ArmyFormation.{h,cpp}: siege and whirlpool army reshaping.

local ArmyFormation = {}

ArmyFormation.FortLevel = {
	NONE = 0,
	FORT = 1,
	CITADEL = 2,
	CASTLE = 3
}

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function armySize(army)
	return army and (army.armySize or army.ARMY_SIZE) or 7
end

local function armySlots(army)
	return call(army, "Slots") or army and (army.slots or army.stacks) or {}
end

local function stackSlot(stack, fallback)
	return type(stack) == "table" and (stack.slot or fallback) or fallback
end

local function stackCount(stack)
	return type(stack) == "table" and (stack.count or stack.amount or 0) or 0
end

local function stackCreature(stack)
	return type(stack) == "table" and (stack.creature or stack.creatureType or stack) or stack
end

local function creatureID(stack)
	local creature = stackCreature(stack)
	if type(creature) == "table" then
		return creature.id or creature.creatureID or creature.typeID or creature[1]
	end
	return creature
end

local function creatureAIValue(stack)
	local creature = stackCreature(stack)
	if type(stack) == "table" and stack.aiValue ~= nil then
		return stack.aiValue
	end
	if type(creature) == "table" then
		return creature.aiValue or creature.AIValue or creature.value or 0
	end
	return 0
end

local function creatureFlying(stack)
	local creature = stackCreature(stack)
	if type(stack) == "table" and stack.flying ~= nil then
		return stack.flying == true
	end
	if type(creature) == "table" then
		return creature.flying == true or creature.hasFlyingBonus == true
	end
	return false
end

local function sameCreature(lhs, rhs)
	return creatureID(lhs) ~= nil and creatureID(lhs) == creatureID(rhs)
end

local function stackAtSlot(army, targetSlot)
	for key, stack in pairs(armySlots(army)) do
		if stackSlot(stack, key) == targetSlot then
			return stack, key
		end
	end
	return nil, nil
end

local function freeSlots(army)
	local explicit = army and army.freeSlots
	if explicit then
		local result = {}
		for _, slot in ipairs(explicit) do
			table.insert(result, slot)
		end
		table.sort(result)
		return result
	end

	local result = {}
	for slot = 0, armySize(army) - 1 do
		if not stackAtSlot(army, slot) then
			table.insert(result, slot)
		end
	end
	return result
end

local function cloneSingleStack(stack, slot)
	local result = {}
	for key, value in pairs(stack or {}) do
		result[key] = value
	end
	result.slot = slot
	result.count = 1
	return result
end

local function insertStack(army, stack)
	local slots = armySlots(army)
	table.insert(slots, stack)
	if army then
		army.slots = slots
		army.stacksCount = (army.stacksCount or 0) + 1
	end
end

local function applySplit(army, sourceStack, destinationSlot)
	if type(sourceStack) ~= "table" then
		return
	end

	sourceStack.count = stackCount(sourceStack) - 1
	insertStack(army, cloneSingleStack(sourceStack, destinationSlot))
end

local function applySwap(army, firstSlot, secondSlot)
	local firstStack = stackAtSlot(army, firstSlot)
	local secondStack = stackAtSlot(army, secondSlot)
	if firstStack then
		firstStack.slot = secondSlot
	end
	if secondStack then
		secondStack.slot = firstSlot
	end
end

local function weakestSplittableStack(army)
	local weakest = nil
	local weakestValue = math.huge

	for key, stack in pairs(armySlots(army)) do
		local value = stackCount(stack) == 1 and math.huge or creatureAIValue(stack)
		if value < weakestValue then
			weakest = stack
			weakest.slot = stackSlot(stack, key)
			weakestValue = value
		end
	end

	if weakest and stackCount(weakest) > 1 then
		return weakest
	end
	return nil
end

function ArmyFormation.addSingleCreatureStacks(aiGw, hero)
	local slots = freeSlots(hero)

	while #slots > 0 do
		local weakest = weakestSplittableStack(hero)
		if not weakest then
			break
		end

		local destinationSlot = slots[#slots]
		table.remove(slots)
		aiGw:splitStack(hero, hero, stackSlot(weakest), destinationSlot, 1)
		applySplit(hero, weakest, destinationSlot)
	end
end

function ArmyFormation.rearrangeArmyForWhirlpool(aiGw, hero)
	ArmyFormation.addSingleCreatureStacks(aiGw, hero)
end

local function sortedStacksForSiege(hero)
	local result = {}
	for key, stack in pairs(armySlots(hero)) do
		stack.slot = stackSlot(stack, key)
		table.insert(result, stack)
	end

	table.sort(result, function(lhs, rhs)
		local flyingDiff = (creatureFlying(lhs) and 1 or 0) - (creatureFlying(rhs) and 1 or 0)
		if flyingDiff ~= 0 then
			return flyingDiff < 0
		end
		return creatureAIValue(lhs) < creatureAIValue(rhs)
	end)

	return result
end

local function fortLevel(town)
	local value = call(town, "fortLevel")
	if value ~= nil then
		return value
	end
	return town and (town.fortLevel or town.fortLevelValue) or ArmyFormation.FortLevel.NONE
end

function ArmyFormation.rearrangeArmyForSiege(aiGw, town, attacker)
	ArmyFormation.addSingleCreatureStacks(aiGw, attacker)

	if fortLevel(town) <= ArmyFormation.FortLevel.FORT then
		return
	end

	for index, stack in ipairs(sortedStacksForSiege(attacker)) do
		local targetSlot = index - 1
		local currentSlot = stackSlot(stack)
		if currentSlot ~= targetSlot then
			aiGw:swapCreatures(attacker, attacker, targetSlot, currentSlot)
			applySwap(attacker, targetSlot, currentSlot)
		end
	end
end

ArmyFormation._helpers = {
	freeSlots = freeSlots,
	sortedStacksForSiege = sortedStacksForSiege,
	sameCreature = sameCreature
}

return ArmyFormation
