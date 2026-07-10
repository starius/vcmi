local Script = {}

--[[
Statistics probe adventure policy
---------------------------------

This candidate exists to exercise one Lua API capability, not to compete as a
strategy profile. It requests the normal player statistics dataset, refreshes
visible input so the response can arrive through the update journal, records a
small amount of script-local memory, and then delegates the rest of the day to
native Nullkiller.

Useful properties:

* The statistics request uses the same checked callback/server path as a normal
  player action.
* The response is expected in `updates` as `statistics_response`; no hidden
  state is read directly by Lua.
* Delegation after the probe prevents this script from weakening game play when
  used as a smoke target.
]]

local MemoryVersion = 1

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

function Script.runDay(ai, input)
    local memory = normalizedMemory(input)
    memory.statisticsRequests = memory.statisticsRequests + 1
    ai:setMemory(memory)

    local ok, result = pcall(function()
        return ai:requestStatistic()
    end)

    if not ok then
        memory.lastStatisticError = tostring(result)
        ai:setMemory(memory)
        ai:nullkiller("statistics probe failed; delegate remaining turn")
    end

    local refreshed = ai:refresh()
    local responses = countStatisticResponses(refreshed)
    memory.statisticsResponses = memory.statisticsResponses + responses
    memory.lastStatisticResponseSeen = responses > 0
    ai:setMemory(memory)

    ai:nullkiller("statistics probe completed; delegate remaining turn")
end

function Script.planDay(input)
    return fallbackOutput(input)
end

return Script
