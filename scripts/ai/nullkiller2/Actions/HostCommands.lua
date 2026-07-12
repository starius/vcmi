-- Host action adapter for Lua Nullkiller2 policy.
--
-- Lua policy records every intended command here. The C++ host may execute a
-- command, but decision code still owns which command is requested and with
-- which payload.

local HostCommands = {}
HostCommands.__index = HostCommands

local function objectID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function spellID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.num or objectID(value.spell)
	end
	return value
end

local function copyPayload(payload)
	local result = {}
	for key, value in pairs(payload or {}) do
		result[key] = value
	end
	return result
end

function HostCommands.new(host)
	return setmetatable({
		host = host,
		journal = {}
	}, HostCommands)
end

function HostCommands:command(name, payload)
	local entry = {
		name = name,
		payload = copyPayload(payload)
	}
	table.insert(self.journal, entry)

	if self.host and type(self.host.command) == "function" then
		local result = self.host:command(name, entry.payload)
		if result ~= nil then
			return result
		end
	end

	return {
		ok = true,
		queued = true,
		commandIndex = #self.journal
	}
end

function HostCommands:trace(event, data)
	if self.host and type(self.host.trace) == "function" then
		return self.host:trace(event, data or {})
	end
	return nil
end

function HostCommands:endTurn()
	local result = self:command("endTurn", {})
	if self.host and type(self.host.endTurn) == "function" then
		return self.host:endTurn()
	end
	return result
end

function HostCommands:buildBuilding(town, bid)
	return self:command("buildBuilding", {
		town = objectID(town),
		bid = objectID(bid)
	})
end

function HostCommands:buildBoat(shipyard)
	return self:command("buildBoat", {
		shipyard = objectID(shipyard)
	})
end

function HostCommands:dismissHero(hero)
	return self:command("dismissHero", {
		hero = objectID(hero)
	})
end

function HostCommands:recruitHero(town, hero)
	return self:command("recruitHero", {
		town = objectID(town),
		hero = objectID(hero)
	})
end

function HostCommands:recruitCreatures(town, destination, creature, count, level)
	return self:command("recruitCreatures", {
		town = objectID(town),
		dst = objectID(destination),
		creature = objectID(creature),
		count = count,
		level = level
	})
end

function HostCommands:dismissCreature(army, slot)
	return self:command("dismissCreature", {
		army = objectID(army),
		slot = slot
	})
end

function HostCommands:castSpell(hero, spell, tile)
	return self:command("castSpell", {
		hero = objectID(hero),
		spell = spellID(spell),
		x = tile and tile.x,
		y = tile and tile.y,
		z = tile and tile.z
	})
end

function HostCommands:lockResources(resources)
	return self:command("lockResources", {
		resources = resources
	})
end

function HostCommands:lockHero(hero, reason)
	if self.nullkiller and type(self.nullkiller.lockHero) == "function" then
		self.nullkiller:lockHero(hero, reason)
	end
	return {
		ok = true,
		state = "lockHero",
		hero = objectID(hero),
		reason = reason
	}
end

function HostCommands:unlockHero(hero)
	if self.nullkiller and type(self.nullkiller.unlockHero) == "function" then
		self.nullkiller:unlockHero(hero)
	end
	return {
		ok = true,
		state = "unlockHero",
		hero = objectID(hero)
	}
end

function HostCommands:executeHeroChain(path, objid)
	local tile = nil
	if path then
		if type(path.targetTile) == "function" then
			tile = path:targetTile()
		else
			tile = path.targetTile or path.tile
		end
	end

	return self:command("executeHeroChain", {
		hero = objectID(path and path.targetHero),
		objid = objid,
		x = tile and tile.x,
		y = tile and tile.y,
		z = tile and tile.z,
		path = path
	})
end

function HostCommands:getAvailableHeroes(town)
	if self.host and type(self.host.getAvailableHeroes) == "function" then
		return self.host:getAvailableHeroes(town)
	end
	return town and town.availableHeroes or {}
end

function HostCommands:getFreeResources()
	if self.nullkiller and type(self.nullkiller.getFreeResources) == "function" then
		return self.nullkiller:getFreeResources()
	end
	if self.host and type(self.host.getFreeResources) == "function" then
		return self.host:getFreeResources()
	end
	return self.freeResources or {}
end

function HostCommands:swapGarrisonHero(town)
	return self:command("swapGarrisonHero", {
		town = objectID(town)
	})
end

function HostCommands:getJournal()
	return self.journal
end

return HostCommands
