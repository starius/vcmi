-- Mirrors AI/Nullkiller2/Engine/DeepDecomposer.{h,cpp}: recursive goal decomposition.

local AbstractGoal = require("Goals.AbstractGoal")
local Composition = require("Goals.Composition")

local DeepDecomposer = {}
DeepDecomposer.__index = DeepDecomposer

local function containsGoal(goals, goal)
	for _, existing in ipairs(goals) do
		if AbstractGoal.subgoalEquals(existing, goal) then
			return true
		end
	end
	return false
end

function DeepDecomposer.new(aiNk)
	return setmetatable({
		aiNk = aiNk,
		goals = {},
		decompositionCache = {},
		depth = 1
	}, DeepDecomposer)
end

function DeepDecomposer:reset()
	self.decompositionCache = {}
	self.goals = {}
end

function DeepDecomposer:decompose(results, behavior, depthLimit)
	self.goals = {}
	self.decompositionCache = {}
	for index = 1, depthLimit do
		self.goals[index] = {}
		self.decompositionCache[index] = {}
	end
	self.depth = 1
	self.goals[1] = { behavior }

	while #self.goals[1] > 0 do
		local current = self.goals[self.depth][#self.goals[self.depth]]
		local subgoals, fromCache = self:decomposeCached(self:unwrapComposition(current))

		if self.depth < depthLimit then
			self.goals[self.depth + 1] = {}
		end

		for _, subgoal in ipairs(subgoals) do
			if not subgoal:invalid() then
				if subgoal:isElementar() then
					local task = self.depth >= 2 and self:aggregateGoals(1, subgoal) or subgoal

					if not self:isCompositionLoop(subgoal) then
						table.insert(results, task)

						if not fromCache then
							self:addToCache(subgoal)
						end
					end
				elseif self.depth < depthLimit then
					if not self:isCompositionLoop(subgoal) then
						local goalToAdd = self.depth >= 2 and self:unwrapComposition(subgoal) or subgoal

						if not containsGoal(self.goals[self.depth + 1], goalToAdd) then
							table.insert(self.goals[self.depth + 1], subgoal)
						end
					end
				end
			end
		end

		if self.depth < depthLimit and #self.goals[self.depth + 1] > 0 then
			self.depth = self.depth + 1
		else
			table.remove(self.goals[self.depth])

			while self.depth > 1 and #self.goals[self.depth] == 0 do
				self.depth = self.depth - 1
				table.remove(self.goals[self.depth])
			end
		end
	end
end

function DeepDecomposer:aggregateGoals(startDepth, last)
	local composition = Composition.new()

	for index = startDepth, self.depth do
		composition:addNext(self.goals[index][#self.goals[index]])
	end

	composition:addNext(last)
	return composition
end

function DeepDecomposer:unwrapComposition(goal)
	if goal.goalType == AbstractGoal.EGoals.COMPOSITION then
		local decomposed = goal:decompose(self.aiNk)
		return decomposed[#decomposed]
	end

	return goal
end

function DeepDecomposer:isEquivalentGoals(goal1, goal2)
	if AbstractGoal.subgoalEquals(goal1, goal2) then
		return true
	end

	if goal1.goalType == AbstractGoal.EGoals.CAPTURE_OBJECT and goal2.goalType == AbstractGoal.EGoals.CAPTURE_OBJECT then
		local cc = self.aiNk and self.aiNk.cc
		if cc and type(cc.getObj) == "function" then
			local obj1 = cc:getObj(goal1.objid)
			local obj2 = cc:getObj(goal2.objid)
			return obj1 and obj2 and obj1.ID == "SHIPYARD" and obj1.ID == obj2.ID
		end
	end

	return false
end

function DeepDecomposer:isCompositionLoop(goal)
	local goalsToTest = goal.goalType == AbstractGoal.EGoals.COMPOSITION and goal:decompose(self.aiNk) or { goal }

	for _, goalToTest in ipairs(goalsToTest) do
		for index = self.depth, 1, -1 do
			local parent = self:unwrapComposition(self.goals[index][#self.goals[index]])

			if self:isEquivalentGoals(parent, goalToTest) then
				return true
			end
		end
	end

	return false
end

function DeepDecomposer:decomposeCached(goal)
	if goal:hasHash() then
		local hash = goal:getHash()
		for index = 1, self.depth do
			local cached = self.decompositionCache[index][hash]
			if cached then
				return cached, true
			end
		end

		self.decompositionCache[self.depth][hash] = {}
	end

	return goal:decompose(self.aiNk), false
end

function DeepDecomposer:addToCache(goal)
	local trusted = true

	for parentDepth = 2, self.depth do
		local parent = self:unwrapComposition(self.goals[parentDepth][#self.goals[parentDepth]])

		if parent:hasHash() then
			local solution = parentDepth < self.depth and self:aggregateGoals(parentDepth + 1, goal) or goal
			local parentHash = parent:getHash()

			self.decompositionCache[parentDepth][parentHash] = self.decompositionCache[parentDepth][parentHash] or {}
			table.insert(self.decompositionCache[parentDepth][parentHash], solution)

			if trusted then
				self.decompositionCache[1][parentHash] = self.decompositionCache[1][parentHash] or {}
				table.insert(self.decompositionCache[1][parentHash], solution)
				trusted = false
			end
		end
	end
end

return DeepDecomposer
