-- Mirrors AI/Nullkiller2/Goals/ExchangeSwapTownHeroes.{h,cpp}: ExchangeSwapTownHeroes.

local AbstractGoal = require("Goals.AbstractGoal")
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

local function armyStacksBySlot(army)
	return army and (army.slots or army.stacks) or {}
end

local stacksCount

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

	local id = CGoal.objectID(creature)
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
		if type(army) == "table" and type(army.stacksCount) == "number" then
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
