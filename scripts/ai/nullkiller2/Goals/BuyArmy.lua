-- Mirrors AI/Nullkiller2/Goals/BuyArmy.{h,cpp}: BuyArmy identity, string, and task shape.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local BuyArmy = CGoal.derive("BuyArmy", AbstractGoal.EGoals.BUY_ARMY, { elementar = true })

local function townName(town)
	return AbstractGoal._helpers.translatedName(town, tostring(CGoal.objectID(town)))
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
		hasSlot = slot and slot.validSlot ~= false
	elseif army and army.slotsByCreature then
		hasSlot = army.slotsByCreature[creatureID] ~= nil
	end

	return stacksCount == armySize and not hasSlot
end

function BuyArmy:toString()
	return "Buy army at " .. townName(self.town)
end

function BuyArmy:accept(aiGw)
	if aiGw and type(aiGw.buyArmy) == "function" then
		return aiGw:buyArmy(self)
	end

	error("No creatures to buy.", 2)
end

return BuyArmy
