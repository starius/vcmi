-- Mirrors AI/Nullkiller2/Goals/BuyArmy.{h,cpp}: BuyArmy identity, string, and task shape.

local AbstractGoal = require("Goals.AbstractGoal")
local GatewayPolicy = require("Actions.GatewayPolicy")
local CGoal = require("Goals.CGoal")

local BuyArmy = CGoal.derive("BuyArmy", AbstractGoal.EGoals.BUY_ARMY, { elementar = true })
local RESOURCE_COUNT = 7

local function townName(town)
	return AbstractGoal._helpers.translatedName(town, tostring(CGoal.objectID(town)))
end

local function call(object, name, ...)
	if type(object) == "table" and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function numericID(value)
	return CGoal.objectID(value)
end

local function getResource(resources, resourceID)
	if type(resources) == "number" then
		return resourceID == 6 and resources or 0
	end
	if not resources then
		return 0
	end
	if resources[0] == nil and resources[7] ~= nil then
		return resources[resourceID + 1] or 0
	end
	return resources[resourceID] or 0
end

local function resourceCost(creature)
	return type(creature) == "table" and (creature.fullRecruitCost or creature.recruitCost or creature.cost) or 0
end

local function maxAffordableCount(resources, cost)
	if type(cost) == "number" then
		if cost <= 0 then
			return math.huge
		end
		return math.floor(getResource(resources, 6) / cost)
	end

	local result = math.huge
	for resourceID = 0, RESOURCE_COUNT - 1 do
		local needed = getResource(cost, resourceID)
		if needed > 0 then
			result = math.min(result, math.floor(getResource(resources, resourceID) / needed))
		end
	end
	return result
end

local function creatureAIValue(creature)
	return type(creature) == "table" and (creature.aiValue or creature.AIValue or creature.value) or 0
end

local function creatureFactionID(creature)
	return type(creature) == "table" and (creature.factionID or creature.faction or -1) or -1
end

local function availableArmy(town)
	local result = call(town, "getArmyAvailableToBuy")
	if result ~= nil then
		return result
	end
	return town and (town.armyAvailableToBuy or town.availableToBuy) or {}
end

local function upperArmy(town)
	return call(town, "getUpperArmy") or town and (town.upperArmy or town.garrisonHero or town)
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town and town.visitingHero
end

local function garrisonHero(town)
	return call(town, "getGarrisonHero") or town and town.garrisonHero
end

local function visitablePos(town)
	return call(town, "visitablePos") or town and (town.visitablePos or town.tile)
end

local function resources(aiGw)
	if aiGw and type(aiGw.getFreeResources) == "function" then
		return aiGw:getFreeResources()
	end
	if aiGw and aiGw.nullkiller and type(aiGw.nullkiller.getFreeResources) == "function" then
		return aiGw.nullkiller:getFreeResources()
	end
	if aiGw and type(aiGw.getResourceAmount) == "function" then
		return aiGw:getResourceAmount()
	end
	return aiGw and (aiGw.freeResources or aiGw.resources) or {}
end

local function upgradeSlots(town, army)
	local result = call(town, "getUpgradeSlots", army)
		or call(army, "getUpgradeSlots")
		or army and army.upgradeSlots
		or town and town.upgradeSlots
		or {}
	return result
end

local function makePossibleUpgrades(aiGw, town, army, resourceSnapshot)
	local upgraded = false
	for _, entry in ipairs(upgradeSlots(town, army)) do
		local stack = entry.stack or entry
		local slot = entry.slot or stack.slot
		local upgradeInfo = entry.upgradeInfo or entry
		local upgrade = GatewayPolicy.chooseUpgrade(upgradeInfo, stack, resourceSnapshot)
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

local function armyStacks(army)
	return type(army) == "table" and (army.slots or army.stacks or army.Slots) or {}
end

local function stacksCount(army)
	local result = call(army, "stacksCount")
	if result ~= nil then
		return result
	end
	return army and (army.stacksCount or army.stacks_count) or 0
end

local function slotHasCreature(army, creatureID)
	local result = call(army, "getSlotFor", creatureID)
	if result ~= nil then
		if type(result) == "table" then
			return result.validSlot ~= false
		end
		return result ~= false
	end

	local slotsByCreature = type(army) == "table" and army.slotsByCreature or nil
	local id = numericID(creatureID)
	return slotsByCreature ~= nil
		and (slotsByCreature[id] ~= nil or slotsByCreature[tostring(id)] ~= nil)
end

local function lowestDismissibleSlot(army, town)
	local resultSlot = nil
	local resultValue = math.huge
	local townFaction = creatureFactionID(town)

	for slot, stack in pairs(armyStacks(army)) do
		local actualSlot = type(stack) == "table" and (stack.slot or slot) or slot
		local creature = type(stack) == "table" and (stack.creature or stack.creatureID or stack) or stack
		if numericID(creature) ~= nil and creatureFactionID(creature) ~= townFaction then
			local count = type(stack) == "table" and (stack.count or stack.stackCount or 0) or 0
			local stackValue = type(stack) == "table" and stack.marketValue or nil
			local creatureValue = type(creature) == "table" and creature.marketValue or nil
			local marketValue = (stackValue or creatureValue or creatureAIValue(creature)) * count
			if marketValue < resultValue then
				resultValue = marketValue
				resultSlot = actualSlot
			end
		end
	end

	return resultSlot
end

function BuyArmy:init(Town, val)
	if Town ~= nil then
		self.town = Town
		self.value = val
		self.priority = 3
	end
end

function BuyArmy:equalsTyped(other)
	return self.town == other.town and self.objid == other.objid
end

function BuyArmy.needsFreeSlotToRecruit(army, creatureID)
	local armySize = army and (army.armySize or army.ARMY_SIZE) or 7
	local stacksCount = army and (army.stacksCount or army.stacks_count or 0) or 0

	if type(stacksCount) == "function" then
		stacksCount = stacksCount(army)
	end

	local hasSlot = false
	if army and type(army.getSlotFor) == "function" then
		local slot = army:getSlotFor(creatureID)
		if type(slot) == "table" then
			hasSlot = slot.validSlot ~= false
		else
			hasSlot = slot ~= nil and slot ~= false
		end
	elseif army and army.slotsByCreature then
		hasSlot = army.slotsByCreature[creatureID] ~= nil or army.slotsByCreature[tostring(creatureID)] ~= nil
	end

	return stacksCount == armySize and not hasSlot
end

function BuyArmy:toString()
	return "Buy army at " .. townName(self.town)
end

function BuyArmy:accept(aiGw)
	local valueBought = 0
	local army = upperArmy(self.town)
	local resourceSnapshot = resources(aiGw)
	local upgradeSuccessful = makePossibleUpgrades(aiGw, self.town, army, resourceSnapshot)
	local armyToBuy = {}
	for _, creatureInfo in ipairs(availableArmy(self.town)) do
		table.insert(armyToBuy, creatureInfo)
	end

	if #armyToBuy == 0 and upgradeSuccessful then
		return {
			valueBought = 0,
			upgradeSuccessful = true
		}
	end

	table.sort(armyToBuy, function(lhs, rhs)
		return creatureAIValue(lhs.creature or lhs.creID or lhs) > creatureAIValue(rhs.creature or rhs.creID or rhs)
	end)

	for _, creatureInfo in ipairs(armyToBuy) do
		if valueBought >= self.value then
			break
		end

		local creature = creatureInfo.creature or creatureInfo.creID or creatureInfo
		if self.objid == -1 or numericID(creature) == self.objid then
			local count = math.min(creatureInfo.count or 0, maxAffordableCount(resourceSnapshot, resourceCost(creature)))
			if count > 0 then
				local freedSlot = false
				if BuyArmy.needsFreeSlotToRecruit(army, numericID(creature)) then
					local slot = lowestDismissibleSlot(army, self.town)
					if slot ~= nil and aiGw and type(aiGw.dismissCreature) == "function" then
						aiGw:dismissCreature(army, slot)
						freedSlot = true
					end
				end

				if freedSlot or stacksCount(army) < (army and (army.armySize or army.ARMY_SIZE) or 7) or slotHasCreature(army, creature) then
					if aiGw and type(aiGw.recruitCreatures) == "function" then
						aiGw:recruitCreatures(self.town, army, creature, count, creatureInfo.level)
					else
						error("No creature recruitment command target.", 2)
					end
					valueBought = valueBought + count * creatureAIValue(creature)
				end
			end
		end
	end

	if valueBought == 0 then
		error("No creatures to buy.", 2)
	end

	local hero = visitingHero(self.town)
	if hero and not garrisonHero(self.town) then
		if aiGw and type(aiGw.moveHeroToTile) == "function" then
			aiGw:moveHeroToTile(visitablePos(self.town), hero)
		else
			error("No hero movement command target.", 2)
		end
	end

	return {
		valueBought = valueBought,
		upgradeSuccessful = upgradeSuccessful
	}
end

return BuyArmy
