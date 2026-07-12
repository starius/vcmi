-- Mirrors AI/Nullkiller2/Markers/ArmyUpgrade.{h,cpp}: ArmyUpgrade.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local ArmyUpgrade = CGoal.derive("ArmyUpgrade", AbstractGoal.EGoals.ARMY_UPGRADE)

local GOLD = 6

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function armyStrength(army)
	local result = call(army, "getArmyStrength")
	if result ~= nil then
		return result
	end
	return army and (army.armyStrength or army.totalStrength or army.strength) or 0
end

local function getResource(resources, resourceID)
	if not resources then
		return 0
	end
	if resources[0] == nil and resources[7] ~= nil then
		return resources[resourceID + 1] or 0
	end
	return resources[resourceID] or 0
end

local function objectName(object)
	return object and (object.objectName or object.name or object.typeName or tostring(CGoal.objectID(object))) or ""
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile) or {}
end

function ArmyUpgrade:init(pathOrTargetMain, upgrader, upgrade)
	upgrade = upgrade or {}
	self.upgrader = upgrader
	self.upgradeValue = upgrade.upgradeValue or 0
	self.goldCost = getResource(upgrade.upgradeCost, GOLD)

	if pathOrTargetMain and pathOrTargetMain.targetHero then
		self:sethero(pathOrTargetMain.targetHero)
		self.initialValue = armyStrength(pathOrTargetMain.heroArmy)
	else
		self:sethero(pathOrTargetMain)
		self.initialValue = armyStrength(pathOrTargetMain)
	end
end

function ArmyUpgrade:equalsTyped(_other)
	return false
end

function ArmyUpgrade:toString()
	return "Army upgrade at " .. objectName(self.upgrader) .. AbstractGoal._helpers.tileToString(visitablePos(self.upgrader))
end

function ArmyUpgrade:getUpgradeValue()
	return self.upgradeValue
end

function ArmyUpgrade:getInitialArmyValue()
	return self.initialValue
end

return ArmyUpgrade
