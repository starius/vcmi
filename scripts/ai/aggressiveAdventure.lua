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

local function resource(resources, name)
    if type(resources) ~= "table" then
        return 0
    end
    return tonumber(resources[name] or 0) or 0
end

local function initMemory(input)
    local memory = input.memory or {}
    if memory.version ~= 1 or memory.profile ~= "aggressive" then
        memory = { version = 1, profile = "aggressive" }
    end
    memory.calls = (memory.calls or 0) + 1
    memory.lastDay = input.state and input.state.day or memory.lastDay
    return memory
end

local function failed(progress)
    for _ in pairs(array(progress and progress.failed)) do
        return true
    end
    return false
end

local function scoreRecruit(option)
    return (tonumber(option.level or 0) or 0) * 180 + (tonumber(option.amount or 0) or 0) * 12
end

local function bestRecruit(input)
    local best, bestScore
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.recruitOptions)) do
        if option.planAction and (tonumber(option.amount or 0) or 0) > 0 then
            local score = scoreRecruit(option)
            if not bestScore or score > bestScore then
                best, bestScore = option, score
            end
        end
    end
    return best
end

local function scoreBuild(option)
    local name = lower(option.building)
    local score = 40 - resource(option.cost, "gold") * 0.01
    if name:find("dwelling", 1, true) or name:find("portal", 1, true) then
        score = score + 550
    end
    if name:find("castle", 1, true) or name:find("citadel", 1, true) then
        score = score + 360
    end
    if name:find("blacksmith", 1, true) or name:find("mage guild", 1, true) then
        score = score + 120
    end
    if name:find("city hall", 1, true) or name:find("capitol", 1, true) then
        score = score + 180
    end
    return score
end

local function bestBuild(input)
    local best, bestScore
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.buildOptions)) do
        if option.planAction then
            local score = scoreBuild(option)
            if not bestScore or score > bestScore then
                best, bestScore = option, score
            end
        end
    end
    return best
end

local function scoreTarget(target)
    local object = target.object or {}
    local path = target.path or {}
    local words = lower((object.name or "") .. " " .. (object.type or "") .. " " .. (object.hoverText or ""))
    local score = 500 - (tonumber(path.cost or 0) or 0) * 120

    if path.pathAction == "battle" or path.pathAction == "teleport_battle" then
        score = score + 260
    end
    if words:find("town", 1, true) or words:find("mine", 1, true) then
        score = score + 420
    end
    if words:find("artifact", 1, true) or words:find("treasure", 1, true) then
        score = score + 260
    end
    if words:find("resource", 1, true) or words:find("gold", 1, true) then
        score = score + 180
    end
    if path.isTeleportAction then
        score = score - 80
    end
    return score
end

local function bestTarget(input)
    local best, bestScore
    for _, target in ipairs(array(input.actionSpace and input.actionSpace.reachableObjects)) do
        if target.planAction then
            local score = scoreTarget(target)
            if not bestScore or score > bestScore then
                best, bestScore = target, score
            end
        end
    end
    return best
end

function Script.planDay(input)
    local memory = initMemory(input)
    if failed(input.progress) then
        return { status = "fallback", memory = memory, actions = {}, intent = "aggressive fallback after failed action" }
    end

    local actions = {}
    local intents = {}
    local recruit = bestRecruit(input)
    local build = bestBuild(input)
    local target = bestTarget(input)

    if recruit then
        actions[#actions + 1] = copy(recruit.planAction)
        intents[#intents + 1] = "recruit"
    elseif build then
        actions[#actions + 1] = copy(build.planAction)
        intents[#intents + 1] = "build " .. tostring(build.building or build.building_id)
    end
    if target then
        actions[#actions + 1] = copy(target.planAction)
        intents[#intents + 1] = "attack/claim " .. tostring((target.object or {}).name or (target.object or {}).id)
    end

    if #actions == 0 then
        return { status = "end_turn", memory = memory, actions = { { type = "end_turn" } }, intent = "aggressive idle" }
    end
    memory.lastIntent = table.concat(intents, "; ")
    return { status = "need_replan", memory = memory, actions = actions, intent = memory.lastIntent, confidence = 0.52 }
end

return Script
