local AbstractGoal = require("Goals.AbstractGoal")
local CGoal = require("Goals.CGoal")
local TaskPlan = require("Engine.TaskPlan")

local Leaf = CGoal.derive("Leaf", AbstractGoal.EGoals.BUY_ARMY, { elementar = true })

function Leaf:init(name, priority, objectID, hero)
	self.name = name
	self.priority = priority
	self.objid = objectID or -1
	self.hero = hero
end

function Leaf:equalsTyped(other)
	return self.name == other.name
end

function Leaf:toString()
	return self.name
end

local hero = { id = 77 }
local plan = TaskPlan.new()
plan:mergeAndFilter(Leaf.new("low", 1, 10))
plan:mergeAndFilter(Leaf.new("ignored", 0, 11))
plan:mergeAndFilter(Leaf.new("high", 2, 10))
plan:mergeAndFilter(Leaf.new("hero-low", 1, 12, hero))
plan:mergeAndFilter(Leaf.new("hero-high", 3, 13, hero))

local tasks = plan:getTasks()
assert(#tasks == 2)
assert(tasks[1].name == "high")
assert(tasks[2].name == "hero-high")

assert(TaskPlan.chooseTaskFailureAction(true, true, true) == TaskPlan.TaskFailureAction.REPLAN)
assert(TaskPlan.chooseTaskFailureAction(false, true, true) == TaskPlan.TaskFailureAction.TRY_NEXT_TASK)
assert(TaskPlan.chooseTaskFailureAction(false, false, true) == TaskPlan.TaskFailureAction.REPLAN)
assert(TaskPlan.chooseTaskFailureAction(false, false, false) == TaskPlan.TaskFailureAction.STOP_TURN)

local best = TaskPlan.choseBestTask({
	Leaf.new("a", 1),
	Leaf.new("b", 5),
	Leaf.new("c", 2)
})
assert(best.name == "b")

local filtered = TaskPlan.buildPlanAndFilter({
	Leaf.new("object-a", 1, 30),
	Leaf.new("object-b", 5, 30),
	Leaf.new("object-c", 4, 31)
}, 1, {
	evaluate = function(task)
		return task.priority
	end
})
assert(#filtered == 2)
assert(filtered[1].name == "object-b")
assert(filtered[2].name == "object-c")
