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

local function stacks(army)
	return army and (army.slots or army.stacks) or {}
end

local function stacksCount(army)
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
	end

	return {
		action = "exchangeSwapTownHeroes",
		town = CGoal.objectID(self.town),
		garrisonHero = CGoal.objectID(targetGarrisonHero),
		lockingReason = self.lockingReason
	}
end

return ExchangeSwapTownHeroes
