local Script = {}

--[[
Explorer adventure policy
-------------------------

This profile is built to reveal map information and exercise movement-heavy
plans. It favors exploration objects, teleports, and simple scout movement. It
still obeys the functional script contract: the host provides legal candidates,
the script ranks them, and the host executes or rejects the returned plan.
]]

local MemoryVersion = 1
local Profile = "explorer"

local Score = {
    impossible = -1000000000,
    target = {
        base = 350,
        pathCost = 100,
        visitedPenalty = 500,
        blockedPenalty = 500,
        battlePenalty = 500,
        explorationObjectBonus = 500,
        treasureOrArtifactBonus = 250,
        resourceOrGoldBonus = 180,
        teleportBonus = 120
    },
    move = {
        pathCost = 20,
        nonNormalPathPenalty = 250
    }
}

-- Accept missing optional arrays as empty arrays.
local function array(value)
    if type(value) == "table" then
        return value
    end
    return {}
end

-- Copy action fields before returning a plan. The script should not return
-- candidate metadata that the host did not ask to execute.
local function copy(action)
    local result = {}
    for key, value in pairs(action or {}) do
        result[key] = value
    end
    return result
end

-- Normalize object/building names for simple profile-specific substring checks.
local function lower(value)
    return string.lower(tostring(value or ""))
end

-- Reset memory when a different profile or version produced the stored data.
local function initMemory(input)
    local memory = input.memory or {}
    if memory.version ~= MemoryVersion or memory.profile ~= Profile then
        memory = { version = MemoryVersion, profile = Profile, visitedTargets = {}, blockedTargets = {} }
    end
    memory.calls = (memory.calls or 0) + 1
    memory.lastDay = input.state and input.state.day or memory.lastDay
    memory.visitedTargets = memory.visitedTargets or {}
    memory.blockedTargets = memory.blockedTargets or {}
    return memory
end

-- A failed action is a normal replan signal from the host.
local function failed(progress)
    for _ in pairs(array(progress and progress.failed)) do
        return true
    end
    return false
end

-- Track visited and blocked targets. Explorer mode penalizes repeats but does
-- not absolutely forbid them because a portal or exploration target may still be
-- worth retrying after a transient issue.
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
    local score = Score.target.base - (tonumber(path.cost or 0) or 0) * Score.target.pathCost

    if memory.visitedTargets[objectId] then
        score = score - Score.target.visitedPenalty
    end
    if memory.blockedTargets[objectId] then
        score = score - memory.blockedTargets[objectId] * Score.target.blockedPenalty
    end
    if path.pathAction == "battle" or path.pathAction == "teleport_battle" then
        score = score - Score.target.battlePenalty
    end
    -- Exploration markers and map-transition words are the main purpose of this
    -- profile, so they outrank ordinary resources.
    if words:find("obelisk", 1, true) or words:find("subterranean", 1, true) or words:find("monolith", 1, true) then
        score = score + Score.target.explorationObjectBonus
    end
    if words:find("treasure", 1, true) or words:find("chest", 1, true) or words:find("artifact", 1, true) then
        score = score + Score.target.treasureOrArtifactBonus
    end
    if words:find("resource", 1, true) or words:find("gold", 1, true) then
        score = score + Score.target.resourceOrGoldBonus
    end
    if path.isTeleportAction then
        score = score + Score.target.teleportBonus
    end
    return score
end

-- Pick the best reachable object. If no object exists, the policy falls back to
-- open movement below.
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

-- Movement fallback tries to push scouts outward using x+y as a cheap frontier
-- proxy. This is intentionally simple; trace review should replace it with
-- better host-provided exploration scores later.
local function bestMove(input)
    local best, bestScore
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.movementOptions)) do
        if option.planAction then
            local path = option.path or {}
            local destination = path.destination or {}
            local score = (tonumber(destination.x or 0) or 0) + (tonumber(destination.y or 0) or 0)
            score = score - (tonumber(path.cost or 0) or 0) * Score.move.pathCost
            if path.pathAction ~= "normal" then
                score = score - Score.move.nonNormalPathPenalty
            end
            if not bestScore or score > bestScore then
                best, bestScore = option, score
            end
        end
    end
    return best
end

-- Explorer utility builds are intentionally narrow: Tavern can add scouts and
-- Mage Guild can unlock map/utility spells. Other town development is left to
-- the default/economy policies.
local function bestBuild(input)
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.buildOptions)) do
        local name = lower(option.building)
        if option.planAction and (name:find("tavern", 1, true) or name:find("mage guild", 1, true)) then
            return option
        end
    end
end

function Script.planDay(input)
    -- Host entry point. Explorer mode may build utility first, then tries an
    -- exploration target, then falls back to movement if there is no target.
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

    -- A utility build can be paired with movement/targeting because it does not
    -- consume hero movement.
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
        -- Explicitly end the turn once there is no target, move, or utility
        -- build. This keeps host/script replanning finite.
        return { status = "end_turn", memory = memory, actions = { { type = "end_turn" } }, intent = "explorer idle" }
    end
    memory.lastIntent = table.concat(intents, "; ")
    return { status = "need_replan", memory = memory, actions = actions, intent = memory.lastIntent, confidence = 0.5 }
end

return Script
