-- Mirrors AI/Nullkiller2/Behaviors/ClusterBehavior.{h,cpp}: ClusterBehavior.

local AbstractGoal = require("Goals.AbstractGoal")
local CaptureObjectsBehavior = require("Behaviors.CaptureObjectsBehavior")
local CGoal = require("Goals.CGoal")
local Composition = require("Goals.Composition")
local UnlockCluster = require("Markers.UnlockCluster")

local ClusterBehavior = CGoal.derive("ClusterBehavior", AbstractGoal.EGoals.CLUSTER_BEHAVIOR)

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	return CGoal.objectID(value)
end

local function visitablePos(object)
	return call(object, "visitablePos") or object and (object.visitablePos or object.tile) or {}
end

local function tileEquals(lhs, rhs)
	lhs = lhs or {}
	rhs = rhs or {}
	return lhs.x == rhs.x and lhs.y == rhs.y and lhs.z == rhs.z
end

local function sameObject(lhs, rhs)
	if lhs == rhs then
		return true
	end
	return objectID(lhs) ~= nil and objectID(lhs) == objectID(rhs)
end

local function lockedClusters(aiNk)
	return call(aiNk and aiNk.objectClusterizer, "getLockedClusters") or aiNk and aiNk.lockedClusters or {}
end

local function clusterCenter(aiNk, cluster)
	return call(cluster, "calculateCenter", aiNk and aiNk.cc) or cluster.center
end

local function pathInfo(aiNk, center)
	return call(aiNk and aiNk.pathfinder, "getPathInfo", visitablePos(center), call(aiNk, "isObjectGraphAllowed") or false)
		or center and center.paths
		or {}
end

local function blockerForPath(aiNk, path)
	return call(aiNk and aiNk.objectClusterizer, "getBlocker", path) or path.blocker
end

local function guardingCreaturePosition(aiNk, coord)
	return call(aiNk and aiNk.cc, "getGuardingCreaturePosition", coord)
end

local function copyPath(path)
	local result = {}
	for key, value in pairs(path or {}) do
		if key ~= "nodes" then
			result[key] = value
		end
	end
	result.nodes = {}
	return result
end

local function copyNode(node)
	local result = {}
	for key, value in pairs(node or {}) do
		result[key] = value
	end
	return result
end

local function clonePathToBlocker(aiNk, path, blockerPos)
	local clonedPath = copyPath(path)
	local originalNodes = path.nodes or {}

	for index = #originalNodes, 1, -1 do
		local node = copyNode(originalNodes[index])
		table.insert(clonedPath.nodes, 1, node)
		if tileEquals(node.coord, blockerPos) or tileEquals(guardingCreaturePosition(aiNk, node.coord), blockerPos) then
			break
		end
	end

	local indexOffset = #originalNodes - #clonedPath.nodes
	for _, node in ipairs(clonedPath.nodes) do
		if node.parentIndex ~= nil then
			node.parentIndex = node.parentIndex - indexOffset
		end
	end

	if #clonedPath.nodes == 0 then
		error("ClusterBehavior::decomposeCluster clonedPath has no nodes", 2)
	end

	if #clonedPath.nodes < 2 and clonedPath.nodes[1].targetHero ~= clonedPath.targetHero then
		clonedPath.targetHero = clonedPath.nodes[1].targetHero
	end

	clonedPath.tile = clonedPath.nodes[1].coord
	clonedPath.targetTile = clonedPath.nodes[1].coord

	return clonedPath
end

function ClusterBehavior:init()
	self.goalType = AbstractGoal.EGoals.CLUSTER_BEHAVIOR
end

function ClusterBehavior:toString()
	return "Unlock Clusters"
end

function ClusterBehavior:equalsTyped(_other)
	return true
end

function ClusterBehavior:decomposeCluster(aiNk, cluster)
	local center = clusterCenter(aiNk, cluster)
	local paths = pathInfo(aiNk, center)
	local blockerPos = visitablePos(cluster.blocker)
	local blockerPaths = {}
	local filteredPaths = {}
	local goals = {}

	for _, path in ipairs(paths) do
		if sameObject(blockerForPath(aiNk, path), cluster.blocker) then
			table.insert(filteredPaths, path)
			table.insert(blockerPaths, clonePathToBlocker(aiNk, path, blockerPos))
		end
	end

	local unlockTasks = CaptureObjectsBehavior.getVisitGoals(blockerPaths, aiNk, cluster.blocker)

	for index, path in ipairs(filteredPaths) do
		if index > #unlockTasks then
			error("ClusterBehavior::decomposeCluster unlockTasks size mismatch with paths size", 2)
		end

		if not unlockTasks[index]:invalid() then
			local composition = Composition.new()
			composition:addNext(UnlockCluster.new(cluster, path))
			composition:addNext(unlockTasks[index])
			table.insert(goals, composition)
		end
	end

	return goals
end

function ClusterBehavior:decompose(aiNk)
	local tasks = {}

	for _, cluster in ipairs(lockedClusters(aiNk)) do
		for _, task in ipairs(self:decomposeCluster(aiNk, cluster)) do
			table.insert(tasks, task)
		end
	end

	return tasks
end

return ClusterBehavior
