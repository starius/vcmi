local Script = {}

--[[
Bounded Nullkiller control
--------------------------

This script is an API parity probe, not an attempt to outsmart Nullkiller.
It drives Nullkiller's normal day phases through bounded host calls and keeps
control in Lua after every native pass:

1. Answer any pending server dialog through the bounded Nullkiller query helper.
2. Run a capped native day helper that composes one-pass native turn slices:
   priority tasks, adventure task, trade, and artifact cleanup.
3. Refresh visible state after side effects, then decide whether to ask for
   another native pass.
4. End the turn when the bounded native helper reports no more work or an
   explicit stop-turn condition.

The important contract is what this script does not do in normal play: it does
not call ai:nullkiller(), which would delegate the whole remaining day to C++.
If a bounded helper itself fails, the script raises an error and lets the host's
existing safety fallback take over. That keeps full fallback reserved for real
script/host failures, not for ordinary strategy.
]]

local MemoryVersion = 1
local DefaultMaxCommandsPerDay = 256
local SafetyMaxSlicesPerDay = 256
local DefaultMaxPassesPerSlice = 16
local SafetyMaxPassesPerSlice = 64
-- A zero limit asks the host to let the bounded Nullkiller helper inspect and
-- execute every candidate generated for the current native pass. This keeps
-- Lua in control between passes without accidentally narrowing Nullkiller's
-- own task search to the first N serialized candidates.
local DefaultMaxCandidates = 0
local DefaultMaxAttempts = 0
local MaxQueriesPerSlice = 16

local function normalizeMemory(input)
    local memory = (input and input.memory) or {}
    if memory.version ~= MemoryVersion then
        memory = { version = MemoryVersion }
    end

    memory.days = memory.days or 0
    memory.totalSlices = memory.totalSlices or 0
    memory.totalQueriesAnswered = memory.totalQueriesAnswered or 0
    return memory
end

local function turnIsActive(input)
    local turn = input and input.state and input.state.turn
    if not turn then
        return true
    end
    return turn.active ~= false
end

local function commandBudget(input)
    local limits = (input and input.limits) or {}
    local actions = tonumber(limits.maxActions)
    local scriptCalls = tonumber(limits.maxScriptCallsPerTurn)

    -- Imperative scripts run once per day and may yield/resume many checked
    -- host commands. The old maxScriptCallsPerTurn limit describes legacy
    -- multi-call planning; use it only when the host does not expose the real
    -- imperative command budget.
    local budget = actions or scriptCalls or DefaultMaxCommandsPerDay

    return math.max(1, math.min(SafetyMaxSlicesPerDay, budget))
end

local function nullkillerSettings(input)
    local analysis = (input and input.analysis) or {}
    local nullkiller = analysis.nullkiller or {}
    return nullkiller.settings or {}
end

local function pendingQueries(input)
    local turn = input and input.state and input.state.turn
    return (turn and turn.queries) or {}
end

local function hasPendingQueries(input)
    return #pendingQueries(input) > 0
end

local function answeredQueryIntent(count)
    if count == 1 then
        return "answered one pending query before bounded native planning"
    end
    return "answered " .. tostring(count) .. " pending queries before bounded native planning"
end

local function answerPendingQueries(ai, current, memory)
    local answered = 0

    while answered < MaxQueriesPerSlice and hasPendingQueries(current) do
        local query = pendingQueries(current)[1]
        ai:nullkillerAnswerQuery(query, 0)
        answered = answered + 1
        memory.totalQueriesAnswered = memory.totalQueriesAnswered + 1
        current = ai:refresh()
    end

    if hasPendingQueries(current) then
        error("bounded Nullkiller control could not clear pending queries within one slice")
    end

    if answered > 0 then
        memory.lastIntent = answeredQueryIntent(answered)
    end

    return current, answered
end

local function sliceDidAdventureWork(result)
    return (tonumber(result.adventureStepsExecuted or 0) or 0) > 0
        or (tonumber(result.adventureReplanSteps or 0) or 0) > 0
end

local function sliceDidPriorityWork(result)
    return (tonumber(result.priorityTasksExecuted or 0) or 0) > 0
end

local function sliceDidTrade(result)
    return (tonumber(result.tradePasses or 0) or 0) > 0
end

local function sliceDidWork(result)
    return result.didWork == true
        or sliceDidPriorityWork(result)
        or sliceDidAdventureWork(result)
        or sliceDidTrade(result)
        or result.paused == true
end

local function sliceShouldEndTurn(result)
    -- A bounded pass can exhaust one generated candidate set after doing useful
    -- priority/adventure/trade work. Native Nullkiller keeps planning in that
    -- case; only a no-work exhaustion means the day is actually idle.
    return result.shouldEndTurn == true
        or result.shouldStopTurn == true
        or (tonumber(result.adventureStopTurnSteps or 0) or 0) > 0
        or (result.exhaustedCandidates == true and not sliceDidWork(result))
end

local function runNativeSlice(ai, current)
    local settings = nullkillerSettings(current)
    local maxPasses = tonumber(settings.maxPass) or DefaultMaxPassesPerSlice

    return ai:nullkillerBoundedDay({
        max_passes = math.max(1, math.min(SafetyMaxPassesPerSlice, maxPasses)),
        max_candidates = DefaultMaxCandidates,
        max_attempts = DefaultMaxAttempts,
        max_queries_per_pass = MaxQueriesPerSlice,
        default_answer = 0
    })
end

function Script.runDay(ai, input)
    local memory = normalizeMemory(input)
    memory.days = memory.days + 1
    memory.slicesToday = 0
    ai:setMemory(memory)

    local current = input
    local maxSlices = commandBudget(input)

    while turnIsActive(current) and memory.slicesToday < maxSlices do
        current = answerPendingQueries(ai, current, memory)

        if not turnIsActive(current) then
            return ai:output("end_turn", "turn ended while answering bounded-control queries", 0.5)
        end

        local result = runNativeSlice(ai, current)
        local passCount = tonumber(result.passCount or 1) or 1
        memory.slicesToday = memory.slicesToday + passCount
        memory.totalSlices = memory.totalSlices + passCount
        ai:setMemory(memory)

        if sliceShouldEndTurn(result) then
            ai:endTurn()
            local intent = "bounded Nullkiller control accepted native stop-turn signal"
            if result.status == "idle" then
                intent = "bounded Nullkiller control found no remaining native work"
            end
            return ai:output("end_turn", intent, 0.5)
        end

        if sliceDidWork(result) then
            current = ai:refresh()
        else
            ai:endTurn()
            return ai:output("end_turn", "bounded Nullkiller control found no remaining native work", 0.5)
        end
    end

    if not turnIsActive(current) then
        return ai:output("end_turn", "turn ended during bounded Nullkiller control", 0.5)
    end

    error("bounded Nullkiller control exhausted its command budget before the day was idle")
end

function Script.planDay(input)
    return {
        status = "fallback",
        memory = normalizeMemory(input),
        actions = {},
        intent = "bounded Nullkiller control requires imperative runDay",
        confidence = 0.5
    }
end

function Script.decideBattleRetreat(input)
    -- Battle retreat/surrender is a synchronous battle callback, not part of
    -- the daily imperative action loop. This parity script intentionally keeps
    -- Nullkiller's existing policy as the decision owner until a candidate is
    -- explicitly experimenting with battle-preservation strategy.
    return {
        decision_id = 0,
        intent = "bounded Nullkiller control delegates battle retreat policy"
    }
end

return Script
