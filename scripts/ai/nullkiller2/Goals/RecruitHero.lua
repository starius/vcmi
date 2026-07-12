-- Mirrors AI/Nullkiller2/Goals/RecruitHero.{h,cpp}: RecruitHero.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local RecruitHero = CGoal.derive("RecruitHero", AbstractGoal.EGoals.RECRUIT_HERO, { elementar = true })

local function nameOf(value)
	return AbstractGoal._helpers.translatedName(value, tostring(CGoal.objectID(value)))
end

local function strengthOf(hero)
	if not hero then
		return 0
	end
	if type(hero.getTotalStrength) == "function" then
		local ok, result = pcall(hero.getTotalStrength, hero)
		if ok and result then
			return result
		end
	end
	return hero.totalStrength or hero.strength or 0
end

local function availableHeroes(aiGw, town)
	if aiGw and type(aiGw.getAvailableHeroes) == "function" then
		return aiGw:getAvailableHeroes(town)
	end
	return town and town.availableHeroes or {}
end

local function hasVisitingHero(town)
	if not town then
		return false
	end
	if type(town.getVisitingHero) == "function" then
		local ok, result = pcall(town.getVisitingHero, town)
		return ok and result ~= nil
	end
	return town.visitingHero ~= nil
end

function RecruitHero:init(townWithTavern, heroToBuy)
	self.heroToBuy = heroToBuy
	self.town = townWithTavern
	self.priority = 1
end

function RecruitHero:equalsTyped(_other)
	return true
end

function RecruitHero:toString()
	if self.heroToBuy then
		return "Recruit " .. nameOf(self.heroToBuy) .. " at " .. nameOf(self.town)
	end

	return "Recruit hero at " .. nameOf(self.town)
end

function RecruitHero:getHero()
	return self.heroToBuy
end

function RecruitHero:accept(aiGw)
	local town = self.town
	if not town then
		error("No town to recruit hero!", 2)
	end

	local heroes = availableHeroes(aiGw, town)
	if not heroes or #heroes == 0 then
		error("No available heroes in tavern in " .. nameOf(town), 2)
	end

	local heroToHire = self.heroToBuy
	if not heroToHire then
		for _, candidate in ipairs(heroes) do
			if not heroToHire or strengthOf(candidate) > strengthOf(heroToHire) then
				heroToHire = candidate
			end
		end
	end

	if not heroToHire then
		error("No hero to hire!", 2)
	end

	if hasVisitingHero(town) and aiGw and type(aiGw.swapGarrisonHero) == "function" then
		aiGw:swapGarrisonHero(town)
	end

	if hasVisitingHero(town) then
		error("Town " .. nameOf(town) .. " is occupied. Cannot recruit hero!", 2)
	end

	if aiGw and type(aiGw.recruitHero) == "function" then
		return aiGw:recruitHero(town, heroToHire)
	end

	return {
		action = "recruitHero",
		town = town,
		hero = heroToHire
	}
end

return RecruitHero
