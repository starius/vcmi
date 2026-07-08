local Script = {}

local function array(value)
    if type(value) == "table" then
        return value
    end
    return {}
end

local function copy(action)
    local result = {}
    for key, value in pairs(action or {}) do
        result[key] = value
    end
    return result
end

local function lower(value)
    return string.lower(tostring(value or ""))
end

local function initMemory(input)
    local memory = input.memory or {}
    if memory.version ~= 1 or memory.profile ~= "explorer" then
        memory = { version = 1, profile = "explorer", visitedTargets = {}, blockedTargets = {} }
    end
    memory.calls = (memory.calls or 0) + 1
    memory.lastDay = input.state and input.state.day or memory.lastDay
    memory.visitedTargets = memory.visitedTargets or {}
    memory.blockedTargets = memory.blockedTargets or {}
    return memory
end

local function failed(progress)
    for _ in pairs(array(progress and progress.failed)) do
        return true
    end
    return false
end

local function markProgress(memory, progress)
    for _, item in ipairs(array(progress and progress.executed)) do
        if item.object_id then
            memory.visitedTargets[tostring(item.object_id)] = true
        end
    end
    for _, item in ipairs(array(progress and progress.failed)) do
        if item.object_id then
            local key = tostring(item.object_id)
            memory.blockedTargets[key] = (memory.blockedTargets[key] or 0) + 1
        end
    end
end

local function scoreTarget(target, memory)
    local object = target.object or {}
    local path = target.path or {}
    local words = lower((object.name or "") .. " " .. (object.type or "") .. " " .. (object.hoverText or ""))
    local objectId = tostring(object.id or "")
    local score = 350 - (tonumber(path.cost or 0) or 0) * 100

    if memory.visitedTargets[objectId] then
        score = score - 500
    end
    if memory.blockedTargets[objectId] then
        score = score - memory.blockedTargets[objectId] * 500
    end
    if path.pathAction == "battle" or path.pathAction == "teleport_battle" then
        score = score - 500
    end
    if words:find("obelisk", 1, true) or words:find("subterranean", 1, true) or words:find("monolith", 1, true) then
        score = score + 500
    end
    if words:find("treasure", 1, true) or words:find("chest", 1, true) or words:find("artifact", 1, true) then
        score = score + 250
    end
    if words:find("resource", 1, true) or words:find("gold", 1, true) then
        score = score + 180
    end
    if path.isTeleportAction then
        score = score + 120
    end
    return score
end

local function bestTarget(input, memory)
    local best, bestScore
    for _, target in ipairs(array(input.actionSpace and input.actionSpace.reachableObjects)) do
        if target.planAction then
            local score = scoreTarget(target, memory)
            if not bestScore or score > bestScore then
                best, bestScore = target, score
            end
        end
    end
    return best
end

local function bestMove(input)
    local best, bestScore
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.movementOptions)) do
        if option.planAction then
            local path = option.path or {}
            local destination = path.destination or {}
            local score = (tonumber(destination.x or 0) or 0) + (tonumber(destination.y or 0) or 0)
            score = score - (tonumber(path.cost or 0) or 0) * 20
            if path.pathAction ~= "normal" then
                score = score - 250
            end
            if not bestScore or score > bestScore then
                best, bestScore = option, score
            end
        end
    end
    return best
end

local function bestBuild(input)
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.buildOptions)) do
        local name = lower(option.building)
        if option.planAction and (name:find("tavern", 1, true) or name:find("mage guild", 1, true)) then
            return option
        end
    end
end

function Script.planDay(input)
    local memory = initMemory(input)
    markProgress(memory, input.progress)
    if failed(input.progress) then
        memory.lastFailure = "replanning after failed action"
    end

    local actions = {}
    local intents = {}
    local build = bestBuild(input)
    local target = bestTarget(input, memory)
    local move = bestMove(input)

    if build then
        actions[#actions + 1] = copy(build.planAction)
        intents[#intents + 1] = "utility build " .. tostring(build.building or build.building_id)
    end
    if target then
        actions[#actions + 1] = copy(target.planAction)
        intents[#intents + 1] = "explore target " .. tostring((target.object or {}).name or (target.object or {}).id)
    elseif move then
        actions[#actions + 1] = copy(move.planAction)
        intents[#intents + 1] = "scout movement"
    end

    if #actions == 0 then
        return { status = "end_turn", memory = memory, actions = { { type = "end_turn" } }, intent = "explorer idle" }
    end
    memory.lastIntent = table.concat(intents, "; ")
    return { status = "need_replan", memory = memory, actions = actions, intent = memory.lastIntent, confidence = 0.5 }
end

return Script
