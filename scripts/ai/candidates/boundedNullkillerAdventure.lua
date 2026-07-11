local Script = {}

--[[
Bounded Nullkiller adventure policy
-----------------------------------

This candidate is intentionally simple: it demonstrates the new imperative
Lua-to-Nullkiller bridge without trying to outscore Nullkiller yet.

The script asks C++ for one native Nullkiller task at a time, lets C++ execute
that one task through Nullkiller's normal machinery, refreshes visible state,
and then decides whether to ask for another bounded task. This keeps Lua in
control after each native step instead of delegating the entire remaining day.

Why this script exists:

* It exercises task snapshots, task handles, one-task execution, refresh, and
  script memory with a real Lua policy.
* It is a readable starting point for future scripts that rank native
  Nullkiller candidates instead of rebuilding pathfinding and blocker logic in
  Lua.
* It keeps a strict step budget. If Lua cannot confidently continue, it
  ends the turn on native stop/idle signals or raises an error on unexpected
  helper failure, leaving full fallback to the host safety path only.

Strategic policy should not use debug descriptions returned by C++. The useful
machine fields are numeric/stable identifiers such as goalTypeId, priority,
priorityTier, hero_id, town_id, object_id, tile, and affectedObjectIds.
]]

local MemoryVersion = 1
local MaxNullkillerStepsPerDay = 8
local MaxCandidatesPerStep = 16
local MaxTaskAttemptsPerStep = 4

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
        local attempt = ai:tryCall(function()
            return ai:nullkillerStep("all", MaxCandidatesPerStep, MaxTaskAttemptsPerStep)
        end)
        stepsToday = stepsToday + 1
        memory.totalNullkillerSteps = memory.totalNullkillerSteps + 1

        if not attempt.ok then
            memory.lastNullkillerStepError = attempt.error
            ai:setMemory(memory)
            error("bounded Nullkiller task failed: " .. tostring(attempt.error), 0)
        end

        local result = attempt.result or {}
        if not result.didExecute then
            -- Native Nullkiller can sometimes learn from a failed candidate by
            -- locking the involved hero or widening scan depth. In that case
            -- the script refreshes and asks for a new bounded step instead of
            -- surrendering the whole day.
            if result.outcomeId == ai.nullkillerStepOutcomes.replan then
                current = ai:refresh()
            elseif result.shouldStopTurn then
                ai:endTurn()
                return ai:output("end_turn", "bounded Nullkiller policy accepted native stop-turn signal", 0.5)
            else
                ai:endTurn()
                return ai:output("end_turn", "bounded Nullkiller policy found no executable native task", 0.5)
            end
        else
            current = ai:refresh()
        end
    end

    if not turnIsActive(current) then
        return ai:output("end_turn", "bounded Nullkiller policy completed after host ended turn", 0.5)
    end

    error("bounded Nullkiller policy reached its per-day step budget before the day was idle", 0)
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
