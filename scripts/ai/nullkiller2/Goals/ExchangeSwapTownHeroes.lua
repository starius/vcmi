-- Mirrors AI/Nullkiller2/Goals/ExchangeSwapTownHeroes.{h,cpp}: ExchangeSwapTownHeroes.

local AbstractGoal = require("Goals.AbstractGoal")
local ArmyManager = require("Analyzers.ArmyManager")
local CGoal = require("Goals.CGoal")
local GatewayPolicy = require("Actions.GatewayPolicy")
local State = require("Engine.State")

local ExchangeSwapTownHeroes = CGoal.derive(
	"ExchangeSwapTownHeroes",
	AbstractGoal.EGoals.EXCHANGE_SWAP_TOWN_HEROES,
	{ elementar = true })

local RESOURCE_COUNT = 7

local function townName(town)
	return AbstractGoal._helpers.translatedName(town, tostring(CGoal.objectID(town)))
end

local function visitingHero(town)
	if not town then
		return nil
	end
	if type(town.getVisitingHero) == "function" then
		return town:getVisitingHero()
	end
	return town.visitingHero
end

local function garrisonHero(town)
	if not town then
		return nil
	end
	if type(town.getGarrisonHero) == "function" then
		return town:getGarrisonHero()
	end
	return town.garrisonHero
end

local function sameObject(lhs, rhs)
	local leftID = CGoal.objectID(lhs)
	local rightID = CGoal.objectID(rhs)
	return lhs == rhs or (leftID ~= nil and leftID == rightID)
end

local function visitablePos(object)
	if not object then
		return nil
	end
	if type(object.visitablePos) == "function" then
		return object:visitablePos()
	end
	return object.visitablePos or object.tile
end

local function upperArmy(town)
	if not town then
		return nil
	end
	if type(town.getUpperArmy) == "function" then
		return town:getUpperArmy()
	end
	return town.upperArmy or town
end

local function resources(aiGw)
	if aiGw and type(aiGw.getFreeResources) == "function" then
		return aiGw:getFreeResources()
	end
	return aiGw and (aiGw.freeResources or aiGw.resources) or {}
end

local function resourceValue(resourceSnapshot, resourceID)
	if type(resourceSnapshot) == "number" then
		return resourceID == 6 and resourceSnapshot or 0
	end
	if not resourceSnapshot then
		return 0
	end
	if resourceSnapshot[0] == nil and resourceSnapshot[7] ~= nil then
		return resourceSnapshot[resourceID + 1] or 0
	end
	return resourceSnapshot[resourceID] or resourceSnapshot[tostring(resourceID)] or 0
end

local function resourceCost(creature)
	return type(creature) == "table" and (creature.fullRecruitCost or creature.recruitCost or creature.cost) or 0
end

local function maxAffordableCount(resourceSnapshot, cost)
	if type(cost) == "number" then
		if cost <= 0 then
			return math.huge
		end
		return math.floor(resourceValue(resourceSnapshot, 6) / cost)
	end

	local result = math.huge
	for resourceID = 0, RESOURCE_COUNT - 1 do
		local needed = resourceValue(cost, resourceID)
		if needed > 0 then
			result = math.min(result, math.floor(resourceValue(resourceSnapshot, resourceID) / needed))
		end
	end
	return result
end

local function creatureEntries(town)
	local result = {}
	for level, entry in ipairs(town and town.creatures or {}) do
		local count = entry.count or entry[1] or 0
		local ids = entry.creatures or entry.ids or entry[2] or {}
		local creature = ids[#ids] or entry.creature
		if creature then
			table.insert(result, {
				level = level - 1,
				count = count,
				creature = creature
			})
		end
	end
	return result
end

local function armySize(army)
	return army and (army.armySize or army.ARMY_SIZE) or 7
end

local function stackSlot(stack, fallback)
	return type(stack) == "table" and (stack.slot or fallback) or fallback
end

local function stackCreature(stack)
	return type(stack) == "table" and (stack.creature or stack.creatureID or stack) or stack
end

local function creatureID(value)
	if type(value) == "table" and value.creatureID ~= nil then
		return CGoal.objectID(value.creatureID)
	end
	if type(value) == "table" and value.creature ~= nil then
		return CGoal.objectID(value.creature)
	end
	return CGoal.objectID(value)
end

local function armyStacksBySlot(army)
	return army and (army.slots or army.stacks) or {}
end

local stacksCount
local applyMergeOrSwap

local function slotForCreature(army, creature)
	if army and type(army.getSlotFor) == "function" then
		local slot = army:getSlotFor(creature)
		if type(slot) == "table" then
			if slot.validSlot == false then
				return nil
			end
			return slot.num or slot.id or slot[1]
		end
		if slot ~= nil and slot ~= false and slot ~= -1 then
			return slot
		end
	end

	local id = creatureID(creature)
	for slot, stack in pairs(armyStacksBySlot(army)) do
		if id ~= nil and creatureID(stack) == id then
			return stackSlot(stack, slot)
		end
	end

	local slotsByCreature = army and army.slotsByCreature
	if slotsByCreature then
		local slot = slotsByCreature[id] or slotsByCreature[tostring(id)]
		if slot ~= nil then
			return slot
		end
	end

	if stacksCount(army) < armySize(army) then
		return army and army.freeSlot or 0
	end
	return nil
end

local function findDuplicatingStack(army)
	for slot, stack in pairs(armyStacksBySlot(army)) do
		local fromSlot = stackSlot(stack, slot)
		local toSlot = type(stack) == "table" and stack.duplicatingSlot
		if toSlot ~= nil and toSlot ~= fromSlot then
			return fromSlot, toSlot
		end

		local creature = stackCreature(stack)
		toSlot = slotForCreature(army, creature)
		if toSlot ~= nil and toSlot ~= fromSlot then
			return fromSlot, toSlot
		end
	end
	return nil, nil
end

local function ensureRecruitSlot(aiGw, army, creature)
	if slotForCreature(army, creature) ~= nil then
		return true
	end

	local fromSlot, toSlot = findDuplicatingStack(army)
	if fromSlot ~= nil and toSlot ~= nil and aiGw and type(aiGw.mergeStacks) == "function" then
		aiGw:mergeStacks(army, fromSlot, toSlot)
		if applyMergeOrSwap then
			applyMergeOrSwap(army, army, fromSlot, toSlot)
		elseif type(army) == "table" and type(army.stacksCount) == "number" then
			army.stacksCount = math.max(0, army.stacksCount - 1)
		end
		return true
	end
	return false
end

local function recruitCreaturesForArmy(aiGw, town, army)
	local resourceSnapshot = resources(aiGw)
	local recruited = false
	for _, entry in ipairs(creatureEntries(town)) do
		local count = entry.count
		local creature = entry.creature
		if count > 0 and ensureRecruitSlot(aiGw, army, creature) then
			count = math.min(count, maxAffordableCount(resourceSnapshot, resourceCost(creature)))
			if count > 0 then
				if aiGw and type(aiGw.recruitCreatures) == "function" then
					aiGw:recruitCreatures(town, army, creature, count, entry.level)
					recruited = true
				else
					error("No creature recruitment command target.", 2)
				end
			end
		end
	end
	return recruited
end

local function upgradeSlots(army)
	if not army then
		return {}
	end
	if type(army.getUpgradeSlots) == "function" then
		return army:getUpgradeSlots()
	end
	return army.upgradeSlots or {}
end

local function makePossibleUpgrades(aiGw, army)
	local upgraded = false
	for _, entry in ipairs(upgradeSlots(army)) do
		local stack = entry.stack or entry
		local slot = entry.slot or stack.slot
		local upgradeInfo = entry.upgradeInfo or entry
		local upgrade = GatewayPolicy.chooseUpgrade(upgradeInfo, stack, resources(aiGw))
		if upgrade then
			if aiGw and type(aiGw.upgradeCreature) == "function" then
				aiGw:upgradeCreature(army, slot, upgrade.creature)
				upgraded = true
			else
				error("No creature upgrade command target.", 2)
			end
		end
	end
	return upgraded
end

local function stacks(army)
	return army and (army.slots or army.stacks) or {}
end

function stacksCount(army)
	if army and type(army.stacksCount) == "function" then
		return army:stacksCount()
	end
	return army and (army.stacksCount or 0) or 0
end

local function mutableArmyStacks(army)
	if not army then
		return {}
	end
	if not army.slots and not army.stacks then
		army.slots = {}
	end
	return army.slots or army.stacks
end

local function stackAtSlot(army, targetSlot)
	for slot, stack in pairs(armyStacksBySlot(army)) do
		if stackSlot(stack, slot) == targetSlot then
			return stack
		end
	end
	return nil
end

local function hasStackAtSlot(army, slot)
	return stackAtSlot(army, slot) ~= nil
end

local function firstStackSlot(army)
	local result = nil
	for slot, stack in pairs(armyStacksBySlot(army)) do
		local candidate = stackSlot(stack, slot)
		if candidate ~= nil and (result == nil or candidate < result) then
			result = candidate
		end
	end
	return result
end

local function refreshStacksCount(army)
	if type(army) ~= "table" or type(army.stacksCount) ~= "number" then
		return
	end

	local count = 0
	for _, stack in pairs(armyStacksBySlot(army)) do
		if stack ~= nil then
			count = count + 1
		end
	end
	army.stacksCount = count
end

local function removeStackAtSlot(army, targetSlot)
	local slots = mutableArmyStacks(army)
	for key, stack in pairs(slots) do
		if stackSlot(stack, key) == targetSlot then
			if type(key) == "number" and key >= 1 and slots[key] == stack then
				table.remove(slots, key)
			else
				slots[key] = nil
			end
			refreshStacksCount(army)
			return stack
		end
	end
	return nil
end

local function setStackAtSlot(army, targetSlot, stack)
	if not stack then
		removeStackAtSlot(army, targetSlot)
		return
	end

	if type(stack) == "table" then
		stack.slot = targetSlot
	end

	local slots = mutableArmyStacks(army)
	for key, existing in pairs(slots) do
		if stackSlot(existing, key) == targetSlot then
			slots[key] = stack
			refreshStacksCount(army)
			return
		end
	end

	table.insert(slots, stack)
	refreshStacksCount(army)
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

local function armyStrength(army)
	return army and (army.armyStrength or army.totalStrength or 0) or 0
end

local function sameCreature(left, right)
	local leftID = creatureID(left)
	return leftID ~= nil and leftID == creatureID(right)
end

applyMergeOrSwap = function(source, destination, fromSlot, toSlot)
	if source == destination and fromSlot == toSlot then
		return
	end

	local sourceStack = stackAtSlot(source, fromSlot)
	local destinationStack = stackAtSlot(destination, toSlot)

	if sourceStack and destinationStack and sameCreature(sourceStack, destinationStack) then
		if type(destinationStack) == "table" and type(sourceStack) == "table" then
			local combinedCount = stackCount(destinationStack) + stackCount(sourceStack)
			local combinedPower = stackPower(destinationStack) + stackPower(sourceStack)
			destinationStack.count = combinedCount
			destinationStack.power = combinedPower
		end
		removeStackAtSlot(source, fromSlot)
		return
	end

	removeStackAtSlot(source, fromSlot)
	removeStackAtSlot(destination, toSlot)
	setStackAtSlot(source, fromSlot, destinationStack)
	setStackAtSlot(destination, toSlot, sourceStack)
end

local function bestArmyForTransfer(town, destination, source)
	return (town and (town.moveCreaturesToHeroBestArmy or town.armyTransferBestArmy or town.bestArmy))
		or (destination and destination.bestArmy)
		or (source and source.bestArmy)
		or ArmyManager.getBestArmy(destination, destination, source, {
			armySize = armySize(destination),
			settings = town and town.settings or destination and destination.settings or source and source.settings,
			terrain = source and (source.terrain or source.armyTerrain) or town and town.terrain
		})
end

local function bestArmyCreatureID(entry)
	if type(entry) == "table" then
		return creatureID(entry.creature or entry.creatureID or entry)
	end
	return creatureID(entry)
end

local function owner(object)
	if type(object) ~= "table" then
		return nil
	end
	return object.tempOwner or object.owner
end

local function sameOwner(left, right)
	local leftOwner = owner(left)
	local rightOwner = owner(right)
	if leftOwner == nil or rightOwner == nil then
		return true
	end
	return CGoal.objectID(leftOwner) == CGoal.objectID(rightOwner)
end

local function moveCreaturesToHero(aiGw, town, hero)
	if not (aiGw and type(aiGw.mergeOrSwapStacks) == "function") then
		return false
	end

	local destination = hero or visitingHero(town)
	local source = upperArmy(town)
	if not destination or not source or stacksCount(source) == 0 or not sameOwner(destination, town) then
		return false
	end

	for _, army in ipairs({ destination, source }) do
		if not hasStackAtSlot(army, 0) and stacksCount(army) > 0 then
			local slot = firstStackSlot(army)
			if slot ~= nil and slot ~= 0 then
				aiGw:mergeOrSwapStacks(army, army, 0, slot)
				applyMergeOrSwap(army, army, 0, slot)
			end
		end
	end

	local bestArmy = bestArmyForTransfer(town, destination, source)
	local moved = false
	for targetSlot = 0, armySize(destination) - 1 do
		local bestEntry = bestArmy[targetSlot + 1]
		if not bestEntry then
			local currentStack = stackAtSlot(destination, targetSlot)
			if currentStack then
				local sourceSlot = slotForCreature(source, stackCreature(currentStack))
				if sourceSlot ~= nil then
					aiGw:mergeOrSwapStacks(destination, source, targetSlot, sourceSlot)
					applyMergeOrSwap(destination, source, targetSlot, sourceSlot)
					moved = true
				elseif stackPower(currentStack) < armyStrength(destination) / 100 and type(aiGw.dismissCreature) == "function" then
					aiGw:dismissCreature(destination, targetSlot)
					removeStackAtSlot(destination, targetSlot)
					moved = true
				end
			end
		else
			local targetCreature = bestArmyCreatureID(bestEntry)
			for _, army in ipairs({ destination, source }) do
				local found = false
				for slot, stack in pairs(armyStacksBySlot(army)) do
					local currentSlot = stackSlot(stack, slot)
					if targetCreature ~= nil and creatureID(stack) == targetCreature and (currentSlot ~= targetSlot or army ~= destination) then
						aiGw:mergeOrSwapStacks(army, destination, currentSlot, targetSlot)
						applyMergeOrSwap(army, destination, currentSlot, targetSlot)
						moved = true
						found = true
						break
					end
				end
				if found then
					break
				end
			end
		end
	end

	return moved
end

local function canBeMergedWith(hero, town)
	if hero and type(hero.canBeMergedWith) == "function" then
		return hero:canBeMergedWith(town)
	end
	if hero and hero.canBeMergedWithTown ~= nil then
		return hero.canBeMergedWithTown
	end
	return true
end

local function addObject(result, object)
	local id = CGoal.objectID(object)
	if id ~= nil then
		table.insert(result, id)
	end
end

function ExchangeSwapTownHeroes:init(town, garrisonHeroValue, lockingReason)
	self.town = town
	self.garrisonHero = garrisonHeroValue
	self.lockingReason = lockingReason or State.HeroLockedReason.NOT_LOCKED
end

function ExchangeSwapTownHeroes:getGarrisonHero()
	return self.garrisonHero
end

function ExchangeSwapTownHeroes:getLockingReason()
	return self.lockingReason
end

function ExchangeSwapTownHeroes:getAffectedObjects()
	local affectedObjects = {}
	addObject(affectedObjects, self.town)
	addObject(affectedObjects, garrisonHero(self.town))
	addObject(affectedObjects, visitingHero(self.town))
	return affectedObjects
end

function ExchangeSwapTownHeroes:isObjectAffected(id)
	local targetID = CGoal.objectID(id)
	return CGoal.objectID(self.town) == targetID
		or (visitingHero(self.town) and CGoal.objectID(visitingHero(self.town)) == targetID)
		or (garrisonHero(self.town) and CGoal.objectID(garrisonHero(self.town)) == targetID)
end

function ExchangeSwapTownHeroes:toString()
	return "Exchange and swap heroes of " .. townName(self.town)
end

function ExchangeSwapTownHeroes:equalsTyped(other)
	return self.town == other.town
end

function ExchangeSwapTownHeroes:accept(aiGw)
	if not (aiGw and type(aiGw.swapGarrisonHero) == "function") then
		return {
			action = "exchangeSwapTownHeroes",
			town = CGoal.objectID(self.town),
			garrisonHero = CGoal.objectID(self.garrisonHero),
			lockingReason = self.lockingReason
		}
	end

	local targetGarrisonHero = self:getGarrisonHero()
	if not targetGarrisonHero then
		local currentGarrisonHero = garrisonHero(self.town)
		if not currentGarrisonHero then
			error("Invalid configuration. There is no hero in town garrison.", 2)
		end

		aiGw:swapGarrisonHero(self.town)
		makePossibleUpgrades(aiGw, currentGarrisonHero)
		makePossibleUpgrades(aiGw, self.town)
		recruitCreaturesForArmy(aiGw, self.town, upperArmy(self.town))
		moveCreaturesToHero(aiGw, self.town, currentGarrisonHero)
		if type(aiGw.unlockHero) == "function" then
			aiGw:unlockHero(currentGarrisonHero)
		end
		return {
			action = "exchangeSwapTownHeroes",
			extractedHero = CGoal.objectID(currentGarrisonHero)
		}
	end

	if visitingHero(self.town) and not sameObject(visitingHero(self.town), targetGarrisonHero) then
		aiGw:swapGarrisonHero(self.town)
	end

	makePossibleUpgrades(aiGw, self.town)

	local targetTile = visitablePos(self.town)
	if targetTile and type(aiGw.executeHeroChain) == "function" then
		aiGw:executeHeroChain({
			targetHero = targetGarrisonHero,
			targetTile = targetTile
		}, CGoal.objectID(self.town))
	end

	local army = upperArmy(self.town)
	local armyCleared = stacksCount(army) == 0
	if not garrisonHero(self.town) and not canBeMergedWith(targetGarrisonHero, self.town) then
		for _, stack in ipairs(stacks(army)) do
			local slot = type(stack) == "table" and stack.slot or nil
			if slot ~= nil and type(aiGw.dismissCreature) == "function" then
				aiGw:dismissCreature(army, slot)
			end
		end
		armyCleared = true
	end

	if armyCleared or canBeMergedWith(targetGarrisonHero, self.town) then
		aiGw:swapGarrisonHero(self.town)
	end

	if self.lockingReason ~= State.HeroLockedReason.NOT_LOCKED and type(aiGw.lockHero) == "function" then
		aiGw:lockHero(targetGarrisonHero, self.lockingReason)
	end

	if visitingHero(self.town) and not sameObject(visitingHero(self.town), targetGarrisonHero) and type(aiGw.unlockHero) == "function" then
		aiGw:unlockHero(visitingHero(self.town))
		makePossibleUpgrades(aiGw, visitingHero(self.town))
	end

	return {
		action = "exchangeSwapTownHeroes",
		town = CGoal.objectID(self.town),
		garrisonHero = CGoal.objectID(targetGarrisonHero),
		lockingReason = self.lockingReason
	}
end

return ExchangeSwapTownHeroes
