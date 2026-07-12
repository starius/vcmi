-- Mirrors AI/Nullkiller2/Markers/HeroExchange.{h,cpp}: HeroExchange.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local HeroExchange = CGoal.derive("HeroExchange", AbstractGoal.EGoals.HERO_EXCHANGE)

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectName(object)
	return object and (object.objectName or object.name or tostring(CGoal.objectID(object))) or ""
end

local function pathName(path)
	return call(path, "toString") or path and path.name or ""
end

function HeroExchange:init(targetHero, exchangePath)
	self.exchangePath = exchangePath or {}
	self:sethero(targetHero)
end

function HeroExchange:equalsTyped(_other)
	return false
end

function HeroExchange:toString()
	return "Hero exchange for " .. objectName(self.hero) .. " by " .. pathName(self.exchangePath)
end

function HeroExchange:getReinforcementArmyStrength(aiNk)
	if self.exchangePath.reinforcementArmyStrength ~= nil then
		return self.exchangePath.reinforcementArmyStrength
	end

	local result = call(
		aiNk and aiNk.armyManager,
		"howManyReinforcementsCanGet",
		self.hero,
		self.hero,
		self.exchangePath.heroArmy,
		self.exchangePath.terrainId)
	if result ~= nil then
		return result
	end

	return 0
end

return HeroExchange
