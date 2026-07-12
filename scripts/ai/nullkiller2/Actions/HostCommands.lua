-- Host action adapter for Lua Nullkiller2 policy.
--
-- Lua policy records every intended command here. The C++ host may execute a
-- command, but decision code still owns which command is requested and with
-- which payload.

local State = require("Engine.State")
local ArmyFormation = require("Helpers.ArmyFormation")

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

local function pathTurn(path)
	local value = call(path, "turn")
	if value ~= nil then
		return value
	end
	return path and (path.turn or path.turns) or 0
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile)
end

local function isTown(object)
	return object and (object.isTown == true
		or object.ID == "TOWN"
		or object.type == "TOWN"
		or object.typeName == "TOWN")
end

local function isEnemy(object, actor)
	if not object then
		return false
	end
	if object.enemy ~= nil then
		return object.enemy == true
	end
	local relation = object.relations or object.relationsName or object.relation
	if relation ~= nil then
		return relation == "ENEMIES" or relation == 2
	end
	return object.owner ~= nil and actor and actor.owner ~= nil and object.owner ~= actor.owner
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

local function pathInfo(adapter, hero, coord, node)
	if node then
		local value = node.livePathInfo or node.pathInfo or node.livePath
		if value ~= nil then
			return value
		end
	end
	if adapter.host and type(adapter.host.getPathInfo) == "function" then
		return adapter.host:getPathInfo(hero, coord)
	end
	return nil
end

local function pathInfoTurns(value)
	if type(value) ~= "table" then
		return nil
	end
	return value.turns or value.turn
end

local function pathInfoAccessible(value)
	if type(value) ~= "table" then
		return nil
	end
	return value.accessible or value.accessibility
end

local function pathInfoReachable(value)
	if type(value) ~= "table" then
		return false
	end
	if type(value.reachable) == "function" then
		return value:reachable()
	end
	if value.reachable ~= nil then
		return value.reachable == true
	end
	local accessible = pathInfoAccessible(value)
	return accessible == true or accessible == "ACCESSIBLE"
end

local function pathInfoCost(value)
	if type(value) ~= "table" then
		return nil
	end
	if type(value.getCost) == "function" then
		return value:getCost()
	end
	return value.cost or value.pathCost
end

local function pathInfoAction(value)
	if type(value) ~= "table" then
		return nil
	end
	return value.action or value.nodeAction
end

local function normalizedActionName(action)
	if type(action) ~= "table" then
		return ""
	end

	local name = action.type or action.kind or action.name or action.class or action.ID or ""
	return string.gsub(string.lower(tostring(name)), "[%s_%-]", "")
end

local function actionParts(action)
	if type(action) ~= "table" then
		return {}
	end

	return action.parts or action.actions or action.children or {}
end

local function isDimensionDoorAction(action)
	local name = normalizedActionName(action)
	return action.dimensionDoor == true
		or name == "dimensiondoor"
		or name == "dimensiondooraction"
end

local function isAdventureCastAction(action)
	local name = normalizedActionName(action)
	return name == "adventurecastaction"
		or name == "waterwalkingaction"
		or name == "airwalkingaction"
		or name == "summonboataction"
		or name == "townportalaction"
end

local function isBuildBoatAction(action)
	local name = normalizedActionName(action)
	return name == "buildboataction"
		or name == "buildboat"
end

local function isWhirlpoolAction(action)
	local name = normalizedActionName(action)
	return name == "whirlpoolaction"
		or name == "whirlpool"
end

local function isMoveToTileAction(action)
	local name = normalizedActionName(action)
	return name == "battleaction"
		or name == "questaction"
end

local function actionDestination(action, fallback)
	if type(action) ~= "table" then
		return fallback
	end
	local candidate = action.destination or action.targetTile or action.tile or action.coord
	if type(candidate) == "table" and candidate.x ~= nil then
		return candidate
	end

	local object = action.targetObject or action.questObject or action.object or action.target
	return visitablePos(object) or fallback
end

local function actionSpell(action)
	if type(action) ~= "table" then
		return nil
	end
	return action.spell or action.spellID or action.spellId or action.usedSpell or action.usedSpellID or action.spellToCast
end

local function actionShipyard(action)
	if type(action) ~= "table" then
		return nil
	end
	return action.shipyard or action.shipyardID or action.shipyardId or action.targetObject or action.object or action.target
end

local function actionCommand(action)
	if type(action) ~= "table" then
		return nil, nil
	end

	if type(action.command) == "table" then
		return action.command.name, action.command.payload or {}
	end

	return action.command or action.commandName, action.payload or action.commandPayload or {}
end

local function inaccessibleForZeroTurn(value)
	local accessible = pathInfoAccessible(value)
	return accessible == "NOT_SET"
		or accessible == "BLOCKED"
		or accessible == "FLYABLE"
		or accessible == false
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

function HostCommands:answerQuery(query, selection)
	return self:command("answerQuery", {
		query = query,
		selection = selection or 0
	})
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

function HostCommands:swapCreatures(source, destination, fromSlot, toSlot)
	return self:command("swapCreatures", {
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

function HostCommands:swapArtifacts(sourceHero, sourceSlot, destinationHero, destinationSlot)
	return self:command("swapArtifacts", {
		srcHero = objectID(sourceHero),
		srcSlot = sourceSlot,
		dstHero = objectID(destinationHero),
		dstSlot = destinationSlot
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

function HostCommands:rearrangeArmyForSiege(town, attacker, path)
	if pathTurn(path) == 0 and isTown(town) and isEnemy(town, attacker) then
		ArmyFormation.rearrangeArmyForSiege(self, town, attacker)
		return {
			ok = true,
			state = "rearrangeArmyForSiege",
			executed = true
		}
	end

	return {
		ok = true,
		state = "rearrangeArmyForSiege",
		skipped = true
	}
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

local function objectGraphAllowed(adapter)
	if adapter.nullkiller and type(adapter.nullkiller.isObjectGraphAllowed) == "function" then
		return adapter.nullkiller:isObjectGraphAllowed() == true
	end
	if adapter.host and type(adapter.host.isObjectGraphAllowed) == "function" then
		return adapter.host:isObjectGraphAllowed() == true
	end
	if adapter.host and adapter.host.objectGraphAllowed ~= nil then
		return adapter.host.objectGraphAllowed == true
	end
	return false
end

local function recoverStaleDimensionDoorAction(adapter, hero)
	if not hero or objectID(hero) == nil then
		return {
			ok = false,
			stale = true,
			lostHero = true,
			error = "hero was lost trying to execute Dimension Door"
		}
	end

	adapter:lockHero(hero, State.HeroLockedReason.HERO_CHAIN)
	adapter:invalidatePathfinderData()
	return {
		ok = false,
		stale = true,
		error = "stale Dimension Door hero chain action"
	}
end

local function executeSpecialAction(adapter, hero, coord, action)
	for _, part in ipairs(actionParts(action)) do
		local partResult = executeSpecialAction(adapter, hero, coord, part)
		if partResult.ok == false then
			return partResult
		end
	end

	if #actionParts(action) > 0 then
		return {
			ok = true
		}
	end

	if action.cannotFulfill or action.stale then
		if isDimensionDoorAction(action) then
			return recoverStaleDimensionDoorAction(adapter, hero)
		end
		error("Can not execute " .. tostring(action.name or action.type or "special action"), 3)
	end

	if isDimensionDoorAction(action) then
		local spell = actionSpell(action)
		if spell == nil then
			error("Dimension Door special action is missing spell id", 3)
		end

		local result = adapter:castSpell(hero, spell, actionDestination(action, coord))
		if result.ok == false then
			return recoverStaleDimensionDoorAction(adapter, hero)
		end

		setVisitablePos(hero, actionDestination(action, coord))
		return result
	end

	if isAdventureCastAction(action) then
		local spell = actionSpell(action)
		if spell == nil then
			error("Adventure spell special action is missing spell id", 3)
		end
		return adapter:castSpell(hero, spell, actionDestination(action))
	end

	if isBuildBoatAction(action) then
		local shipyard = actionShipyard(action)
		if shipyard == nil then
			error("Build Boat special action is missing shipyard id", 3)
		end
		return adapter:buildBoat(shipyard)
	end

	if isWhirlpoolAction(action) then
		ArmyFormation.rearrangeArmyForWhirlpool(adapter, hero)
		return {
			ok = true,
			state = "whirlpoolAction"
		}
	end

	if isMoveToTileAction(action) then
		local tile = actionDestination(action, coord)
		if tile == nil then
			error("Move-to-tile special action is missing target tile", 3)
		end
		return adapter:moveHeroToTile(tile, hero)
	end

	local commandName, payload = actionCommand(action)
	if commandName then
		local commandPayload = copyPayload(payload)
		if commandPayload.hero == nil then
			commandPayload.hero = objectID(hero)
		end
		return adapter:command(commandName, commandPayload)
	end

	error("Path special action is not implemented by Lua Nullkiller2 yet.", 3)
end

local function objectGraphShortcut(adapter, path, nodes, luaIndex, node, hero)
	if node.specialAction or luaIndex <= 1 or not objectGraphAllowed(adapter) then
		return luaIndex, node
	end

	local chainMask = node.chainMask
	if luaIndex < #nodes and nodes[luaIndex + 1] then
		chainMask = nodes[luaIndex + 1].chainMask
	end

	for nextLuaIndex = luaIndex - 1, 1, -1 do
		local nextNode = nodes[nextLuaIndex]
		if not nextNode or nextNode.specialAction or nextNode.chainMask ~= chainMask then
			break
		end

		local targetNode = pathInfo(adapter, hero, nodeCoord(path, nextNode), nextNode)
		local targetCost = pathInfoCost(targetNode)
		if not pathInfoReachable(targetNode) or targetCost == nil or targetCost > (nextNode.cost or 0) then
			break
		end

		luaIndex = nextLuaIndex
		node = nextNode

		local action = pathInfoAction(targetNode)
		if action == "BATTLE" or action == "TELEPORT_BATTLE" then
			break
		end
	end

	return luaIndex, node
end

local function executePathNode(adapter, path, nodes, luaIndex, blockedIndexes)
	local node = nodes[luaIndex] or {}
	local hero = node.targetHero or path and path.targetHero
	local coord = nodeCoord(path, node)
	local cxxIndex = luaIndex - 1

	if blockedIndexes[cxxIndex] then
		lockBlockedHero(adapter, blockedIndexes, hero, node.parentIndex)
		return {
			ok = true,
			nextLuaIndex = luaIndex,
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
			if node.actionIsBlocked then
				lockBlockedHero(adapter, blockedIndexes, hero, node.parentIndex)
				adapter:invalidatePathfinderData()
				error("Path is nondeterministic.", 3)
			end

			local actionResult = executeSpecialAction(adapter, hero, coord, node.specialAction)
			if actionResult.ok == false then
				return actionResult
			end

			if tileEquals(coord, visitablePos(hero)) then
				return {
					ok = true,
					nextLuaIndex = luaIndex
				}
			end
		else
			local shortcutLuaIndex
			shortcutLuaIndex, node = objectGraphShortcut(adapter, path, nodes, luaIndex, node, hero)
			luaIndex = shortcutLuaIndex
			coord = nodeCoord(path, node)
		end

		if (node.turns or node.turn or 0) == 0 and not tileEquals(coord, visitablePos(hero)) then
			local livePathInfo = pathInfo(adapter, hero, coord, node)
			if livePathInfo and (inaccessibleForZeroTurn(livePathInfo) or pathInfoTurns(livePathInfo) ~= nil and pathInfoTurns(livePathInfo) ~= 0) then
				return {
					ok = false,
					stale = true,
					error = "stale zero-turn hero chain node"
				}
			end
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
			ok = true,
			nextLuaIndex = luaIndex
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
		nextLuaIndex = luaIndex,
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

	local luaIndex = #nodes
	while luaIndex >= 1 do
		local nodeResult = executePathNode(self, path, nodes, luaIndex, blockedIndexes)
		if nodeResult.ok == false then
			return nodeResult
		end
		result = nodeResult
		luaIndex = nodeResult.nextLuaIndex or luaIndex
		luaIndex = luaIndex - 1
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
