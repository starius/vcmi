local Script = {}

--[[
Explorer adventure policy
-------------------------

This profile is built to reveal map information and exercise movement-heavy
plans. It favors exploration objects, teleports, and simple scout movement. It
still obeys the script contract: the host provides legal candidates, the script
ranks them, and side effects go through checked host actions.
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

local BuildingKind = {
    unknown = 0,
    mageGuild = 1,
    tavern = 2
}

local ObjectKind = {
    unknown = 0,
    treasure = 1,
    resource = 2,
    artifact = 4,
    teleport = 10,
    visitBonus = 12,
    quest = 14
}

local PathAction = {
    unknown = 0,
    normal = 3,
    battle = 4,
    teleportBattle = 9
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

-- Normalize labels only as a compatibility fallback for old traces. Current
-- host input exposes integer kind ids for scoring.
local function lower(value)
    return string.lower(tostring(value or ""))
end

local function buildingKindId(option)
    return tonumber(option and (option.buildingKindId or option.building_kind_id) or 0) or 0
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
    local kind = objectKindId(object)
    local words = kind == ObjectKind.unknown and lower((object.name or "") .. " " .. (object.type or "") .. " " .. (object.hoverText or "")) or ""
    local objectId = tostring(object.id or "")
    local score = Score.target.base - (tonumber(path.cost or 0) or 0) * Score.target.pathCost

    if memory.visitedTargets[objectId] then
        score = score - Score.target.visitedPenalty
    end
    if memory.blockedTargets[objectId] then
        score = score - memory.blockedTargets[objectId] * Score.target.blockedPenalty
    end
    if isBattlePath(path) then
        score = score - Score.target.battlePenalty
    end
    -- Exploration markers and map-transition words are the main purpose of this
    -- profile, so they outrank ordinary resources.
    if kind == ObjectKind.teleport
        or kind == ObjectKind.quest
        or kind == ObjectKind.visitBonus
        or words:find("obelisk", 1, true)
        or words:find("subterranean", 1, true)
        or words:find("monolith", 1, true)
    then
        score = score + Score.target.explorationObjectBonus
    end
    if kind == ObjectKind.treasure
        or kind == ObjectKind.artifact
        or words:find("treasure", 1, true)
        or words:find("chest", 1, true)
        or words:find("artifact", 1, true)
    then
        score = score + Score.target.treasureOrArtifactBonus
    end
    if kind == ObjectKind.resource or words:find("resource", 1, true) or words:find("gold", 1, true) then
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
            if pathActionId(path) ~= PathAction.normal then
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
        local kind = buildingKindId(option)
        local name = kind == BuildingKind.unknown and lower(option.building) or ""
        if option.planAction
            and (kind == BuildingKind.tavern
                or kind == BuildingKind.mageGuild
                or name:find("tavern", 1, true)
                or name:find("mage guild", 1, true))
        then
            return option
        end
    end
end

local function chooseDayActions(input)
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

function Script.planDay(input)
    -- Legacy compatibility path for tests and older hosts. Active turns call
    -- runDay and never require the host to execute this returned batch.
    return chooseDayActions(input)
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

local function answerPendingQuery(ai, query)
    ai:answerQueryByPolicy(query, {
        use_plan_actions = true,
        default_answer = 0
    })
end

local NativeNullkiller = {
    maxTurnSlicePasses = 4,
    maxCandidates = 16,
    maxAttempts = 16
}

local function positive(value)
    return (tonumber(value or 0) or 0) > 0
end

local function runBoundedNullkillerTurnSlice(ai, current, commands, limit, intent, confidence)
    -- The explorer profile may exhaust its simple scout heuristic before the
    -- real turn is done. Use a bounded native slice for that gap, then return
    -- control to Lua instead of handing over the whole remaining day.
    local remainingCommands = math.max(1, limit - commands)
    local result = ai:nullkillerTurnSlice({
        max_passes = math.min(NativeNullkiller.maxTurnSlicePasses, remainingCommands),
        max_candidates = NativeNullkiller.maxCandidates,
        max_attempts = NativeNullkiller.maxAttempts
    }) or {}
    commands = commands + 1

    if result.shouldStopTurn == true or positive(result.adventureStopTurnSteps) then
        ai:endTurn()
        return current, commands, ai:output("end_turn", "bounded Nullkiller slice accepted native stop-turn signal", confidence)
    end

    if result.didWork == true
        or result.paused == true
        or result.stop == true
        or positive(result.priorityTasksExecuted)
        or positive(result.adventureStepsExecuted)
        or positive(result.adventureReplanSteps)
        or positive(result.tradePasses)
    then
        current = ai:refresh()
        current.memory = ai:memory()
        return current, commands
    end

    ai:endTurn()
    return current, commands, ai:output("end_turn", intent or "bounded Nullkiller slice found no work", confidence)
end

function Script.runDay(ai, input)
    -- Imperative compatibility path for this profile. The policy still uses a
    -- readable local scorer, but every chosen action is now executed as a
    -- checked host call with refresh/yield points between side effects.
    local current = input or {}
    local commands = 0
    local limit = commandLimit(current)

    while commands < limit do
        local query = firstPendingQuery(current)
        if query then
            answerPendingQuery(ai, query)
            commands = commands + 1
            current = ai:refresh()
            current.memory = ai:memory()
        else
            local output = chooseDayActions(current)
            ai:setMemory(output.memory or ai:memory())

            if output.status == "fallback" then
                local finalOutput
                current, commands, finalOutput = runBoundedNullkillerTurnSlice(ai, current, commands, limit, output.intent, output.confidence)
                if finalOutput then
                    return finalOutput
                end
            else
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
    end

    local finalOutput
    current, commands, finalOutput = runBoundedNullkillerTurnSlice(ai, current, commands, limit, "explorer profile reached its imperative command budget", 0.25)
    if finalOutput then
        return finalOutput
    end
    ai:endTurn()
    return ai:output("end_turn", "explorer profile reached its bounded native budget", 0.25)
end

return Script
