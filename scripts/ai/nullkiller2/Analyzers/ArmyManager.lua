-- Mirrors AI/Nullkiller2/Analyzers/ArmyManager.{h,cpp}: SlotInfo and best-army selection.

local ArmyManager = {}

ArmyManager.ARMY_SIZE = 7

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	if type(value) == "number" or type(value) == "string" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function armySlots(army)
	return call(army, "Slots") or army and (army.slots or army.stacks) or {}
end

local function stackCreature(stack)
	return type(stack) == "table" and (stack.creature or stack.creatureID or stack.creID or stack.type or stack) or stack
end

local function creatureID(value)
	if type(value) == "table" and value.creatureID ~= nil then
		return objectID(value.creatureID)
	end
	if type(value) == "table" and value.creID ~= nil then
		return objectID(value.creID)
	end
	if type(value) == "table" and value.creature ~= nil then
		return objectID(value.creature)
	end
	return objectID(value)
end

local function stackCount(stack)
	return type(stack) == "table" and (stack.count or stack.quantity or 0) or 0
end

local function stackPower(stack)
	if type(stack) ~= "table" then
		return 0
	end
	if stack.power then
		return stack.power
	end

	local creature = stackCreature(stack)
	local aiValue = type(creature) == "table" and (creature.aiValue or creature.value) or stack.aiValue or 0
	return aiValue * stackCount(stack)
end

local function factionID(creature)
	if type(creature) ~= "table" then
		return nil
	end
	return objectID(creature.factionID or creature.faction or creature.alignment)
end

local function creatureLevel(creature)
	return type(creature) == "table" and (creature.level or creature.creatureLevel or 0) or 0
end

local function movementRange(creature)
	return type(creature) == "table" and (creature.movementRange or creature.speed or creature.moveRange or 0) or 0
end

local function stacksCount(army)
	local result = call(army, "stacksCount")
	if result ~= nil then
		return result
	end
	return army and (army.stacksCount or #armySlots(army)) or 0
end

local function setting(context, ...)
	local settings = context and context.settings or {}
	for _, key in ipairs({ ... }) do
		local result = call(settings, "getInteger", key)
		if result ~= nil then
			return result
		end
		if type(settings) == "table" and settings[key] ~= nil then
			return settings[key]
		end
	end
	return nil
end

local function settingVector(context, ...)
	local settings = context and context.settings or {}
	for _, key in ipairs({ ... }) do
		local result = call(settings, "getVector", key)
		if result ~= nil then
			return result
		end
		if type(settings) == "table" and settings[key] ~= nil then
			return settings[key]
		end
	end
	return nil
end

local function vectorValue(vector, zeroBasedIndex)
	if type(vector) ~= "table" then
		return nil
	end
	if vector[0] ~= nil then
		return vector[zeroBasedIndex]
	end
	return vector[zeroBasedIndex + 1]
end

local function vectorLength(vector)
	if type(vector) ~= "table" then
		return 0
	end
	local result = #vector
	for key in pairs(vector) do
		if type(key) == "number" and key >= 0 then
			result = math.max(result, key + 1)
		end
	end
	return result
end

local function moraleValue(slot, army, context)
	local direct = slot.morale
		or type(slot.creature) == "table" and slot.creature.morale
	if direct ~= nil then
		return direct
	end

	local id = creatureID(slot.creature)
	local byCreature = context and context.moraleByCreatureID
	if byCreature and byCreature[id] ~= nil then
		return byCreature[id]
	end
	if byCreature and byCreature[tostring(id)] ~= nil then
		return byCreature[tostring(id)]
	end

	local evaluator = context and (context.moraleEvaluator or context.getMorale)
	if type(evaluator) == "function" then
		return evaluator(slot, army, context)
	end
	return 0
end

local function moraleMultiplier(slot, army, context)
	local morale = moraleValue(slot, army, context)
	local multiplier = 1.0

	if morale < 0 then
		local chance = settingVector(context, "COMBAT_BAD_MORALE_CHANCE", "combatBadMoraleChance", "badMoraleChance")
		local length = vectorLength(chance)
		if length > 0 then
			local index = math.min(length, -morale) - 1
			local dice = setting(context, "COMBAT_MORALE_DICE_SIZE", "combatMoraleDiceSize", "moraleDiceSize") or 1
			multiplier = multiplier - 1.0 / dice * (vectorValue(chance, index) or 0)
		end
	elseif morale > 0 then
		local chance = settingVector(context, "COMBAT_GOOD_MORALE_CHANCE", "combatGoodMoraleChance", "highMoraleChance")
		local length = vectorLength(chance)
		if length > 0 then
			local index = math.min(length, morale) - 1
			local dice = setting(context, "COMBAT_MORALE_DICE_SIZE", "combatMoraleDiceSize", "moraleDiceSize") or 1
			multiplier = multiplier + 1.0 / dice * (vectorValue(chance, index) or 0)
		end
	end

	return multiplier
end

local function needsLastStack(army)
	local result = call(army, "needsLastStack")
	if result ~= nil then
		return result
	end
	return army and army.needsLastStack == true
end

local function terrainID(terrain)
	return objectID(terrain and (terrain.id or terrain.terrainID or terrain.type or terrain))
end

local function isNativeTerrain(creature, terrain)
	local terrainKey = terrainID(terrain)
	if terrainKey == nil or type(creature) ~= "table" then
		return false
	end

	local native = creature.nativeTerrain or creature.nativeTerrains
	if type(native) == "table" then
		return native[terrainKey] == true or native[tostring(terrainKey)] == true
	end

	local faction = creature.faction or creature.factionInfo
	native = type(faction) == "table" and (faction.nativeTerrain or faction.nativeTerrains) or nil
	return type(native) == "table" and (native[terrainKey] == true or native[tostring(terrainKey)] == true)
end

local function movementPointsLimit(creature, context)
	if type(creature) == "table" and creature.movementPointsLimit then
		return creature.movementPointsLimit
	end

	local movementLimits = settingVector(context, "HEROES_MOVEMENT_POINTS_LAND", "heroesMovementPointsLand", "movementPointsLimits")
	local length = vectorLength(movementLimits)
	if length == 0 then
		return movementRange(creature)
	end

	local effectiveMovement = math.min(length - 1, movementRange(creature))
	return vectorValue(movementLimits, effectiveMovement) or 0
end

function ArmyManager.getSortedSlots(target, source)
	local byCreature = {}
	for _, army in ipairs({ target, source }) do
		for _, stack in pairs(armySlots(army)) do
			local creature = stackCreature(stack)
			local id = creatureID(creature)
			if id ~= nil then
				local slotInfo = byCreature[id]
				if not slotInfo then
					slotInfo = {
						creature = creature,
						creatureID = id,
						count = 0,
						power = 0
					}
					byCreature[id] = slotInfo
				end
				slotInfo.count = slotInfo.count + stackCount(stack)
				slotInfo.power = slotInfo.power + stackPower(stack)
			end
		end
	end

	local result = {}
	for _, slot in pairs(byCreature) do
		table.insert(result, slot)
	end
	table.sort(result, function(left, right)
		if left.power ~= right.power then
			return left.power > right.power
		end
		return tostring(left.creatureID) < tostring(right.creatureID)
	end)
	return result
end

function ArmyManager.getBestUnitForScout(army, armyTerrain, context)
	if #army == 0 then
		return nil, nil
	end

	local totalPower = 0
	for _, unit in ipairs(army) do
		totalPower = totalPower + unit.power
	end

	local baseMovementCost = setting(context, "HEROES_MOVEMENT_COST_BASE", "heroesMovementCostBase", "baseMovementCost") or 100
	local terrainHasPenalty = armyTerrain and (armyTerrain.hasPenalty == true or armyTerrain.moveCost ~= nil and armyTerrain.moveCost ~= baseMovementCost)
	local maxUnitValue = totalPower / 100

	local best = army[1]
	local bestIndex = 1
	for index = 2, #army do
		local candidate = army[index]
		local leftCreature = candidate.creature
		local rightCreature = best.creature
		local leftUnitPower = candidate.count == 0 and 0 or candidate.power / candidate.count
		local rightUnitPower = best.count == 0 and 0 or best.power / best.count
		local leftUnitIsWeak = leftUnitPower < maxUnitValue or creatureLevel(leftCreature) < 4
		local rightUnitIsWeak = rightUnitPower < maxUnitValue or creatureLevel(rightCreature) < 4
		local candidateIsBetter = false

		if leftUnitIsWeak ~= rightUnitIsWeak then
			candidateIsBetter = leftUnitIsWeak
		elseif terrainHasPenalty and isNativeTerrain(leftCreature, armyTerrain) ~= isNativeTerrain(rightCreature, armyTerrain) then
			candidateIsBetter = isNativeTerrain(leftCreature, armyTerrain)
		else
			local leftMovement = movementPointsLimit(leftCreature, context)
			local rightMovement = movementPointsLimit(rightCreature, context)
			if leftMovement ~= rightMovement then
				candidateIsBetter = leftMovement > rightMovement
			else
				candidateIsBetter = leftUnitPower < rightUnitPower
			end
		end

		if candidateIsBetter then
			best = candidate
			bestIndex = index
		end
	end

	return best, bestIndex
end

function ArmyManager.getBestArmy(armyCarrier, target, source, context)
	local sortedSlots = ArmyManager.getSortedSlots(target, source)
	if stacksCount(source) == 0 then
		return sortedSlots
	end

	local alignmentMap = {}
	local alignmentCount = 0
	for _, slot in ipairs(sortedSlots) do
		local faction = factionID(slot.creature) or "__none"
		if alignmentMap[faction] == nil then
			alignmentMap[faction] = 0
			alignmentCount = alignmentCount + 1
		end
		alignmentMap[faction] = alignmentMap[faction] + slot.power
	end

	local allowedFactions = {}
	local allowedCount = 0
	local resultingArmy = {}
	local armyValue = 0
	local armySize = context and context.armySize or ArmyManager.ARMY_SIZE

	while allowedCount < alignmentCount do
		local strongestFaction = nil
		local strongestPower = nil
		for faction, power in pairs(alignmentMap) do
			if not allowedFactions[faction] and (strongestPower == nil or power > strongestPower) then
				strongestFaction = faction
				strongestPower = power
			end
		end

		if strongestFaction == nil then
			break
		end

		allowedFactions[strongestFaction] = true
		allowedCount = allowedCount + 1

		local newArmy = {}
		local newValue = 0
		for _, slot in ipairs(sortedSlots) do
			if allowedFactions[factionID(slot.creature) or "__none"] and #newArmy < armySize then
				table.insert(newArmy, {
					creature = slot.creature,
					creatureID = slot.creatureID,
					count = slot.count,
					power = slot.power,
					morale = slot.morale
				})
			end
		end

		for _, slot in ipairs(newArmy) do
			newValue = newValue + moraleMultiplier(slot, newArmy, context) * slot.power
		end

		if armyValue >= newValue then
			break
		end

		resultingArmy = newArmy
		armyValue = newValue
	end

	if #resultingArmy <= armySize and allowedCount == alignmentCount and needsLastStack(source) then
		local weakest, index = ArmyManager.getBestUnitForScout(resultingArmy, context and context.terrain, context)
		if weakest then
			if weakest.count == 1 then
				table.remove(resultingArmy, index)
			else
				weakest.power = weakest.power - weakest.power / weakest.count
				weakest.count = weakest.count - 1
			end
		end
	end

	return resultingArmy
end

return ArmyManager
