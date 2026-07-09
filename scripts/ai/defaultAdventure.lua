local Script = {}

--[[
Default adventure policy
------------------------

The C++ adventure AI host gives this script a read-only snapshot of the current
turn plus an already-validated action space. The script does not call game
actions directly. It only chooses a small ordered plan made from actions the host
has already declared legal enough to try. If a chosen action becomes invalid or
unfinished while the host executes it, the host calls this script again with
progress information and the memory returned here.

The policy is intentionally heuristic and explicit. We prefer named weights over
opaque arithmetic so later trace-based tuning can change priorities without
turning the script into unreadable score soup.
]]

local MemoryVersion = 1

local Pressure = {
    none = 0,
    low = 1,
    high = 2,
    critical = 3
}

local Search = {
    -- Enemy heroes this close to a target make it less attractive, especially
    -- for scouts. This is a squared distance so the script avoids square roots.
    opponentNearTargetDistanceSquared = 36
}

local Score = {
    impossible = -1000000000,

    hero = {
        -- Hero level is a rough proxy for army leadership and spell access when
        -- selecting the main hero. Stack counts are added separately below.
        levelWeight = 25
    },

    build = {
        base = 100,
        incomeGold = 3,
        cityHallOrCapitol = 900,
        townHall = 450,
        castleOrCitadel = 250,
        dwellingOrPortal = 180,
        pressuredCastleOrCitadel = 450,
        pressuredDwelling = 120,
        pressuredIncomePenalty = 150,
        lowGoldNonIncomePenalty = 250,
        goldCost = 0.015
    },

    recruit = {
        level = 100,
        amount = 8,
        defensePressure = 180,
        heroThreatPressure = 90,
        lowGoldPenalty = 300
    },

    object = {
        base = 400,
        visitedPenalty = 600,
        blockedPenalty = 250,
        pathCost = 160,
        targetValue = 0.35,
        unsafeMainBasePenalty = 450,
        unsafeMainDangerPenalty = 350,
        unsafeScoutBasePenalty = 1200,
        unsafeScoutDangerPenalty = 500,
        riskyMainDangerPenalty = 150,
        riskyScoutDangerPenalty = 350,
        treasureBonus = 350,
        mineBonus = 450,
        resourceBonus = 220,
        repeatedMineWordBonus = 180,
        battleMainPenalty = 120,
        battleScoutPenalty = 700,
        teleportPenalty = 120,
        opponentNearMainPenalty = 150,
        opponentNearScoutPenalty = 550,
        criticalDefenseMainPenalty = 350,
        scoutNonTeleportBonus = 80
    },

    escape = {
        distanceSquared = 0.5,
        criticalThreatBonus = 300,
        teleportPenalty = 250
    }
}

local Threshold = {
    buildLowGold = 2500,
    recruitLowGold = 1500
}

local Confidence = {
    scriptedPlan = 0.55,
    idle = 0.5
}

-- Lua receives JSON arrays as tables, but missing fields arrive as nil. This
-- helper lets every loop treat absent optional arrays as empty arrays.
local function asArray(value)
    if type(value) == "table" then
        return value
    end
    return {}
end

-- The host owns validation and execution. Copying keeps script-side temporary
-- fields or shared table references from leaking into the returned plan.
local function copyAction(action)
    local result = {}
    for key, value in pairs(action or {}) do
        result[key] = value
    end
    return result
end

-- Normalize labels before substring checks. The host sends object/building names
-- mostly for policy readability, so string matching is deliberately forgiving.
local function text(value)
    return string.lower(tostring(value or ""))
end

-- Resources can be missing in narrow tests or future reduced snapshots. Missing
-- values should mean "unknown/zero" rather than crashing the script.
local function resourceValue(resources, name)
    if type(resources) ~= "table" then
        return 0
    end
    return tonumber(resources[name] or 0) or 0
end

-- Replanning after a failed action is normal. The host may fail an action after
-- teleport, dialog, path obstruction, or stale state; the script records that
-- signal in memory and chooses again.
local function hasFailures(progress)
    for _ in pairs(asArray(progress and progress.failed)) do
        return true
    end
    return false
end

local function initializeMemory(input)
    local memory = input.memory or {}
    -- Versioning lets future script revisions discard incompatible memory
    -- instead of trying to interpret old fields incorrectly.
    if memory.version ~= MemoryVersion then
        memory = { version = MemoryVersion }
    end
    -- Calls increments on every invocation, not every day. Multiple invocations
    -- in one day are expected when an action fails or asks the host to replan.
    memory.calls = (memory.calls or 0) + 1
    memory.lastDay = input.state and input.state.day or memory.lastDay
    memory.roles = memory.roles or {}
    memory.visitedTargets = memory.visitedTargets or {}
    memory.blockedTargets = memory.blockedTargets or {}
    return memory
end

local function heroId(hero)
    return tostring(hero and hero.id or "")
end

-- Main-hero selection uses only stable, cheap fields from the visible state.
-- This is not a battle simulator; it just identifies which hero should be more
-- willing to take valuable guarded targets.
local function armyScore(hero)
    local score = (tonumber(hero and hero.level or 0) or 0) * Score.hero.levelWeight
    for _, stack in ipairs(asArray(hero and hero.army)) do
        score = score + (tonumber(stack.count or 0) or 0)
    end
    return score
end

local function heroExists(input, id)
    for _, hero in ipairs(asArray(input.state and input.state.heroes)) do
        if tostring(hero.id or "") == tostring(id or "") then
            return true
        end
    end
    return false
end

local function distanceSquared(left, right)
    if type(left) ~= "table" or type(right) ~= "table" then
        return nil
    end
    if left.z ~= nil and right.z ~= nil and tonumber(left.z) ~= tonumber(right.z) then
        return nil
    end
    local dx = (tonumber(left.x or 0) or 0) - (tonumber(right.x or 0) or 0)
    local dy = (tonumber(left.y or 0) or 0) - (tonumber(right.y or 0) or 0)
    return dx * dx + dy * dy
end

-- Defense pressure is the strongest town-defense alert visible this call. It is
-- intentionally compressed to a small ordinal so build/recruit/object scoring
-- can make coarse strategic shifts without copying host-side threat analysis.
local function defensePressure(input)
    local pressure = Pressure.none
    for _, alert in ipairs(asArray(input.analysis and input.analysis.defenseAlerts)) do
        local level = text(alert.level)
        if level == "critical" then
            pressure = math.max(pressure, Pressure.critical)
        elseif level == "high" then
            pressure = math.max(pressure, Pressure.high)
        else
            pressure = math.max(pressure, Pressure.low)
        end
    end
    return pressure
end

-- Hero threat pressure is separate from town pressure. It increases recruitment
-- priority even when no town is under immediate threat.
local function heroThreatPressure(input)
    local pressure = Pressure.none
    for _, alert in ipairs(asArray(input.analysis and input.analysis.heroThreatAlerts)) do
        local level = text(alert.level)
        if level == "critical" then
            pressure = math.max(pressure, Pressure.critical)
        elseif level == "high" then
            pressure = math.max(pressure, Pressure.high)
        else
            pressure = math.max(pressure, Pressure.low)
        end
    end
    return pressure
end

-- Escape decisions are per hero, so index the host threat alerts by hero id.
local function heroThreatsByHero(input)
    local result = {}
    for _, alert in ipairs(asArray(input.analysis and input.analysis.heroThreatAlerts)) do
        local id = tostring(alert.hero_id or "")
        if id ~= "" then
            result[id] = result[id] or {}
            result[id][#result[id] + 1] = alert
        end
    end
    return result
end

local function opponentNearTarget(input, target)
    local object = target.object or {}
    local position = object.position or (target.path and target.path.destination)
    if type(position) ~= "table" then
        return false
    end

    for _, enemyHero in ipairs(asArray(input.analysis and input.analysis.visibleEnemyHeroes)) do
        local distance = distanceSquared(position, enemyHero.position)
        if distance and distance <= Search.opponentNearTargetDistanceSquared then
            return true
        end
    end
    return false
end

-- Opponent updates are summarized in memory instead of storing raw event lists.
-- This keeps script memory bounded while still allowing future policy rules to
-- react to "the opponent moved/built/fought since I last planned".
local function rememberOpponentUpdates(input, memory)
    local previousRevision = tonumber(memory.lastOpponentRevision or 0) or 0
    local latestRevision = tonumber(input.opponentUpdates and input.opponentUpdates.latestRevision or previousRevision) or previousRevision
    local counts = {}
    local any = false

    for _, event in ipairs(asArray(input.opponentUpdates and input.opponentUpdates.events)) do
        local revision = tonumber(event.revision or 0) or 0
        if revision > previousRevision then
            local eventType = tostring(event.type or "unknown")
            counts[eventType] = (counts[eventType] or 0) + 1
            any = true
        end
    end

    if latestRevision > previousRevision then
        memory.lastOpponentRevision = latestRevision
    end
    if any then
        memory.recentOpponentEvents = counts
    end
end

-- Assign one persistent main hero. If that hero disappears, reassign from the
-- current visible heroes. Scouts are every other owned hero by default.
local function assignRoles(input, memory)
    if memory.roles.mainHero and heroExists(input, memory.roles.mainHero) then
        return
    end

    local bestHero
    local bestScore = -1
    for _, hero in ipairs(asArray(input.state and input.state.heroes)) do
        local score = armyScore(hero)
        if score > bestScore then
            bestHero = hero
            bestScore = score
        end
    end

    if bestHero then
        memory.roles.mainHero = bestHero.id
    end
end

local function roleForHero(memory, id)
    if tostring(memory.roles and memory.roles.mainHero or "") == tostring(id or "") then
        return "main"
    end
    return "scout"
end

-- Progress is the bridge between host execution and script memory. Successful
-- visits become low-priority repeat targets; failed visits get a growing penalty
-- so the script can try alternatives on the next call.
local function markProgress(memory, progress)
    for _, item in ipairs(asArray(progress and progress.executed)) do
        if item.object_id then
            memory.visitedTargets[tostring(item.object_id)] = true
        end
    end
    for _, item in ipairs(asArray(progress and progress.failed)) do
        if item.object_id then
            memory.blockedTargets[tostring(item.object_id)] = (memory.blockedTargets[tostring(item.object_id)] or 0) + 1
        end
    end
end

local function scoreBuild(option, input)
    local score = Score.build.base
    local name = text(option.building)
    local gold = resourceValue(input.state and input.state.resources, "gold")
    local costGold = resourceValue(option.cost, "gold")
    local incomeGold = resourceValue(option.income, "gold")
    local pressure = defensePressure(input)

    -- Income buildings are valuable in normal development because they compound
    -- over future days. City Hall and Capitol get extra priority because their
    -- names are stable and they are the main economic milestones.
    score = score + incomeGold * Score.build.incomeGold
    if name:find("city hall", 1, true) or name:find("capitol", 1, true) then
        score = score + Score.build.cityHallOrCapitol
    end
    if name:find("town hall", 1, true) then
        score = score + Score.build.townHall
    end
    if name:find("castle", 1, true) or name:find("citadel", 1, true) then
        score = score + Score.build.castleOrCitadel
    end
    if name:find("dwelling", 1, true) or name:find("portal", 1, true) then
        score = score + Score.build.dwellingOrPortal
    end
    -- Under pressure, defensive buildings and creature production beat pure
    -- income because surviving the next enemy move matters more than payback.
    if pressure >= Pressure.high then
        if name:find("castle", 1, true) or name:find("citadel", 1, true) then
            score = score + Score.build.pressuredCastleOrCitadel
        elseif name:find("dwelling", 1, true) then
            score = score + Score.build.pressuredDwelling
        elseif incomeGold > 0 then
            score = score - Score.build.pressuredIncomePenalty
        end
    end
    if gold < Threshold.buildLowGold and incomeGold <= 0 then
        score = score - Score.build.lowGoldNonIncomePenalty
    end
    score = score - costGold * Score.build.goldCost
    return score
end

-- Pick a single town build. The host has already filtered unavailable builds,
-- so this function only ranks currently offered options.
local function chooseBuild(input)
    local best
    local bestScore = Score.impossible
    for _, option in ipairs(asArray(input.actionSpace and input.actionSpace.buildOptions)) do
        local score = scoreBuild(option, input)
        if option.planAction and score > bestScore then
            best = option
            bestScore = score
        end
    end
    return best, bestScore
end

local function scoreRecruit(option, input)
    local gold = resourceValue(input.state and input.state.resources, "gold")
    local score = (tonumber(option.level or 0) or 0) * Score.recruit.level
    score = score + (tonumber(option.amount or 0) or 0) * Score.recruit.amount
    score = score + defensePressure(input) * Score.recruit.defensePressure
    score = score + heroThreatPressure(input) * Score.recruit.heroThreatPressure
    if gold < Threshold.recruitLowGold then
        score = score - Score.recruit.lowGoldPenalty
    end
    return score
end

-- Recruit only when the host reports positive available creature count. The
-- script does not try to compute dwelling growth or town slots itself.
local function chooseRecruit(input)
    local best
    local bestScore = Score.impossible
    for _, option in ipairs(asArray(input.actionSpace and input.actionSpace.recruitOptions)) do
        if (tonumber(option.amount or 0) or 0) > 0 and option.planAction then
            local score = scoreRecruit(option, input)
            if score > bestScore then
                best = option
                bestScore = score
            end
        end
    end
    return best, bestScore
end

local function scoreObject(target, memory, input)
    local object = target.object or {}
    local path = target.path or {}
    local name = text((object.name or "") .. " " .. (object.type or "") .. " " .. (object.hoverText or ""))
    local score = Score.object.base
    local role = roleForHero(memory, target.hero_id)
    local objectKey = tostring(object.id or "")

    -- Repeating an already completed or recently failed target usually wastes
    -- movement. Failed targets are not forbidden because one retry can still be
    -- correct after a transient dialog/path issue; they just become expensive.
    if memory.visitedTargets[objectKey] then
        score = score - Score.object.visitedPenalty
    end
    if memory.blockedTargets[objectKey] then
        score = score - memory.blockedTargets[objectKey] * Score.object.blockedPenalty
    end

    -- Host-provided value is combined with path cost. The script trusts the host
    -- for danger/path calculations and only adds strategic preferences.
    score = score - (tonumber(path.cost or 0) or 0) * Score.object.pathCost
    score = score + (tonumber(target.value or 0) or 0) * Score.object.targetValue

    local dangerRatio = tonumber(target.dangerRatio or 0) or 0
    if target.safe == false then
        if role == "main" then
            score = score - Score.object.unsafeMainBasePenalty - dangerRatio * Score.object.unsafeMainDangerPenalty
        else
            score = score - Score.object.unsafeScoutBasePenalty - dangerRatio * Score.object.unsafeScoutDangerPenalty
        end
    elseif dangerRatio > 0 then
        score = score - dangerRatio * (role == "main" and Score.object.riskyMainDangerPenalty or Score.object.riskyScoutDangerPenalty)
    end

    -- Name-based bonuses are deliberately simple and visible. The C++ host still
    -- decides what is reachable and legal; these terms express strategic taste.
    if name:find("gold", 1, true) or name:find("treasure", 1, true) or name:find("chest", 1, true) then
        score = score + Score.object.treasureBonus
    end
    if name:find("mine", 1, true) or name:find("sawmill", 1, true) or name:find("ore pit", 1, true) then
        score = score + Score.object.mineBonus
    end
    if name:find("resource", 1, true) or name:find("wood", 1, true) or name:find("ore", 1, true) then
        score = score + Score.object.resourceBonus
    end
    if name:find("mine", 1, true) then
        score = score + Score.object.repeatedMineWordBonus
    end
    if path.pathAction == "battle" or path.pathAction == "teleport_battle" then
        if role == "main" then
            score = score - Score.object.battleMainPenalty
        else
            score = score - Score.object.battleScoutPenalty
        end
    end
    if path.isTeleportAction then
        score = score - Score.object.teleportPenalty
    end
    if opponentNearTarget(input, target) then
        if role == "main" then
            score = score - Score.object.opponentNearMainPenalty
        else
            score = score - Score.object.opponentNearScoutPenalty
        end
    end
    if defensePressure(input) >= Pressure.critical and role == "main" then
        score = score - Score.object.criticalDefenseMainPenalty
    end
    if role == "scout" and not path.isTeleportAction then
        score = score + Score.object.scoutNonTeleportBonus
    end
    return score
end

-- When a hero is threatened, choose a safe move that increases distance from
-- the enemy. This avoids sending a threatened hero to a normal object target
-- before escaping.
local function chooseEscapeMove(input)
    local threats = heroThreatsByHero(input)
    local best
    local bestScore = Score.impossible

    for _, option in ipairs(asArray(input.actionSpace and input.actionSpace.movementOptions)) do
        local heroKey = tostring(option.hero_id or "")
        local heroThreats = threats[heroKey]
        local action = option.planAction
        if heroThreats and action and action.type == "move_hero" and option.safe ~= false then
            local destination = option.path and option.path.destination
            local score = tonumber(option.value or 0) or 0
            local improvesDistance = false

            for _, threat in ipairs(heroThreats) do
                local currentDistance = tonumber(threat.distanceSquared or 0) or 0
                local nextDistance = distanceSquared(destination, threat.enemyPosition)
                if nextDistance then
                    score = score + nextDistance * Score.escape.distanceSquared
                    if nextDistance > currentDistance then
                        improvesDistance = true
                    end
                end
                if text(threat.level) == "critical" then
                    score = score + Score.escape.criticalThreatBonus
                end
            end

            if option.path and option.path.isTeleportAction then
                score = score - Score.escape.teleportPenalty
            end
            if improvesDistance and score > bestScore then
                best = option
                bestScore = score
            end
        end
    end

    return best, bestScore
end

local function chooseObject(input, memory)
    local best
    local bestScore = Score.impossible
    for _, target in ipairs(asArray(input.actionSpace and input.actionSpace.reachableObjects)) do
        if target.planAction then
            local score = scoreObject(target, memory, input)
            if score > bestScore then
                best = target
                bestScore = score
            end
        end
    end
    return best, bestScore
end

function Script.planDay(input)
    -- The host calls exactly this method. Keep all game-state mutation out of
    -- Lua: read input, update memory, return a declarative plan.
    local memory = initializeMemory(input)
    markProgress(memory, input.progress)
    rememberOpponentUpdates(input, memory)
    assignRoles(input, memory)

    if hasFailures(input.progress) then
        memory.lastFailure = "replanning after failed action"
    end

    local actions = {}
    local intents = {}

    local pressure = defensePressure(input)
    local build, recruit
    -- Town defense pressure changes the daily opening: spend on troops first,
    -- then fall back to construction only if no useful recruitment exists.
    if pressure >= Pressure.high then
        recruit = chooseRecruit(input)
        if recruit then
            actions[#actions + 1] = copyAction(recruit.planAction)
            intents[#intents + 1] = "recruit under pressure " .. tostring(recruit.creature or recruit.creature_id)
        else
            build = chooseBuild(input)
            if build then
                actions[#actions + 1] = copyAction(build.planAction)
                intents[#intents + 1] = "build under pressure " .. tostring(build.building or build.building_id)
            end
        end
    else
        -- In normal development, take the best economic/strategic build first.
        -- Recruitment is considered later only if no build was selected.
        build = chooseBuild(input)
        if build then
            actions[#actions + 1] = copyAction(build.planAction)
            intents[#intents + 1] = "build " .. tostring(build.building or build.building_id)
        end
    end

    if not build and not recruit and pressure < Pressure.high then
        recruit = chooseRecruit(input)
        if recruit then
            actions[#actions + 1] = copyAction(recruit.planAction)
            intents[#intents + 1] = "recruit " .. tostring(recruit.creature or recruit.creature_id)
        end
    end

    -- Escape movement has priority over normal target visits. Mixing both in a
    -- single plan could spend movement on the target before the hero is safe.
    local escapeMove = chooseEscapeMove(input)
    if escapeMove then
        actions[#actions + 1] = copyAction(escapeMove.planAction)
        intents[#intents + 1] = "move threatened hero " .. tostring(escapeMove.hero or escapeMove.hero_id)
    end

    local target = chooseObject(input, memory)
    if target and not escapeMove then
        actions[#actions + 1] = copyAction(target.planAction)
        intents[#intents + 1] = "visit " .. tostring((target.object or {}).name or (target.object or {}).id)
    end

    if #actions > 0 then
        -- need_replan asks the host to execute this batch and call us again if
        -- anything failed, stopped early, or left more useful work for the day.
        memory.lastIntent = table.concat(intents, "; ")
        return {
            status = "need_replan",
            memory = memory,
            actions = actions,
            intent = memory.lastIntent,
            confidence = Confidence.scriptedPlan
        }
    end

    -- No useful candidate remains. Ending the turn is an explicit action so the
    -- host does not keep asking the script to produce an empty plan forever.
    return {
        status = "end_turn",
        memory = memory,
        actions = {
            { type = "end_turn" }
        },
        intent = "No useful scripted candidate remains.",
        confidence = Confidence.idle
    }
end

return Script
