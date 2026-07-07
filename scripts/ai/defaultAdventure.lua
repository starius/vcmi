local Script = {}

function Script.planDay(input)
    local memory = input.memory or {}
    memory.version = memory.version or 1

    return {
        status = "fallback",
        memory = memory,
        actions = {},
        intent = "Default script delegates to Nullkiller until scripted strategy is enabled."
    }
end

return Script
