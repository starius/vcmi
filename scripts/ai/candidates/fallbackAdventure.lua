local Script = {}

--[[
Fallback adventure policy
-------------------------

This is the current measured control/champion policy for full-game evaluation.
It deliberately does not choose build, recruit, movement, dialog, or transfer
actions in Lua. Instead it asks the C++ host to delegate the whole turn to the
native Nullkiller adventure AI.

Why keep this as a Lua script instead of just using Nullkiller2 directly?

* It exercises the ScriptedAdventureAI wrapper, imperative Lua API,
  configuration, memory handling, and promotion tooling without letting a weak
  Lua policy oversteer the game.
* It is the baseline every candidate policy must beat before promotion.
* It gives us a safe rollback target while richer declarative actions and typed
  dialog/preparation policies are developed.

The script still preserves memory shape so save/load and script-version checks
continue to follow the same contract as real policies.
]]

local function fallbackOutput(input)
    return {
        status = "fallback",
        memory = input.memory or { version = 1 },
        actions = {},
        intent = "control: delegate full turn to Nullkiller",
        confidence = 0.5
    }
end

function Script.runDay(ai, input)
    local output = fallbackOutput(input)
    ai:setMemory(output.memory)
    ai:nullkiller(output.intent)
end

function Script.planDay(input)
    return fallbackOutput(input)
end

return Script
