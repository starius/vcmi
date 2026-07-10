local Script = {}

--[[
Bounded Nullkiller adventure policy
-----------------------------------

This candidate is intentionally simple: it demonstrates the new imperative
Lua-to-Nullkiller bridge without trying to outscore Nullkiller yet.

The script asks C++ for one native Nullkiller task at a time, lets C++ execute
that one task through Nullkiller's normal machinery, refreshes visible state,
and then decides whether to ask for another bounded task. This is different
from ai:nullkiller(), which delegates the entire remaining day.

Why this script exists:

* It exercises task snapshots, task handles, one-task execution, refresh, and
  script memory with a real Lua policy.
* It is a readable starting point for future scripts that rank native
  Nullkiller candidates instead of rebuilding pathfinding and blocker logic in
  Lua.
* It keeps a strict step budget. If Lua cannot confidently continue, it
  delegates the rest of the day to native Nullkiller rather than idling.

Strategic policy should not use debug descriptions returned by C++. The useful
machine fields are numeric/stable identifiers such as goalTypeId, priority,
priorityTier, hero_id, town_id, object_id, tile, and affectedObjectIds.
]]

local MemoryVersion = 1
local MaxNullkillerStepsPerDay = 8
local MaxCandidatesPerStep = 16

local function normalizedMemory(input)
    local memory = (input and input.memory) or {}
    if memory.version ~= MemoryVersion then
        memory = { version = MemoryVersion }
    end

    memory.days = memory.days or 0
    memory.totalNullkillerSteps = memory.totalNullkillerSteps or 0
    return memory
end

local function turnIsActive(input)
    return input
        and input.state
        and input.state.turn
        and input.state.turn.active == true
end

local function configuredStepLimit(input)
    local actionLimit = input
        and input.limits
        and input.limits.maxActions

    if type(actionLimit) == "number" and actionLimit > 0 then
        return math.min(MaxNullkillerStepsPerDay, actionLimit)
    end

    return MaxNullkillerStepsPerDay
end

function Script.runDay(ai, input)
    local memory = normalizedMemory(input)
    memory.days = memory.days + 1
    ai:setMemory(memory)

    local current = input
    local stepLimit = configuredStepLimit(current)
    local stepsToday = 0

    while stepsToday < stepLimit and turnIsActive(current) do
        local ok, result = pcall(function()
            return ai:nullkillerStep("all", MaxCandidatesPerStep)
        end)
        stepsToday = stepsToday + 1
        memory.totalNullkillerSteps = memory.totalNullkillerSteps + 1

        if not ok then
            memory.lastNullkillerStepError = tostring(result)
            ai:nullkiller("bounded Nullkiller task failed; delegate remaining turn")
        end

        if not result.didExecute then
            ai:nullkiller("bounded Nullkiller policy found no executable native task")
        end

        current = ai:refresh()
    end

    if not turnIsActive(current) then
        return ai:output("end_turn", "bounded Nullkiller policy completed after host ended turn", 0.5)
    end

    ai:nullkiller("bounded Nullkiller policy reached its per-day step budget")
end

function Script.planDay(input)
    return {
        status = "fallback",
        memory = normalizedMemory(input),
        actions = {},
        intent = "bounded Nullkiller candidate requires imperative runDay",
        confidence = 0.5
    }
end

return Script
