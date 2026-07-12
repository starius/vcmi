-- Mirrors AI/Nullkiller2/Goals/DismissHero.{h,cpp}: DismissHero.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local DismissHero = CGoal.derive("DismissHero", AbstractGoal.EGoals.DISMISS_HERO, { elementar = true })

local function heroName(hero)
	return AbstractGoal._helpers.translatedName(hero, tostring(CGoal.objectID(hero)))
end

function DismissHero:init(hero)
	self:sethero(hero)
	self.heroName = heroName(hero)
end

function DismissHero:equalsTyped(other)
	return self.hero == other.hero
end

function DismissHero:accept(aiGw)
	if not self.hero then
		error("Invalid hero!", 2)
	end

	if aiGw and type(aiGw.dismissHero) == "function" then
		return aiGw:dismissHero(self.hero)
	end

	return {
		action = "dismissHero",
		hero = self.hero
	}
end

function DismissHero:toString()
	return "DismissHero " .. self.heroName
end

return DismissHero
