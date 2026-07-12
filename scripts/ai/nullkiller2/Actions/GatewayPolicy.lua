-- Mirrors decision policy embedded in native gateway source.
--
-- The host owns event delivery and command execution. This module owns the
-- business choices made by those callbacks so they can be regression-tested
-- and compared against native parity snapshots.

local AIUtility = require("Helpers.AIUtility")
local PriorityEvaluator = require("Engine.PriorityEvaluator")

local GatewayPolicy = {}

local ComponentType = {
	RESOURCE = "RESOURCE"
}

local ObjectType = {
	ARTIFACT = "ARTIFACT",
	BORDERGUARD = "BORDERGUARD",
	HERO = "HERO",
	QUEST_GUARD = "QUEST_GUARD",
	RESOURCE = "RESOURCE"
}

local TeleportPassability = {
	IMPASSABLE = "IMPASSABLE"
}

local RESOURCE_COUNT = 7
local MACH4 = 16
local BACKPACK_START = 19

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function objectType(value)
	if not value then
		return nil
	end
	return value.ID or value.objectType or value.type or value.typeName
end

local function componentType(value)
	if not value then
		return nil
	end
	return value.type or value.componentType
end

local function isType(value, expected)
	local actual = objectType(value)
	if actual == expected then
		return true
	end
	return type(actual) == "string" and actual:upper() == expected
end

local function isComponent(value, expected)
	local actual = componentType(value)
	if actual == expected then
		return true
	end
	return type(actual) == "string" and actual:upper() == expected
end

local function tileValid(tile)
	if not tile then
		return false
	end
	if tile.valid ~= nil then
		return tile.valid
	end
	if type(tile.isValid) == "function" then
		return tile:isValid()
	end
	return tile.x ~= nil and tile.y ~= nil and tile.z ~= nil and tile.x >= 0 and tile.y >= 0 and tile.z >= 0
end

local function sameTile(lhs, rhs)
	if not lhs or not rhs then
		return false
	end
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

local function heroVerified(hero)
	if not hero then
		return false
	end
	if hero.verified ~= nil then
		return hero.verified
	end
	return call(hero, "isVerified") ~= false
end

local function heroStrength(hero)
	return call(hero, "getTotalStrength")
		or hero and (hero.totalStrength or hero.armyStrength or hero.strength)
		or 0
end

local function safeAttackRatio(settings)
	return call(settings, "getSafeAttackRatio")
		or settings and settings.safeAttackRatio
		or 1.1
end

local function heroRole(context, hero)
	return call(context and context.heroManager, "getHeroRoleOrDefault", hero)
		or call(context and context.heroManager, "getHeroRoleOrDefaultInefficient", hero)
		or hero and hero.role
		or PriorityEvaluator.HeroRole.SCOUT
end

local function goldPressureOverMax(context)
	local result = call(context and context.buildAnalyzer, "isGoldPressureOverMax")
	if result ~= nil then
		return result
	end
	return context and context.goldPressureOverMax == true
end

local function dangerAt(context, tile, hero)
	local result = call(context and context.dangerEvaluator, "evaluateDanger", tile, hero)
	if result ~= nil then
		return result
	end
	return context and context.dangerAtTarget or context and context.danger or 0
end

local function objectDanger(context, object)
	local dangers = context and context.objectDanger or {}
	local id = objectID(object)
	local result = dangers[id]
	if result ~= nil then
		return result
	end
	result = call(context and context.dangerEvaluator, "evaluateDanger", object)
	if result ~= nil then
		return result
	end
	return object and (object.danger or object.objectDanger) or 0
end

local function objectsAtTarget(context)
	local result = call(context and context.callback, "getVisitableObjs", context and context.target)
	if result ~= nil then
		return result
	end
	return context and (context.objects or context.visitableObjects) or {}
end

local function topVisitedObject(objects, hero)
	local first = objects[1]
	if first and objectID(first) == objectID(hero) then
		return objects[2] or first
	end
	return first
end

local function containsID(items, id)
	for _, item in ipairs(items or {}) do
		if objectID(item) == id then
			return true
		end
	end
	return false
end

local function exitID(exit)
	return objectID(exit and (exit.id or exit.objectID or exit.objectId or exit[1] or exit.exitID))
end

local function exitPos(exit)
	return exit and (exit.pos or exit.tile or exit[2])
end

local function exitVisible(context, exit)
	if exit and exit.visible ~= nil then
		return exit.visible
	end
	local id = exitID(exit)
	local visible = context and context.visibleExits
	if visible and visible[id] ~= nil then
		return visible[id]
	end
	local obj = call(context and context.callback, "getObj", id, false)
	return obj ~= nil
end

local function resourceAmount(resources, resourceID)
	if type(resources) == "number" then
		return resources
	end
	if type(resources) == "table" then
		if resourceID == "gold" or resourceID == "GOLD" or resourceID == 6 then
			if resources[0] == nil and resources[7] ~= nil then
				return resources[7] or 0
			end
			return resources[6] or resources.gold or resources.GOLD or 0
		end
		return resources[resourceID] or resources.gold or resources.GOLD or resources[1] or 0
	end
	return 0
end

local function resourceVectorAmount(resources, resourceID)
	if type(resources) == "number" then
		return resourceID == 6 and resources or 0
	end
	if type(resources) == "table" then
		if resources[0] == nil and resources[7] ~= nil then
			return resources[resourceID + 1] or 0
		end
		if resourceID == 6 then
			return resources[6] or resources.gold or resources.GOLD or 0
		end
		return resources[resourceID] or resources[tostring(resourceID)] or 0
	end
	return 0
end

local function maxAffordableCount(resources, cost)
	if type(cost) == "number" then
		if cost <= 0 then
			return math.huge
		end
		return math.floor(resourceAmount(resources, "gold") / cost)
	end

	local result = math.huge
	for resourceID = 0, RESOURCE_COUNT - 1 do
		local needed = resourceVectorAmount(cost, resourceID)
		if needed > 0 then
			result = math.min(result, math.floor(resourceVectorAmount(resources, resourceID) / needed))
		end
	end
	return result
end

local function canAfford(resources, cost)
	if type(resources) == "table" and type(resources.canAfford) == "function" then
		return resources:canAfford(cost)
	end
	for res, amount in pairs(cost or {}) do
		if resourceAmount(resources, res) < amount then
			return false
		end
	end
	return true
end

local function multiplyCost(cost, count)
	if type(cost) == "number" then
		return { gold = cost * count }
	end
	local result = {}
	for res, amount in pairs(cost or {}) do
		result[res] = amount * count
	end
	return result
end

function GatewayPolicy.chooseBlockingDialogAnswer(context)
	local answer = true
	local hero = context and context.hero
	local target = context and context.target
	local objects = objectsAtTarget(context)

	if heroVerified(hero) and tileValid(target) and #objects > 0 then
		local topObj = topVisitedObject(objects, hero)
		local goalObjectID = objectID(context.goalObject or context.goalObjectID or context.targetObject)
		local danger = dangerAt(context, target, hero)
		local strength = heroStrength(hero)
		local ratio = strength == 0 and math.huge or danger / strength

		answer = true
		if objectID(topObj) ~= goalObjectID and objectDanger(context, topObj) > 0 then
			answer = false
		end

		if isType(topObj, ObjectType.BORDERGUARD) or isType(topObj, ObjectType.QUEST_GUARD) then
			answer = true
		elseif isType(topObj, ObjectType.ARTIFACT) or isType(topObj, ObjectType.RESOURCE) then
			local dangerUnknown = danger == 0
			local dangerTooHigh = ratio * safeAttackRatio(context.settings) > 1
			answer = not dangerUnknown and not dangerTooHigh
		end
	end

	return answer and 1 or 0
end

function GatewayPolicy.chooseBlockingDialogSelection(context)
	local components = context and context.components or {}
	local selection = 0

	if context and context.selection then
		selection = #components
	end

	local hero = context and context.hero
	if heroVerified(hero)
		and #components == 2
		and isComponent(components[1], ComponentType.RESOURCE)
		and (heroRole(context, hero) ~= PriorityEvaluator.HeroRole.MAIN or goldPressureOverMax(context)) then
		selection = 1
	end

	return selection
end

function GatewayPolicy.chooseTeleportExit(context)
	local chosenExit = -1
	local probingList = {}
	for _, item in ipairs(context and context.teleportChannelProbingList or {}) do
		table.insert(probingList, objectID(item))
	end

	local passability = nil
	if context and context.impassable then
		passability = TeleportPassability.IMPASSABLE
	else
		local destinationTeleport = objectID(context and context.destinationTeleport)
		local destinationTeleportPos = context and context.destinationTeleportPos

		if destinationTeleport and tileValid(destinationTeleportPos) then
			for index, exit in ipairs(context.exits or {}) do
				if exitID(exit) == destinationTeleport and sameTile(exitPos(exit), destinationTeleportPos) then
					chosenExit = index - 1
					break
				end
			end
		end

		for index, exit in ipairs(context and context.exits or {}) do
			local id = exitID(exit)
			if context.channelProbing and id == destinationTeleport then
				chosenExit = index - 1
				break
			elseif not exitVisible(context, exit) and not containsID(probingList, id) then
				if id ~= destinationTeleport then
					table.insert(probingList, id)
				end
			end
		end
	end

	return {
		selection = chosenExit,
		teleportChannel = context and context.channel,
		passability = passability,
		teleportChannelProbingList = probingList
	}
end

function GatewayPolicy.chooseMapObjectSelection(context)
	return objectID(context and context.selectedObject) or 0
end

function GatewayPolicy.makeSurrenderRetreatDecision(context)
	local battleState = context and context.battleState or context or {}
	local ourHero = battleState.ourHero
	if ourHero and ourHero.patrol and ourHero.patrol.patrolling then
		return nil
	end

	local ourStrength = call(battleState, "getOurStrength") or battleState.ourStrength or 0
	local enemyStrength = call(battleState, "getEnemyStrength") or battleState.enemyStrength or 0
	local fightRatio = enemyStrength == 0 and math.huge or ourStrength / enemyStrength
	local settings = context and context.settings or battleState.settings or {}
	local townsCount = context and (context.townsCount or #(context.townsInfo or {})) or battleState.townsCount or 0

	local retreatThresholdAbsolute = call(settings, "getRetreatThresholdAbsolute")
		or settings.retreatThresholdAbsolute
		or 0
	local retreatThresholdRelative = call(settings, "getRetreatThresholdRelative")
		or settings.retreatThresholdRelative
		or 0

	if townsCount > 0
		and ourStrength < retreatThresholdAbsolute
		and fightRatio < retreatThresholdRelative
		and battleState.canFlee then
		return {
			action = "retreat",
			side = battleState.ourSide
		}
	end

	return nil
end

function GatewayPolicy.shouldUseGarrisonTroops(context)
	local up = context and context.up
	local down = context and context.down
	local settings = context and context.settings or {}
	local garrisonUsageAllowed = call(settings, "isGarrisonTroopsUsageAllowed")
	if garrisonUsageAllowed == nil then
		garrisonUsageAllowed = settings.garrisonTroopsUsageAllowed ~= false
	end

	return context and context.removableUnits == true
		and up and down
		and up.tempOwner == down.tempOwner
		and garrisonUsageAllowed
		and context.restrictedGarrisonsForAI ~= true
end

function GatewayPolicy.chooseHeroExchange(context)
	local firstHero = context and context.firstHero
	local secondHero = context and context.secondHero
	if not firstHero or not secondHero then
		return nil
	end
	if firstHero.tempOwner ~= secondHero.tempOwner then
		return nil
	end

	local firstActive = context.firstHeroActive
	if firstActive == nil and context.isActive then
		firstActive = context.isActive[objectID(firstHero)] == true
	end

	if firstActive then
		return {
			destination = secondHero,
			source = firstHero
		}
	end

	return {
		destination = firstHero,
		source = secondHero
	}
end

function GatewayPolicy.chooseUpgrade(upgradeInfo, stack, resources)
	if not upgradeInfo or not stack then
		return nil
	end

	local available = upgradeInfo.availableUpgrades or {}
	local best = nil
	for _, candidate in ipairs(available) do
		if not best or (candidate.aiValue or candidate.value or 0) > (best.aiValue or best.value or 0) then
			best = candidate
		end
	end

	if not best then
		return nil
	end

	local oldCreature = type(stack.creature) == "table" and stack.creature or {}
	local oldValue = stack.aiValue or stack.value or oldCreature.aiValue or oldCreature.value or 0
	local newValue = best.aiValue or best.value or 0
	local count = stack.count or 0
	local cost = multiplyCost(best.cost or upgradeInfo.costs and upgradeInfo.costs[objectID(best)] or upgradeInfo.cost or {}, count)

	if newValue > oldValue and canAfford(resources, cost) then
		return {
			creature = best,
			cost = cost
		}
	end

	return nil
end

local function stackSlot(stack, fallback)
	return type(stack) == "table" and (stack.slot or stack.position or stack.pos or fallback) or fallback
end

local function stackCreature(stack)
	if type(stack) ~= "table" then
		return stack
	end
	return stack.creature or stack.creatureID or stack.creatureId or stack.id or stack
end

local function stackCreatureID(stack)
	return objectID(stackCreature(stack))
end

local function armyStacks(army)
	local result = {}
	for key, stack in pairs(army and (army.slots or army.stacks) or {}) do
		local slot = stackSlot(stack, key)
		if slot ~= nil and stackCreatureID(stack) ~= nil then
			table.insert(result, {
				slot = slot,
				creature = stackCreature(stack)
			})
		end
	end
	table.sort(result, function(lhs, rhs)
		return lhs.slot < rhs.slot
	end)
	return result
end

local function makeArmyState(army)
	local state = {
		armySize = army and (army.armySize or army.slotsCount or army.size) or 7,
		stacks = armyStacks(army)
	}
	return state
end

local function occupiedSlots(state)
	local result = {}
	for _, stack in ipairs(state.stacks) do
		result[stack.slot] = true
	end
	return result
end

local function slotForCreatureInState(state, creature)
	local id = objectID(creature)
	for _, stack in ipairs(state.stacks) do
		if id ~= nil and objectID(stack.creature) == id then
			return stack.slot
		end
	end

	local occupied = occupiedSlots(state)
	for slot = 0, state.armySize - 1 do
		if not occupied[slot] then
			return slot
		end
	end
	return nil
end

local function findDuplicateMerge(state)
	local firstByCreature = {}
	for _, stack in ipairs(state.stacks) do
		local id = objectID(stack.creature)
		if id ~= nil then
			if firstByCreature[id] ~= nil and firstByCreature[id] ~= stack.slot then
				return {
					fromSlot = stack.slot,
					toSlot = firstByCreature[id]
				}
			end
			firstByCreature[id] = stack.slot
		end
	end
	return nil
end

local function applyMerge(state, merge)
	if not merge then
		return
	end
	for index, stack in ipairs(state.stacks) do
		if stack.slot == merge.fromSlot then
			table.remove(state.stacks, index)
			return
		end
	end
end

local function applyRecruitment(state, creature)
	local slot = slotForCreatureInState(state, creature)
	if slot == nil then
		return
	end
	for _, stack in ipairs(state.stacks) do
		if stack.slot == slot then
			return
		end
	end
	table.insert(state.stacks, {
		slot = slot,
		creature = creature
	})
	table.sort(state.stacks, function(lhs, rhs)
		return lhs.slot < rhs.slot
	end)
end

function GatewayPolicy.chooseDwellingRecruitment(dwelling, recruiter, resourceAmountValue)
	local result = {}
	local creatures = dwelling and dwelling.creatures or {}
	local state = makeArmyState(recruiter)

	for level, entry in ipairs(creatures) do
		local count = entry.count or entry[1] or 0
		local ids = entry.creatures or entry.ids or entry[2] or {}
		local creature = ids[#ids]
		if count > 0 and creature then
			local merge = nil
			local canFit = slotForCreatureInState(state, creature) ~= nil
			if not canFit then
				merge = findDuplicateMerge(state)
				applyMerge(state, merge)
				canFit = slotForCreatureInState(state, creature) ~= nil
			end

			local fullCost = creature.fullRecruitCost or creature.cost or 0
			if canFit then
				count = math.min(count, maxAffordableCount(resourceAmountValue, fullCost))
			end
			if canFit and count > 0 then
				table.insert(result, {
					level = level - 1,
					creature = creature,
					count = count,
					merge = merge
				})
				applyRecruitment(state, creature)
			end
		end
	end

	return result
end

local function artifactSlot(entry, fallback)
	return type(entry) == "table" and (entry.slot or entry.position or entry.pos or fallback) or fallback
end

local function artifactFromSlot(entry)
	return type(entry) == "table" and (entry.artifact or entry.art or entry.item or entry) or entry
end

local function artifactInstanceID(artifact)
	return objectID(artifact and (artifact.instanceID or artifact.artifactInstanceID or artifact.artifactID or artifact))
end

local function artifactPossibleSlots(artifact)
	if not artifact then
		return {}
	end
	return artifact.possibleSlots
		or artifact.equipmentSlots
		or artifact.slots
		or artifact.type and (artifact.type.possibleSlots or artifact.type.equipmentSlots or artifact.type.slots)
		or {}
end

local function artifactCanBePutAt(artifact, hero, slot)
	if not artifact then
		return false
	end
	if artifact.canBePutAt then
		if type(artifact.canBePutAt) == "function" then
			return artifact:canBePutAt(hero, slot)
		end
		if type(artifact.canBePutAt) == "table" then
			return artifact.canBePutAt[slot] ~= false and artifact.canBePutAt[tostring(slot)] ~= false
		end
	end
	local denied = artifact.deniedSlots
	return not (type(denied) == "table" and (denied[slot] or denied[tostring(slot)]))
end

local function isEquipmentSlot(slot)
	return type(slot) == "number" and slot >= 0 and slot < BACKPACK_START
end

local function addArtifactLocations(result, hero, entries)
	for index, entry in pairs(entries or {}) do
		local artifact = artifactFromSlot(entry)
		if artifact then
			table.insert(result, {
				hero = hero,
				slot = artifactSlot(entry, index),
				artifact = artifact,
				locked = type(entry) == "table" and entry.locked == true
			})
		end
	end
end

local function collectArtifacts(hero, otherHero, giveStuffToFirstHero)
	local result = {}
	if giveStuffToFirstHero then
		addArtifactLocations(result, hero, hero and (hero.artifactsWorn or hero.wornArtifacts))
	end
	addArtifactLocations(result, hero, hero and (hero.artifactsInBackpack or hero.backpackArtifacts))
	if otherHero then
		addArtifactLocations(result, otherHero, otherHero.artifactsWorn or otherHero.wornArtifacts)
		addArtifactLocations(result, otherHero, otherHero.artifactsInBackpack or otherHero.backpackArtifacts)
	end
	return result
end

local function findArtifactAt(hero, slot)
	for _, entry in pairs(hero and (hero.artifactsWorn or hero.wornArtifacts) or {}) do
		if artifactSlot(entry) == slot then
			return entry, artifactFromSlot(entry)
		end
	end
	for _, entry in pairs(hero and (hero.artifactsInBackpack or hero.backpackArtifacts) or {}) do
		if artifactSlot(entry) == slot then
			return entry, artifactFromSlot(entry)
		end
	end
	return nil, nil
end

local function isPositionFree(hero, slot)
	if hero and hero.freeArtifactSlots then
		return hero.freeArtifactSlots[slot] == true or hero.freeArtifactSlots[tostring(slot)] == true
	end
	return findArtifactAt(hero, slot) == nil
end

local function setArtifactAt(hero, slot, artifact)
	local entry = findArtifactAt(hero, slot)
	if entry then
		entry.artifact = artifact
		entry.art = artifact
		entry.item = artifact
	elseif isEquipmentSlot(slot) then
		hero.artifactsWorn = hero.artifactsWorn or {}
		table.insert(hero.artifactsWorn, { slot = slot, artifact = artifact })
	else
		hero.artifactsInBackpack = hero.artifactsInBackpack or {}
		table.insert(hero.artifactsInBackpack, { slot = slot, artifact = artifact })
	end
end

local function removeArtifactAt(hero, slot)
	local function removeFrom(entries)
		for index, entry in pairs(entries or {}) do
			if artifactSlot(entry, index) == slot then
				entries[index] = nil
				return true
			end
		end
		return false
	end

	if removeFrom(hero and (hero.artifactsWorn or hero.wornArtifacts)) then
		return
	end
	removeFrom(hero and (hero.artifactsInBackpack or hero.backpackArtifacts))
end

local function applyArtifactSwap(sourceHero, sourceSlot, destinationHero, destinationSlot)
	local _, sourceArtifact = findArtifactAt(sourceHero, sourceSlot)
	local _, destinationArtifact = findArtifactAt(destinationHero, destinationSlot)
	removeArtifactAt(sourceHero, sourceSlot)
	removeArtifactAt(destinationHero, destinationSlot)
	if destinationArtifact then
		setArtifactAt(sourceHero, sourceSlot, destinationArtifact)
	end
	if sourceArtifact then
		setArtifactAt(destinationHero, destinationSlot, sourceArtifact)
	end
end

local function swapArtifacts(aiGw, sourceHero, sourceSlot, destinationHero, destinationSlot)
	aiGw:swapArtifacts(sourceHero, sourceSlot, destinationHero, destinationSlot)
	applyArtifactSwap(sourceHero, sourceSlot, destinationHero, destinationSlot)
end

local function moveBetterArtifactToSlot(aiGw, location, target, slot, otherArtifact)
	if artifactCanBePutAt(otherArtifact, location.hero, location.slot) then
		swapArtifacts(aiGw, location.hero, location.slot, target, slot)
		return 1
	end

	swapArtifacts(aiGw, target, slot, location.hero, BACKPACK_START)
	swapArtifacts(aiGw, location.hero, location.slot, target, slot)
	return 2
end

local function equipArtifactsForTarget(aiGw, hero, otherHero, giveStuffToFirstHero)
	local target = (giveStuffToFirstHero or not otherHero) and hero or otherHero
	local swapped = {}
	local commandCount = 0
	local changeMade = true

	while changeMade do
		changeMade = false
		for _, location in ipairs(collectArtifacts(hero, otherHero, giveStuffToFirstHero)) do
			local skip = (objectID(location.hero) == objectID(target) and isEquipmentSlot(location.slot))
				or location.slot == MACH4
				or location.locked

			if not skip then
				local artifact = location.artifact
				for _, slot in ipairs(artifactPossibleSlots(artifact)) do
					if isPositionFree(target, slot) and artifactCanBePutAt(artifact, target, slot) then
						swapArtifacts(aiGw, location.hero, location.slot, target, slot)
						commandCount = commandCount + 1
						changeMade = true
						break
					end
				end
				if changeMade then
					break
				end

				local artifactScore = AIUtility.getArtifactScoreForHero(target, artifact)
				for _, slot in ipairs(artifactPossibleSlots(artifact)) do
					local _, otherArtifact = findArtifactAt(target, slot)
					if otherArtifact then
						local otherArtifactScore = AIUtility.getArtifactScoreForHero(target, otherArtifact)
						if artifactScore > otherArtifactScore and artifactCanBePutAt(artifact, target, slot) then
							local left = artifactInstanceID(artifact)
							local right = artifactInstanceID(otherArtifact)
							local swapKey = tostring(math.min(left or 0, right or 0)) .. ":" .. tostring(math.max(left or 0, right or 0))
							if not swapped[swapKey] then
								commandCount = commandCount + moveBetterArtifactToSlot(aiGw, location, target, slot, otherArtifact)
								swapped[swapKey] = true
								changeMade = true
							end
							break
						end
					end
				end
				if changeMade then
					break
				end
			end
		end
	end

	return commandCount
end

function GatewayPolicy.pickBestArtifacts(aiGw, hero, otherHero)
	if not (aiGw and type(aiGw.swapArtifacts) == "function") then
		return 0
	end

	local commandCount = equipArtifactsForTarget(aiGw, hero, otherHero, true)
	if otherHero then
		commandCount = commandCount + equipArtifactsForTarget(aiGw, hero, otherHero, false)
	end
	return commandCount
end

GatewayPolicy.ComponentType = ComponentType
GatewayPolicy.ObjectType = ObjectType
GatewayPolicy.TeleportPassability = TeleportPassability

return GatewayPolicy
