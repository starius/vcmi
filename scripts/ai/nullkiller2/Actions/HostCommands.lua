-- Host action adapter for Lua Nullkiller2 policy.
--
-- Lua policy records every intended command here. The C++ host may execute a
-- command, but decision code still owns which command is requested and with
-- which payload.

local State = require("Engine.State")

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

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function targetTile(path)
	local value = call(path, "targetTile")
	if value ~= nil then
		return value
	end
	return path and (path.targetTile or path.tile) or nil
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function tileEquals(lhs, rhs)
	lhs = lhs or {}
	rhs = rhs or {}
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

local function movementPoints(hero)
	local value = call(hero, "movementPointsRemaining")
	if value ~= nil then
		return value
	end
	return hero and (hero.movementPointsRemaining or hero.movementPoints or 0) or 0
end

local function pathNodes(path)
	return path and path.nodes or {}
end

local function nodeCoord(path, node)
	return node and (node.coord or node.targetTile or node.tile) or targetTile(path)
end

local function copyTile(tile)
	if not tile then
		return nil
	end
	return {
		x = tile.x,
		y = tile.y,
		z = tile.z
	}
end

local function setVisitablePos(hero, tile)
	if type(hero) == "table" then
		hero.visitablePos = copyTile(tile)
	end
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

function HostCommands:upgradeCreature(army, slot, creature)
	return self:command("upgradeCreature", {
		army = objectID(army),
		slot = slot,
		creature = objectID(creature)
	})
end

function HostCommands:mergeStacks(army, fromSlot, toSlot)
	return self:command("mergeStacks", {
		army = objectID(army),
		fromSlot = fromSlot,
		toSlot = toSlot
	})
end

function HostCommands:mergeOrSwapStacks(source, destination, fromSlot, toSlot)
	return self:command("mergeOrSwapStacks", {
		src = objectID(source),
		dst = objectID(destination),
		fromSlot = fromSlot,
		toSlot = toSlot
	})
end

function HostCommands:splitStack(source, destination, fromSlot, toSlot, count)
	return self:command("splitStack", {
		src = objectID(source),
		dst = objectID(destination),
		fromSlot = fromSlot,
		toSlot = toSlot,
		count = count
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

function HostCommands:setTargetObject(object)
	local result = self:command("setTargetObject", {
		objid = objectID(object)
	})
	if self.nullkiller and type(self.nullkiller.setTargetObject) == "function" then
		self.nullkiller:setTargetObject(objectID(object))
	end
	result.state = "setTargetObject"
	return result
end

function HostCommands:setActive(hero, tile)
	local result = self:command("setActive", {
		hero = objectID(hero),
		x = tile and tile.x,
		y = tile and tile.y,
		z = tile and tile.z
	})
	if self.nullkiller and type(self.nullkiller.setActive) == "function" then
		self.nullkiller:setActive(hero, tile)
	end
	result.state = "setActive"
	return result
end

function HostCommands:invalidatePathfinderData()
	local result = self:command("invalidatePathfinderData", {})
	if self.nullkiller and type(self.nullkiller.invalidatePathfinderData) == "function" then
		self.nullkiller:invalidatePathfinderData()
	elseif self.nullkiller then
		self.nullkiller.pathfinderInvalidated = true
	end
	result.state = "invalidatePathfinderData"
	return result
end

function HostCommands:resetObjectClusterizer()
	local result = self:command("resetObjectClusterizer", {})
	if self.nullkiller and self.nullkiller.objectClusterizer and type(self.nullkiller.objectClusterizer.reset) == "function" then
		self.nullkiller.objectClusterizer:reset()
	end
	result.state = "resetObjectClusterizer"
	return result
end

function HostCommands:moveHeroToTile(tile, hero)
	if tileEquals(visitablePos(hero), tile) then
		return {
			ok = true,
			state = "moveHeroToTile",
			skipped = true,
			hero = objectID(hero),
			tile = tile
		}
	end

	local result = self:command("moveHeroToTile", {
		hero = objectID(hero),
		x = tile and tile.x,
		y = tile and tile.y,
		z = tile and tile.z
	})

	if result.ok ~= false then
		setVisitablePos(hero, tile)
	end
	result.state = "moveHeroToTile"
	return result
end

local function lockBlockedHero(adapter, blockedIndexes, hero, parentIndex)
	if parentIndex ~= nil then
		blockedIndexes[parentIndex] = true
	end
	if hero then
		adapter:lockHero(hero, State.HeroLockedReason.HERO_CHAIN)
	end
end

local function executePathNode(adapter, path, node, blockedIndexes, cxxIndex)
	local hero = node.targetHero or path and path.targetHero
	local coord = nodeCoord(path, node)

	if blockedIndexes[cxxIndex] then
		lockBlockedHero(adapter, blockedIndexes, hero, node.parentIndex)
		return {
			ok = true,
			blocked = true
		}
	end

	if not hero or objectID(hero) == nil or not coord then
		return {
			ok = false,
			error = "hero chain node is missing hero or coordinate"
		}
	end

	if movementPoints(hero) > 0 then
		adapter:setActive(hero, coord)

		if node.specialAction then
			lockBlockedHero(adapter, blockedIndexes, hero, node.parentIndex)
			adapter:invalidatePathfinderData()
			error("Path special actions are not implemented by Lua Nullkiller2 yet.", 3)
		end

		if not tileEquals(coord, visitablePos(hero)) then
			local moveResult = adapter:moveHeroToTile(coord, hero)
			if moveResult.ok == false then
				lockBlockedHero(adapter, blockedIndexes, hero, node.parentIndex)
				return moveResult
			end
		end
	end

	if tileEquals(coord, visitablePos(hero)) then
		return {
			ok = true
		}
	end

	if (node.turns or node.turn or 0) == 0 then
		return {
			ok = false,
			error = "unable to complete zero-turn hero chain node"
		}
	end

	lockBlockedHero(adapter, blockedIndexes, hero, node.parentIndex)
	return {
		ok = true,
		blocked = true
	}
end

function HostCommands:executeHeroChain(path, objid)
	local nodes = pathNodes(path)
	if #nodes == 0 then
		local tile = targetTile(path)
		self:setActive(path and path.targetHero, tile)
		return self:moveHeroToTile(tile, path and path.targetHero)
	end

	local blockedIndexes = {}
	local result = {
		ok = true,
		state = "executeHeroChain",
		objid = objid
	}

	for luaIndex = #nodes, 1, -1 do
		local nodeResult = executePathNode(self, path, nodes[luaIndex] or {}, blockedIndexes, luaIndex - 1)
		if nodeResult.ok == false then
			return nodeResult
		end
		result = nodeResult
	end

	result.state = "executeHeroChain"
	result.objid = objid
	return result
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
