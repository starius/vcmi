-- Mirrors AI/Nullkiller2/Engine/PriorityEvaluator.{h,cpp}: priority tiers and evaluator entry point.

local PriorityEvaluator = {}

PriorityEvaluator.PriorityTier = {
	BUILDINGS = 0,
	INSTAKILL = 1,
	INSTADEFEND = 2,
	KILL = 3,
	ESCAPE = 4,
	EXPLORE_AND_GATHER = 5,
	DEFEND = 6,
	MAX_PRIORITY_TIER = 6
}

function PriorityEvaluator.evaluate(task, priorityTier)
	if task and task.priority ~= nil and task.priority > 0 then
		return task.priority
	end

	return 0
end

return PriorityEvaluator
