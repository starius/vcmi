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

local function objectID(value)
	return CGoal.objectID(value)
end

local function call(object, name, ...)
	if type(object) == "table" and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function isTownPortal(spellID)
	local spell = type(spellID) == "table" and (spellID.spell or spellID) or nil
	if not spell then
		return false
	end

	return spell.townPortal == true
		or spell.isTownPortal == true
		or spell.townPortalEffect == true
		or spell.effect == "TownPortal"
		or spell.mechanics == "TownPortal"
		or spell.name == "Town Portal"
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town and town.visitingHero
end

local function upperArmy(town)
	return call(town, "getUpperArmy") or town and (town.upperArmy or town.garrisonHero or town)
end

local function stacksCount(army)
	local result = call(army, "stacksCount")
	if result ~= nil then
		return result
	end
	return army and (army.stacksCount or 0) or 0
end

local function ownerOf(object)
	return object and (object.owner or object.tempOwner or object.playerID)
end

local function visitedTown(hero)
	return call(hero, "getVisitedTown") or hero and hero.visitedTown
end

local function isGarrisoned(hero)
	local result = call(hero, "isGarrisoned")
	if result ~= nil then
		return result
	end
	return hero and hero.garrisoned == true
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function canCastAt(aiGw, hero, spellID, tile)
	if aiGw and type(aiGw.canCastSpellAt) == "function" then
		return aiGw:canCastSpellAt(hero, spellID, tile)
	end
	if type(spellID) == "table" and spellID.canCastAt ~= nil then
		return spellID.canCastAt
	end
	return true
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

	local townPortal = self.town ~= nil and isTownPortal(self.spellID)
	if townPortal then
		if aiGw and type(aiGw.setTargetObject) == "function" then
			aiGw:setTargetObject(self.town)
		end

		local occupiedByVisitor = visitingHero(self.town) ~= nil
		local playerID = aiGw and aiGw.nullkiller and aiGw.nullkiller.playerID or aiGw and aiGw.playerID
		if occupiedByVisitor
			and ownerOf(self.town) == playerID
			and stacksCount(upperArmy(self.town)) == 0
			and aiGw
			and type(aiGw.swapGarrisonHero) == "function" then
			aiGw:swapGarrisonHero(self.town)
			occupiedByVisitor = false
		end

		if occupiedByVisitor then
			error("The town is already occupied by " .. AbstractGoal._helpers.translatedName(visitingHero(self.town), tostring(objectID(visitingHero(self.town)))), 2)
		end
	end

	if isGarrisoned(self.hero) and aiGw and type(aiGw.swapGarrisonHero) == "function" then
		local town = visitedTown(self.hero)
		if not town then
			error("Garrisoned hero has no visited town.", 2)
		end
		aiGw:swapGarrisonHero(town)
	end

	if self.tile and self.tile.x ~= -1 and not canCastAt(aiGw, self.hero, self.spellID, self.tile) then
		error("Can not cast " .. spellName(self.spellID) .. " at " .. AbstractGoal._helpers.tileToString(self.tile), 2)
	end

	if aiGw and type(aiGw.castSpell) == "function" then
		local result = aiGw:castSpell(self.hero, self.spellID, self.tile)
		if townPortal and type(aiGw.executeHeroChain) == "function" then
			aiGw:executeHeroChain({
				targetHero = self.hero,
				targetTile = visitablePos(self.town)
			}, objectID(self.town))
		end
		return result
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
