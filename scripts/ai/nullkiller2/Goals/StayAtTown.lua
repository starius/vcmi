-- Mirrors AI/Nullkiller2/Goals/StayAtTown.{h,cpp}: StayAtTown.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local State = require("Engine.State")

local StayAtTown = CGoal.derive("StayAtTown", AbstractGoal.EGoals.STAY_AT_TOWN, { elementar = true })

local function manaLimit(hero)
	if type(hero) == "table" then
		if type(hero.manaLimit) == "function" then
			return hero:manaLimit()
		end
		return hero.manaLimit or 0
	end
	return 0
end

function StayAtTown:init(town, path)
	path = path or {}
	self:sethero(path.targetHero)
	self:settown(town)

	local hero = self.hero or {}
	local remaining = hero.movementPointsRemaining or 0
	local limit = hero.movementPointsLimit or 1
	self.movementWasted = remaining / limit - (path.movementCost or 0)
	if self.movementWasted < 0 then
		self.movementWasted = 0
	end
end

function StayAtTown:equalsTyped(other)
	return self.hero == other.hero and self.town == other.town
end

function StayAtTown:toString()
	return "Stay at town " .. AbstractGoal._helpers.translatedName(self.town, tostring(CGoal.objectID(self.town)))
		.. " hero " .. AbstractGoal._helpers.translatedName(self.hero, tostring(CGoal.objectID(self.hero)))
		.. ", mana: " .. tostring(self.hero and self.hero.mana or 0)
		.. " / " .. tostring(manaLimit(self.hero))
end

function StayAtTown:accept(aiGw)
	if aiGw and aiGw.nullkiller and type(aiGw.nullkiller.lockHero) == "function" then
		return aiGw.nullkiller:lockHero(self.hero, State.HeroLockedReason.DEFENCE)
	end

	if aiGw and type(aiGw.lockHero) == "function" then
		return aiGw:lockHero(self.hero, State.HeroLockedReason.DEFENCE)
	end

	return {
		action = "lockHero",
		hero = self.hero,
		reason = State.HeroLockedReason.DEFENCE
	}
end

function StayAtTown:getMovementWasted()
	return self.movementWasted
end

return StayAtTown
