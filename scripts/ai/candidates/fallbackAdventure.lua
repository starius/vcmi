local Script = {}

function Script.planDay(input)
    return {
        status = "fallback",
        memory = input.memory or { version = 1 },
        actions = {},
        intent = "control: delegate full turn to Nullkiller",
        confidence = 0.5
    }
end

return Script
