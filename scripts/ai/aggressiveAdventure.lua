local Script = {}

--[[
Aggressive adventure policy
---------------------------

This profile is intentionally simpler than defaultAdventure.lua. It demonstrates
an attack/tempo-biased personality for multi-AI comparisons: recruit first,
prefer creature production and defenses over economy, and accept battle targets
more readily than the default policy. The script still returns only declarative
host actions; it never mutates game state directly.
]]

local MemoryVersion = 1
local Profile = "aggressive"

local Score = {
    impossible = -1000000000,
    recruit = {
        level = 180,
        amount = 12
    },
    build = {
        base = 40,
        goldCost = 0.01,
        dwellingOrPortal = 550,
        castleOrCitadel = 360,
        blacksmithOrMageGuild = 120,
        cityHallOrCapitol = 180
    },
    target = {
        base = 500,
        pathCost = 120,
        battleBonus = 260,
        townOrMineBonus = 420,
        artifactOrTreasureBonus = 260,
        resourceOrGoldBonus = 180,
        teleportPenalty = 80
    }
}

-- Treat absent optional arrays as empty arrays. This keeps narrow tests and
-- partial host snapshots from crashing the policy.
local function array(value)
    if type(value) == "table" then
        return value
    end
    return {}
end

-- Return a shallow copy of the host-provided action. The plan should contain
-- action fields only, not references to script-local candidate objects.
local function copy(action)
    local result = {}
    for key, value in pairs(action or {}) do
        result[key] = value
    end
    return result
end

-- Lowercase helper for stable name matching. The host validates actions; these
-- string checks are only profile preferences.
local function lower(value)
    return string.lower(tostring(value or ""))
end

-- Missing resource fields are interpreted as zero so fixture inputs can stay
-- focused on the fields relevant to the behavior under test.
local function resource(resources, name)
    if type(resources) ~= "table" then
        return 0
    end
    return tonumber(resources[name] or 0) or 0
end

-- Profile-specific memory is reset if loaded memory belongs to another policy.
-- That prevents an economy/explorer memory layout from influencing this script.
local function initMemory(input)
    local memory = input.memory or {}
    if memory.version ~= MemoryVersion or memory.profile ~= Profile then
        memory = { version = MemoryVersion, profile = Profile }
    end
    memory.calls = (memory.calls or 0) + 1
    memory.lastDay = input.state and input.state.day or memory.lastDay
    memory.visitedTargets = memory.visitedTargets or {}
    memory.blockedTargets = memory.blockedTargets or {}
    return memory
end

-- Any failed host action should be visible in memory so a trace can explain why
-- the script replanned.
local function failed(progress)
    for _ in pairs(array(progress and progress.failed)) do
        return true
    end
    return false
end

-- Remember completed and failed object visits. Aggressive mode fully rejects
-- both categories for target selection because its goal is forward tempo.
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

local function scoreRecruit(option)
    return (tonumber(option.level or 0) or 0) * Score.recruit.level + (tonumber(option.amount or 0) or 0) * Score.recruit.amount
end

-- Recruitment is this profile's first priority. Higher-level stacks are valued
-- heavily, with available amount as a secondary tie-breaker.
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
    local score = Score.build.base - resource(option.cost, "gold") * Score.build.goldCost
    if name:find("dwelling", 1, true) or name:find("portal", 1, true) then
        score = score + Score.build.dwellingOrPortal
    end
    if name:find("castle", 1, true) or name:find("citadel", 1, true) then
        score = score + Score.build.castleOrCitadel
    end
    if name:find("blacksmith", 1, true) or name:find("mage guild", 1, true) then
        score = score + Score.build.blacksmithOrMageGuild
    end
    if name:find("city hall", 1, true) or name:find("capitol", 1, true) then
        score = score + Score.build.cityHallOrCapitol
    end
    return score
end

-- If no recruitment is available, build the option that best supports fighting:
-- dwellings and defenses first, then utility/economy.
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

local function scoreTarget(target, memory)
    local object = target.object or {}
    local path = target.path or {}
    local words = lower((object.name or "") .. " " .. (object.type or "") .. " " .. (object.hoverText or ""))
    local objectKey = tostring(object.id or "")
    local score = Score.target.base - (tonumber(path.cost or 0) or 0) * Score.target.pathCost

    if objectKey ~= "" and (memory.visitedTargets[objectKey] or memory.blockedTargets[objectKey]) then
        return Score.impossible
    end

    -- Unlike the default policy, aggressive mode rewards battle paths. This is
    -- useful for A/B comparisons against economy and explorer profiles.
    if path.pathAction == "battle" or path.pathAction == "teleport_battle" then
        score = score + Score.target.battleBonus
    end
    if words:find("town", 1, true) or words:find("mine", 1, true) then
        score = score + Score.target.townOrMineBonus
    end
    if words:find("artifact", 1, true) or words:find("treasure", 1, true) then
        score = score + Score.target.artifactOrTreasureBonus
    end
    if words:find("resource", 1, true) or words:find("gold", 1, true) then
        score = score + Score.target.resourceOrGoldBonus
    end
    if path.isTeleportAction then
        score = score - Score.target.teleportPenalty
    end
    return score
end

-- Pick one reachable object target after filtering out completed/blocked ids.
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

function Script.planDay(input)
    -- Host entry point. Plan ordering is intentional: prepare army first, then
    -- spend movement on the best attack/claim opportunity.
    local memory = initMemory(input)
    markProgress(memory, input.progress)
    if failed(input.progress) then
        memory.lastFailure = "replanning after failed action"
    end

    local actions = {}
    local intents = {}
    local recruit = bestRecruit(input)
    local build = bestBuild(input)
    local target = bestTarget(input, memory)

    -- Recruitment and building are mutually exclusive in this profile's first
    -- slot. When both exist, troops now beat infrastructure later.
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
        -- Explicit end_turn prevents the host from repeatedly asking for an
        -- empty plan after all useful candidates are exhausted.
        return { status = "end_turn", memory = memory, actions = { { type = "end_turn" } }, intent = "aggressive idle" }
    end
    memory.lastIntent = table.concat(intents, "; ")
    return { status = "need_replan", memory = memory, actions = actions, intent = memory.lastIntent, confidence = 0.52 }
end

return Script
