-- Mirrors AI/Nullkiller2/Markers/DefendTown.{h,cpp}: DefendTown.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local DefendTown = CGoal.derive("DefendTown", AbstractGoal.EGoals.DEFEND_TOWN)

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function armyStrength(object)
	return call(object, "getArmyStrength")
		or call(object, "getTotalStrength")
		or object and (object.heroStrength or object.totalStrength or object.armyStrength or object.strength)
		or 0
end

local function pathTurn(path)
	local result = call(path, "turn")
	if result ~= nil then
		return result
	end
	return path and (path.turn or path.turns) or 0
end

local function pathHeroStrength(path)
	return call(path, "getHeroStrength") or path and (path.heroStrength or armyStrength(path.heroArmy)) or 0
end

local function townName(town)
	return town and (town.objectName or town.name or tostring(CGoal.objectID(town))) or ""
end

function DefendTown:init(town, threat, defencePathOrDefender, isCounterAttack)
	self:settown(town)
	self.threat = threat or {}
	self.counterattack = isCounterAttack == true

	if defencePathOrDefender and defencePathOrDefender.targetHero then
		self:sethero(defencePathOrDefender.targetHero)
		self.defenceArmyStrength = pathHeroStrength(defencePathOrDefender)
		self.turn = pathTurn(defencePathOrDefender)
	else
		self:sethero(defencePathOrDefender)
		self.defenceArmyStrength = armyStrength(defencePathOrDefender)
		self.turn = 0
	end
end

function DefendTown:equalsTyped(_other)
	return false
end

function DefendTown:toString()
	return "Defend town " .. townName(self.town)
end

function DefendTown:getThreat()
	return self.threat
end

function DefendTown:getDefenceStrength()
	return self.defenceArmyStrength
end

function DefendTown:getTurn()
	return self.turn
end

function DefendTown:isCounterAttack()
	return self.counterattack
end

return DefendTown
