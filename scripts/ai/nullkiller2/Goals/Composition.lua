-- Mirrors AI/Nullkiller2/Goals/Composition.{h,cpp}: Composition.

local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")

local Composition = CGoal.derive("Composition", AbstractGoal.EGoals.COMPOSITION, { elementar = true })

local function concatenate(dst, src)
	for _, value in ipairs(src or {}) do
		table.insert(dst, value)
	end
end

local function removeDuplicateIDs(values)
	local result = {}
	local seen = {}
	for _, value in ipairs(values) do
		if not seen[value] then
			seen[value] = true
			table.insert(result, value)
		end
	end
	return result
end

function Composition:init()
	self.subtasks = {}
end

function Composition:equalsTyped(_other)
	return false
end

function Composition:toString()
	local result = "Composition"

	for _, step in ipairs(self.subtasks) do
		result = result .. "["
		for _, goal in ipairs(step) do
			if goal:isElementar() then
				result = result .. goal:toString() .. " => "
			else
				result = result .. goal:toString() .. ", "
			end
		end
		result = result .. "] "
	end

	return result
end

function Composition:accept(aiGw)
	local last = self.subtasks[#self.subtasks]
	if not last then
		return nil
	end

	for _, task in ipairs(last) do
		if task:isElementar() then
			task:asTask():accept(aiGw)
		else
			break
		end
	end

	return nil
end

function Composition:addNext(goal)
	if goal.goalType == AbstractGoal.EGoals.COMPOSITION then
		concatenate(self.subtasks, goal.subtasks)
	else
		table.insert(self.subtasks, { goal })
	end

	return self
end

function Composition:addNextSequence(taskSequence)
	table.insert(self.subtasks, taskSequence)
	return self
end

function Composition:decompose(_aiNk)
	local result = {}
	for _, step in ipairs(self.subtasks) do
		concatenate(result, step)
	end
	return result
end

function Composition:isElementar()
	local last = self.subtasks[#self.subtasks]
	return last and last[1] and last[1]:isElementar() or false
end

function Composition:getHeroExchangeCount()
	local result = 0
	local last = self.subtasks[#self.subtasks]
	if not last then
		return result
	end

	for _, task in ipairs(last) do
		if task:isElementar() then
			result = result + task:asTask():getHeroExchangeCount()
		end
	end

	return result
end

function Composition:getAffectedObjects()
	local affectedObjects = {}

	for _, sequence in ipairs(self.subtasks) do
		for _, task in ipairs(sequence) do
			if task:isElementar() then
				concatenate(affectedObjects, task:asTask():getAffectedObjects())
			end
		end
	end

	return removeDuplicateIDs(affectedObjects)
end

function Composition:isObjectAffected(id)
	for _, sequence in ipairs(self.subtasks) do
		for _, task in ipairs(sequence) do
			if task:isElementar() and task:asTask():isObjectAffected(id) then
				return true
			end
		end
	end

	return false
end

return Composition
