-- Mirrors AI/Nullkiller2/Goals/ExecuteHeroChain.{h,cpp}: ExecuteHeroChain identity and command intent.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local State = require("Engine.State")

local ExecuteHeroChain = CGoal.derive("ExecuteHeroChain", AbstractGoal.EGoals.EXECUTE_HERO_CHAIN, { elementar = true })

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function targetTile(path)
	local value = call(path, "targetTile")
	if value ~= nil then
		return value
	end
	return path and (path.targetTile or path.tile) or {}
end

local function tileEquals(lhs, rhs)
	lhs = lhs or {}
	rhs = rhs or {}
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

local function tileToString(tile)
	return AbstractGoal._helpers.tileToString(tile or {})
end

local function objectTypeName(object)
	if not object then
		return nil
	end
	return object.objectName or object.name or object.typeName or object.ID or object.idType or object.type
end

local function pathNodes(path)
	return path and path.nodes or {}
end

local function hasBlockedSpecialAction(path)
	for index = #pathNodes(path), 1, -1 do
		local node = pathNodes(path)[index]
		if node and node.specialAction and node.actionIsBlocked then
			return node
		end
	end
	return nil
end

local function removeDuplicateIDs(values)
	local result = {}
	local seen = {}
	for _, value in ipairs(values) do
		if value ~= nil and not seen[value] then
			seen[value] = true
			table.insert(result, value)
		end
	end
	return result
end

function ExecuteHeroChain:init(path, object)
	self.chainPath = path or {}
	self.targetObject = object
	self.closestWayRatio = 1
	self:sethero(self.chainPath.targetHero)
	self:settile(targetTile(self.chainPath))

	if object then
		self:setobjid(objectID(object))
		self.targetName = tostring(objectTypeName(object)) .. tileToString(self.tile)
	else
		self.targetName = "tile" .. tileToString(self.tile)
	end
end

function ExecuteHeroChain:equalsTyped(other)
	return tileEquals(self.tile, other.tile)
		and self.chainPath.targetHero == other.chainPath.targetHero
		and #pathNodes(self.chainPath) == #pathNodes(other.chainPath)
		and self.chainPath.chainMask == other.chainPath.chainMask
end

function ExecuteHeroChain:getHeroExchangeCount()
	return self.chainPath.exchangeCount or 0
end

function ExecuteHeroChain:getPath()
	return self.chainPath
end

function ExecuteHeroChain:getAffectedObjects()
	local affectedObjects = { objectID(self.chainPath.targetHero) }

	if self.objid ~= -1 then
		table.insert(affectedObjects, self.objid)
	end

	for _, node in ipairs(pathNodes(self.chainPath)) do
		if node.targetHero then
			table.insert(affectedObjects, objectID(node.targetHero))
		end
	end

	return removeDuplicateIDs(affectedObjects)
end

function ExecuteHeroChain:isObjectAffected(id)
	local targetID = objectID(id)

	if objectID(self.chainPath.targetHero) == targetID or self.objid == targetID then
		return true
	end

	for _, node in ipairs(pathNodes(self.chainPath)) do
		if node.targetHero and objectID(node.targetHero) == targetID then
			return true
		end
	end

	return false
end

function ExecuteHeroChain:accept(aiGw)
	if aiGw and type(aiGw.setActive) == "function" then
		aiGw:setActive(self.chainPath.targetHero, self.tile)
	end

	if aiGw and type(aiGw.setTargetObject) == "function" then
		aiGw:setTargetObject(self.objid)
	end

	if aiGw and type(aiGw.resetObjectClusterizer) == "function" then
		aiGw:resetObjectClusterizer()
	end

	if aiGw and type(aiGw.rearrangeArmyForSiege) == "function" then
		aiGw:rearrangeArmyForSiege(self.targetObject, self.chainPath.targetHero, self.chainPath)
	end

	local blockedNode = hasBlockedSpecialAction(self.chainPath)
	if blockedNode then
		if aiGw and type(aiGw.lockHero) == "function" then
			aiGw:lockHero(blockedNode.targetHero or self.chainPath.targetHero, State.HeroLockedReason.HERO_CHAIN)
		end
		if aiGw and type(aiGw.invalidatePathfinderData) == "function" then
			aiGw:invalidatePathfinderData()
		end
		error("Path is nondeterministic.", 2)
	end

	if aiGw and type(aiGw.executeHeroChain) == "function" then
		return aiGw:executeHeroChain(self.chainPath, self.objid)
	end

	return {
		action = "executeHeroChain",
		hero = self.hero,
		tile = self.tile,
		objid = self.objid,
		path = self.chainPath
	}
end

function ExecuteHeroChain:toString()
	return "ExecuteHeroChain " .. self.targetName
		.. " by " .. AbstractGoal._helpers.translatedName(self.chainPath.targetHero, tostring(objectID(self.chainPath.targetHero)))
end

return ExecuteHeroChain
