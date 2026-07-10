local Script = {}

--[[
Economy adventure policy
------------------------

This profile is a readable baseline for "grow the kingdom first" experiments.
It favors income buildings, mines, and loose resources while avoiding battles
unless the host has no better economic target. It is deliberately not a full AI:
the host supplies legal build/recruit/target candidates and this script ranks
them without mutating game state.
]]

local MemoryVersion = 1
local Profile = "economy"

local Threshold = {
    surplusGoldForRecruitment = 5000,
    goldForDwellingAfterEconomy = 3500
}

local Score = {
    impossible = -1000000000,
    build = {
        base = 100,
        incomeGold = 5,
        goldCost = 0.012,
        townHall = 800,
        cityHall = 1100,
        capitol = 1300,
        marketOrResource = 260,
        dwellingWithSurplus = 130,
        castleOrCitadel = 80
    },
    recruit = {
        belowSurplus = -1000,
        level = 60,
        amount = 5
    },
    target = {
        base = 300,
        pathCost = 180,
        mineBonus = 700,
        resourceBonus = 360,
        battlePenalty = 450,
        teleportPenalty = 150
    }
}

local BuildingKind = {
    unknown = 0,
    mageGuild = 1,
    tavern = 2,
    shipyard = 3,
    fortification = 4,
    hall = 5,
    market = 6,
    resourceSilo = 7,
    blacksmith = 8,
    special = 9,
    horde = 10,
    dwelling = 11,
    grail = 12,
    ship = 13
}

local ObjectKind = {
    unknown = 0,
    treasure = 1,
    resource = 2,
    mine = 3
}

local PathAction = {
    unknown = 0,
    normal = 3,
    battle = 4,
    teleportBattle = 9
}

-- Optional arrays are common in reduced fixtures; treat nil as empty.
local function array(value)
    if type(value) == "table" then
        return value
    end
    return {}
end

-- Return an independent plan action table. Candidate tables may contain host
-- analysis fields that should not be returned as executable action fields.
local function copy(action)
    local result = {}
    for key, value in pairs(action or {}) do
        result[key] = value
    end
    return result
end

-- Case-insensitive labels are used only as a compatibility fallback. Current
-- host input provides integer ids for strategic decisions.
local function lower(value)
    return string.lower(tostring(value or ""))
end

-- Resource helpers default to zero so missing narrow-test fields do not change
-- control flow through Lua errors.
local function resource(resources, name)
    if type(resources) ~= "table" then
        return 0
    end
    return tonumber(resources[name] or 0) or 0
end

local function buildingKindId(option)
    return tonumber(option and (option.buildingKindId or option.building_kind_id) or 0) or 0
end

local function buildingLevel(option)
    return tonumber(option and (option.buildingLevel or option.building_level) or 0) or 0
end

local function objectKindId(object)
    return tonumber(object and (object.kindId or object.objectKindId or object.kind_id) or 0) or 0
end

local function pathActionId(path)
    local action = tonumber(path and (path.pathActionId or path.path_action_id) or nil)
    if action then
        return action
    end

    local label = lower(path and path.pathAction)
    if label == "normal" then
        return PathAction.normal
    end
    if label == "battle" then
        return PathAction.battle
    end
    if label == "teleport_battle" then
        return PathAction.teleportBattle
    end
    return PathAction.unknown
end

local function isBattlePath(path)
    local action = pathActionId(path)
    return action == PathAction.battle or action == PathAction.teleportBattle
end

-- Economy memory is profile-tagged. Reusing memory from another policy could
-- carry different assumptions about blocked/visited targets, so reset it.
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

-- Failed actions are not exceptional; they tell the next script call to avoid
-- overcommitting to the same target.
local function failed(progress)
    for _ in pairs(array(progress and progress.failed)) do
        return true
    end
    return false
end

-- Track object progress across replans and days. Economy mode rejects completed
-- and blocked targets in scoring so movement is spent on fresh value.
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

local function scoreBuild(option, input)
    local kind = buildingKindId(option)
    local level = buildingLevel(option)
    local name = kind == BuildingKind.unknown and lower(option.building) or ""
    local gold = resource(input.state and input.state.resources, "gold")
    local score = Score.build.base + resource(option.income, "gold") * Score.build.incomeGold - resource(option.cost, "gold") * Score.build.goldCost

    -- Income chain milestones dominate this profile because their benefit
    -- compounds over the rest of the map.
    if (kind == BuildingKind.hall and level == 2) or name:find("town hall", 1, true) then
        score = score + Score.build.townHall
    end
    if (kind == BuildingKind.hall and level == 3) or name:find("city hall", 1, true) then
        score = score + Score.build.cityHall
    end
    if (kind == BuildingKind.hall and level >= 4) or name:find("capitol", 1, true) then
        score = score + Score.build.capitol
    end
    if kind == BuildingKind.market
        or kind == BuildingKind.resourceSilo
        or name:find("market", 1, true)
        or name:find("resource", 1, true)
    then
        score = score + Score.build.marketOrResource
    end
    if (kind == BuildingKind.dwelling or name:find("dwelling", 1, true)) and gold >= Threshold.goldForDwellingAfterEconomy then
        score = score + Score.build.dwellingWithSurplus
    end
    if kind == BuildingKind.fortification or name:find("castle", 1, true) or name:find("citadel", 1, true) then
        score = score + Score.build.castleOrCitadel
    end
    return score
end

-- Choose one economic build if the host offers any legal construction.
local function bestBuild(input)
    local best, bestScore
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.buildOptions)) do
        if option.planAction then
            local score = scoreBuild(option, input)
            if not bestScore or score > bestScore then
                best, bestScore = option, score
            end
        end
    end
    return best
end

local function scoreRecruit(option, input)
    local gold = resource(input.state and input.state.resources, "gold")
    -- Recruitment is intentionally suppressed until there is a gold surplus.
    -- This keeps economy runs from spending early income on troops by default.
    if gold < Threshold.surplusGoldForRecruitment then
        return Score.recruit.belowSurplus
    end
    return (tonumber(option.level or 0) or 0) * Score.recruit.level + (tonumber(option.amount or 0) or 0) * Score.recruit.amount
end

-- Recruitment is a fallback after builds and only when scoreRecruit says there
-- is enough surplus to justify it.
local function bestRecruit(input)
    local best, bestScore
    for _, option in ipairs(array(input.actionSpace and input.actionSpace.recruitOptions)) do
        if option.planAction and (tonumber(option.amount or 0) or 0) > 0 then
            local score = scoreRecruit(option, input)
            if score > 0 and (not bestScore or score > bestScore) then
                best, bestScore = option, score
            end
        end
    end
    return best
end

local function scoreTarget(target, memory)
    local object = target.object or {}
    local path = target.path or {}
    local kind = objectKindId(object)
    local words = kind == ObjectKind.unknown and lower((object.name or "") .. " " .. (object.type or "") .. " " .. (object.hoverText or "")) or ""
    local objectKey = tostring(object.id or "")
    local score = Score.target.base - (tonumber(path.cost or 0) or 0) * Score.target.pathCost

    if objectKey ~= "" and (memory.visitedTargets[objectKey] or memory.blockedTargets[objectKey]) then
        return Score.impossible
    end

    -- Mines and resource pickups are the core movement targets for this policy.
    if kind == ObjectKind.mine
        or words:find("mine", 1, true)
        or words:find("sawmill", 1, true)
        or words:find("ore pit", 1, true)
    then
        score = score + Score.target.mineBonus
    end
    if kind == ObjectKind.resource
        or kind == ObjectKind.treasure
        or words:find("gold", 1, true)
        or words:find("resource", 1, true)
        or words:find("campfire", 1, true)
    then
        score = score + Score.target.resourceBonus
    end
    if isBattlePath(path) then
        score = score - Score.target.battlePenalty
    end
    if path.isTeleportAction then
        score = score - Score.target.teleportPenalty
    end
    return score
end

-- Pick one reachable economic target.
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
    -- Host entry point. Economy mode opens with infrastructure, then movement,
    -- then surplus recruitment only if no build was selected.
    local memory = initMemory(input)
    markProgress(memory, input.progress)
    if failed(input.progress) then
        memory.lastFailure = "replanning after failed action"
    end

    local actions = {}
    local intents = {}
    local build = bestBuild(input)
    local target = bestTarget(input, memory)
    local recruit = bestRecruit(input)

    -- Build and recruit share the first action slot. Builds win because this
    -- profile is meant to test long-term economy bias.
    if build then
        actions[#actions + 1] = copy(build.planAction)
        intents[#intents + 1] = "economic build " .. tostring(build.building or build.building_id)
    elseif recruit then
        actions[#actions + 1] = copy(recruit.planAction)
        intents[#intents + 1] = "surplus recruit"
    end
    if target then
        actions[#actions + 1] = copy(target.planAction)
        intents[#intents + 1] = "claim economy target " .. tostring((target.object or {}).name or (target.object or {}).id)
    end

    if #actions == 0 then
        -- Return explicit end_turn once no economic plan remains.
        return { status = "end_turn", memory = memory, actions = { { type = "end_turn" } }, intent = "economy idle" }
    end
    memory.lastIntent = table.concat(intents, "; ")
    return { status = "need_replan", memory = memory, actions = actions, intent = memory.lastIntent, confidence = 0.58 }
end

local function firstPendingQuery(input)
    local queries = input
        and input.state
        and input.state.turn
        and input.state.turn.queries

    if type(queries) == "table" then
        return queries[1]
    end
end

local function commandLimit(input)
    local limits = (input and input.limits) or {}
    return math.max(1, tonumber(limits.maxScriptCallsPerTurn or limits.maxActions or 8) or 8)
end

function Script.runDay(ai, input)
    -- Imperative compatibility path for this profile. The policy still uses
    -- the readable planDay scorer, but every chosen action is now executed as a
    -- checked host call with refresh/yield points between side effects.
    local current = input or {}
    local commands = 0
    local limit = commandLimit(current)

    while commands < limit do
        local query = firstPendingQuery(current)
        if query then
            ai:nullkillerAnswerQuery(query, 0)
            commands = commands + 1
            current = ai:refresh()
            current.memory = ai:memory()
        else
            local output = Script.planDay(current)
            ai:setMemory(output.memory or ai:memory())

            if output.status == "fallback" then
                return ai:nullkiller(output.intent)
            end

            local actions = output.actions or {}
            if #actions == 0 then
                ai:endTurn()
                return ai:output("end_turn", output.intent, output.confidence)
            end

            for _, action in ipairs(actions) do
                if action.type == "end_turn" then
                    ai:endTurn()
                    return ai:output("end_turn", output.intent, output.confidence)
                end

                ai:execute(action)
                commands = commands + 1
                current = ai:refresh()
                current.memory = ai:memory()

                if commands >= limit or firstPendingQuery(current) then
                    break
                end
            end
        end
    end

    return ai:nullkiller("economy profile reached its imperative command budget")
end

return Script
