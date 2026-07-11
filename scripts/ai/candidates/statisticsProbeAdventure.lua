local Script = {}

--[[
Statistics probe adventure policy
---------------------------------

This candidate exists to exercise one Lua API capability, not to compete as a
strategy profile. It requests the normal player statistics dataset, refreshes
visible input so the response can arrive through the update journal, records a
small amount of script-local memory, runs one bounded native turn slice, and
then ends the turn itself.

Useful properties:

* The statistics request uses the same checked callback/server path as a normal
  player action.
* The response is expected in `updates` as `statistics_response`; no hidden
  state is read directly by Lua.
* The bounded Nullkiller slice exercises native parity without giving away the
  rest of the day through explicit full-day fallback.
]]

local MemoryVersion = 1
local MaxPassesPerSlice = 4
local MaxCandidatesPerSlice = 16
local MaxAttemptsPerSlice = 4

local function normalizedMemory(input)
    local memory = (input and input.memory) or {}
    if memory.version ~= MemoryVersion then
        memory = { version = MemoryVersion }
    end

    memory.statisticsRequests = memory.statisticsRequests or 0
    memory.statisticsResponses = memory.statisticsResponses or 0
    return memory
end

local function countStatisticResponses(input)
    local count = 0
    local updates = (input and input.updates) or {}

    for _, update in ipairs(updates) do
        if update.type == "statistics_response" then
            count = count + 1
        end
    end

    return count
end

local function fallbackOutput(input)
    return {
        status = "fallback",
        memory = normalizedMemory(input),
        actions = {},
        intent = "statistics probe requires imperative runDay",
        confidence = 0.5
    }
end

local function sliceDidWork(result)
    return result.didWork == true
        or (tonumber(result.priorityTasksExecuted or 0) or 0) > 0
        or (tonumber(result.adventureStepsExecuted or 0) or 0) > 0
        or (tonumber(result.adventureReplanSteps or 0) or 0) > 0
        or (tonumber(result.tradePasses or 0) or 0) > 0
        or result.paused == true
end

local function sliceShouldStopTurn(result)
    return result.shouldStopTurn == true
        or (tonumber(result.adventureStopTurnSteps or 0) or 0) > 0
end

function Script.runDay(ai, input)
    local memory = normalizedMemory(input)
    memory.statisticsRequests = memory.statisticsRequests + 1
    ai:setMemory(memory)

    local request = ai:tryCall(function()
        return ai:requestStatistic()
    end)

    if not request.ok then
        memory.lastStatisticError = request.error
        ai:setMemory(memory)
        error("statistics probe failed: " .. tostring(request.error), 0)
    end

    local refreshed = ai:refresh()
    local responses = countStatisticResponses(refreshed)
    memory.statisticsResponses = memory.statisticsResponses + responses
    memory.lastStatisticResponseSeen = responses > 0
    ai:setMemory(memory)

    local slice = ai:nullkillerTurnSlice({
        max_passes = MaxPassesPerSlice,
        max_candidates = MaxCandidatesPerSlice,
        max_attempts = MaxAttemptsPerSlice
    })

    memory.lastSliceDidWork = sliceDidWork(slice)
    memory.lastSliceRequestedStop = sliceShouldStopTurn(slice)
    ai:setMemory(memory)

    if memory.lastSliceDidWork and not memory.lastSliceRequestedStop then
        ai:refresh()
    end

    ai:endTurn()
    return ai:output("end_turn", "statistics probe completed through a bounded native slice", 0.5)
end

function Script.planDay(input)
    return fallbackOutput(input)
end

return Script
