-- Mirrors AI/Nullkiller2/Goals/AdventureSpellCast.{h,cpp}: AdventureSpellCast.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local AdventureSpellCast = CGoal.derive("AdventureSpellCast", AbstractGoal.EGoals.ADVENTURE_SPELL_CAST, { elementar = true })

local function sameTile(lhs, rhs)
	lhs = lhs or {}
	rhs = rhs or {}
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

local function spellName(spellID)
	if type(spellID) == "table" then
		if spellID.spell then
			return AbstractGoal._helpers.translatedName(spellID.spell, tostring(spellID.id or spellID.num))
		end
		return AbstractGoal._helpers.translatedName(spellID, tostring(spellID.id or spellID.num))
	end
	return tostring(spellID)
end

local function isAdventure(spellID)
	if type(spellID) == "table" then
		if spellID.isAdventure ~= nil then
			return spellID.isAdventure
		end
		if spellID.spell and spellID.spell.isAdventure ~= nil then
			return spellID.spell.isAdventure
		end
	end
	return true
end

local function spellCost(hero, spellID)
	if type(hero) == "table" and type(hero.getSpellCost) == "function" then
		local ok, result = pcall(hero.getSpellCost, hero, spellID)
		if ok and result then
			return result
		end
	end
	if type(spellID) == "table" then
		return spellID.cost or spellID.spellCost or 0
	end
	return 0
end

function AdventureSpellCast:init(hero, spellID)
	self.spellID = spellID
	self:sethero(hero)
end

function AdventureSpellCast:getSpell()
	if type(self.spellID) == "table" then
		return self.spellID.spell or self.spellID
	end
	return self.spellID
end

function AdventureSpellCast:equalsTyped(other)
	return self.hero == other.hero
		and self.spellID == other.spellID
		and sameTile(self.tile, other.tile)
		and self.town == other.town
end

function AdventureSpellCast:accept(aiGw)
	if not self.hero then
		error("Invalid hero!", 2)
	end

	if not isAdventure(self.spellID) then
		error(spellName(self.spellID) .. " is not an adventure spell.", 2)
	end

	if self.hero.canCastThisSpell == false then
		error("Hero can not cast " .. spellName(self.spellID), 2)
	end

	if (self.hero.mana or 0) < spellCost(self.hero, self.spellID) then
		error("Hero has not enough mana to cast " .. spellName(self.spellID), 2)
	end

	if aiGw and type(aiGw.castSpell) == "function" then
		return aiGw:castSpell(self.hero, self.spellID, self.tile)
	end

	return {
		action = "castSpell",
		hero = self.hero,
		spellID = self.spellID,
		tile = self.tile
	}
end

function AdventureSpellCast:toString()
	return "AdventureSpellCast " .. spellName(self.spellID)
end

return AdventureSpellCast
