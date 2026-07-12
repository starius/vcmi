-- Mirrors AI/Nullkiller2/Engine/Nullkiller.cpp: TaskPlan and related task helpers.

local AbstractGoal = require("Goals.AbstractGoal")
local Invalid = require("Goals.Invalid")
local PriorityEvaluator = require("Engine.PriorityEvaluator")

local TaskPlan = {}
TaskPlan.__index = TaskPlan

TaskPlan.TaskFailureAction = {
	TRY_NEXT_TASK = "TRY_NEXT_TASK",
	REPLAN = "REPLAN",
	STOP_TURN = "STOP_TURN"
}

local function containsGoal(goals, goal)
	for _, existing in ipairs(goals) do
		if AbstractGoal.subgoalEquals(existing, goal) then
			return true
		end
	end
	return false
end

local function removeDuplicateTasks(tasks)
	local result = {}
	for _, task in ipairs(tasks) do
		local duplicate = false
		for _, existing in ipairs(result) do
			if existing:equals(task) then
				duplicate = true
				break
			end
		end
		if not duplicate then
			table.insert(result, task)
		end
	end
	return result
end

function TaskPlan.TaskPlanItem(task)
	return {
		affectedObjects = task:asTask():getAffectedObjects(),
		task = task
	}
end

function TaskPlan.new()
	return setmetatable({
		tasks = {}
	}, TaskPlan)
end

function TaskPlan:getTasks()
	local result = {}

	for _, item in ipairs(self.tasks) do
		table.insert(result, AbstractGoal.taskptr(item.task))
	end

	return removeDuplicateTasks(result)
end

function TaskPlan:mergeAndFilter(task)
	local blockers = {}

	if task:asTask().priority <= 0 then
		return
	end

	for _, item in ipairs(self.tasks) do
		for _, objid in ipairs(item.affectedObjects) do
			local sameTask = AbstractGoal.subgoalEquals(task, item.task)
			local objectBlocked = task:asTask():isObjectAffected(objid)
			local hero = task:asTask():getHero()
			local heroBlocked = hero ~= nil and hero == item.task:asTask():getHero()

			if sameTask or objectBlocked or heroBlocked then
				if item.task:asTask().priority >= task:asTask().priority then
					return
				end

				table.insert(blockers, item.task)
				break
			end
		end
	end

	local filtered = {}
	for _, item in ipairs(self.tasks) do
		if not containsGoal(blockers, item.task) then
			table.insert(filtered, item)
		end
	end
	self.tasks = filtered

	table.insert(self.tasks, TaskPlan.TaskPlanItem(task))
end

function TaskPlan.chooseTaskFailureAction(hasAnySuccess, hasRemainingTasks, canReplan)
	if hasAnySuccess then
		return TaskPlan.TaskFailureAction.REPLAN
	end

	if hasRemainingTasks then
		return TaskPlan.TaskFailureAction.TRY_NEXT_TASK
	end

	if canReplan then
		return TaskPlan.TaskFailureAction.REPLAN
	end

	return TaskPlan.TaskFailureAction.STOP_TURN
end

function TaskPlan.choseBestTask(tasks, evaluator)
	if #tasks == 0 then
		return AbstractGoal.taskptr(Invalid.new())
	end

	evaluator = evaluator or PriorityEvaluator
	for _, task in ipairs(tasks) do
		if task:asTask().priority <= 0 then
			task:asTask().priority = evaluator.evaluate(task)
		end
	end

	local bestTask = tasks[1]
	for _, task in ipairs(tasks) do
		if task:asTask().priority > bestTask:asTask().priority then
			bestTask = task
		end
	end

	return AbstractGoal.taskptr(bestTask)
end

function TaskPlan.buildPlanAndFilter(tasks, priorityTier, evaluator)
	local taskPlan = TaskPlan.new()
	evaluator = evaluator or PriorityEvaluator

	for _, task in ipairs(tasks) do
		if task:asTask().priority <= 0 or priorityTier ~= PriorityEvaluator.PriorityTier.BUILDINGS then
			task:asTask().priority = evaluator.evaluate(task, priorityTier)
		end
	end

	table.sort(tasks, function(lhs, rhs)
		return lhs:asTask().priority > rhs:asTask().priority
	end)

	for _, task in ipairs(tasks) do
		taskPlan:mergeAndFilter(task)
	end

	return taskPlan:getTasks()
end

return TaskPlan
