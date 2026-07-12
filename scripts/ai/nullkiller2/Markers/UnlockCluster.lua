-- Mirrors AI/Nullkiller2/Markers/UnlockCluster.{h,cpp}: UnlockCluster.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local UnlockCluster = CGoal.derive("UnlockCluster", AbstractGoal.EGoals.UNLOCK_CLUSTER)

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile) or {}
end

local function tileEquals(lhs, rhs)
	lhs = lhs or {}
	rhs = rhs or {}
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

local function objectName(object)
	return object and (object.objectName or object.name or object.typeName or tostring(CGoal.objectID(object))) or ""
end

function UnlockCluster:init(cluster, pathToCenter)
	self.cluster = cluster
	self.pathToCenter = pathToCenter or {}

	local blocker = cluster and cluster.blocker
	self:settile(visitablePos(blocker))
	self:sethero(self.pathToCenter.targetHero)
	self:setobjid(CGoal.objectID(blocker))
end

function UnlockCluster:equalsTyped(other)
	return tileEquals(self.tile, other.tile)
end

function UnlockCluster:toString()
	return "Unlock Cluster " .. objectName(self.cluster and self.cluster.blocker)
		.. AbstractGoal._helpers.tileToString(self.tile)
end

function UnlockCluster:getCluster()
	return self.cluster
end

function UnlockCluster:getPathToCenter()
	return self.pathToCenter
end

return UnlockCluster
