-- Mirrors AI/Nullkiller2/Goals/ExchangeSwapTownHeroes.{h,cpp}: ExchangeSwapTownHeroes.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local State = require("Engine.State")

local ExchangeSwapTownHeroes = CGoal.derive(
	"ExchangeSwapTownHeroes",
	AbstractGoal.EGoals.EXCHANGE_SWAP_TOWN_HEROES,
	{ elementar = true })

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
	if aiGw and type(aiGw.exchangeSwapTownHeroes) == "function" then
		return aiGw:exchangeSwapTownHeroes(self)
	end

	return {
		action = "exchangeSwapTownHeroes",
		town = CGoal.objectID(self.town),
		garrisonHero = CGoal.objectID(self.garrisonHero),
		lockingReason = self.lockingReason
	}
end

return ExchangeSwapTownHeroes
