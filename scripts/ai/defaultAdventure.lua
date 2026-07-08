local Script = {}

local function asArray(value)
    if type(value) == "table" then
        return value
    end
    return {}
end

local function copyAction(action)
    local result = {}
    for key, value in pairs(action or {}) do
        result[key] = value
    end
    return result
end

local function text(value)
    return string.lower(tostring(value or ""))
end

local function resourceValue(resources, name)
    if type(resources) ~= "table" then
        return 0
    end
    return tonumber(resources[name] or 0) or 0
end

local function hasFailures(progress)
    for _ in pairs(asArray(progress and progress.failed)) do
        return true
    end
    return false
end

local function initializeMemory(input)
    local memory = input.memory or {}
    if memory.version ~= 1 then
        memory = { version = 1 }
    end
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

local function armyScore(hero)
    local score = (tonumber(hero and hero.level or 0) or 0) * 25
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

local function defensePressure(input)
    local pressure = 0
    for _, alert in ipairs(asArray(input.analysis and input.analysis.defenseAlerts)) do
        local level = text(alert.level)
        if level == "critical" then
            pressure = math.max(pressure, 3)
        elseif level == "high" then
            pressure = math.max(pressure, 2)
        else
            pressure = math.max(pressure, 1)
        end
    end
    return pressure
end

local function heroThreatPressure(input)
    local pressure = 0
    for _, alert in ipairs(asArray(input.analysis and input.analysis.heroThreatAlerts)) do
        local level = text(alert.level)
        if level == "critical" then
            pressure = math.max(pressure, 3)
        elseif level == "high" then
            pressure = math.max(pressure, 2)
        else
            pressure = math.max(pressure, 1)
        end
    end
    return pressure
end

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
        if distance and distance <= 36 then
            return true
        end
    end
    return false
end

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
    local score = 100
    local name = text(option.building)
    local gold = resourceValue(input.state and input.state.resources, "gold")
    local costGold = resourceValue(option.cost, "gold")
    local incomeGold = resourceValue(option.income, "gold")
    local pressure = defensePressure(input)

    score = score + incomeGold * 3
    if name:find("city hall", 1, true) or name:find("capitol", 1, true) then
        score = score + 900
    end
    if name:find("town hall", 1, true) then
        score = score + 450
    end
    if name:find("castle", 1, true) or name:find("citadel", 1, true) then
        score = score + 250
    end
    if name:find("dwelling", 1, true) or name:find("portal", 1, true) then
        score = score + 180
    end
    if pressure >= 2 then
        if name:find("castle", 1, true) or name:find("citadel", 1, true) then
            score = score + 450
        elseif name:find("dwelling", 1, true) then
            score = score + 120
        elseif incomeGold > 0 then
            score = score - 150
        end
    end
    if gold < 2500 and incomeGold <= 0 then
        score = score - 250
    end
    score = score - costGold * 0.015
    return score
end

local function chooseBuild(input)
    local best
    local bestScore = -1e9
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
    local score = (tonumber(option.level or 0) or 0) * 100
    score = score + (tonumber(option.amount or 0) or 0) * 8
    score = score + defensePressure(input) * 180
    score = score + heroThreatPressure(input) * 90
    if gold < 1500 then
        score = score - 300
    end
    return score
end

local function chooseRecruit(input)
    local best
    local bestScore = -1e9
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
    local score = 400
    local role = roleForHero(memory, target.hero_id)
    local objectKey = tostring(object.id or "")

    if memory.visitedTargets[objectKey] then
        score = score - 600
    end
    if memory.blockedTargets[objectKey] then
        score = score - memory.blockedTargets[objectKey] * 250
    end

    score = score - (tonumber(path.cost or 0) or 0) * 160
    score = score + (tonumber(target.value or 0) or 0) * 0.35

    local dangerRatio = tonumber(target.dangerRatio or 0) or 0
    if target.safe == false then
        if role == "main" then
            score = score - 450 - dangerRatio * 350
        else
            score = score - 1200 - dangerRatio * 500
        end
    elseif dangerRatio > 0 then
        score = score - dangerRatio * (role == "main" and 150 or 350)
    end

    if name:find("gold", 1, true) or name:find("treasure", 1, true) or name:find("chest", 1, true) then
        score = score + 350
    end
    if name:find("mine", 1, true) or name:find("sawmill", 1, true) or name:find("ore pit", 1, true) then
        score = score + 450
    end
    if name:find("resource", 1, true) or name:find("wood", 1, true) or name:find("ore", 1, true) then
        score = score + 220
    end
    if name:find("mine", 1, true) then
        score = score + 180
    end
    if path.pathAction == "battle" or path.pathAction == "teleport_battle" then
        if role == "main" then
            score = score - 120
        else
            score = score - 700
        end
    end
    if path.isTeleportAction then
        score = score - 120
    end
    if opponentNearTarget(input, target) then
        if role == "main" then
            score = score - 150
        else
            score = score - 550
        end
    end
    if defensePressure(input) >= 3 and role == "main" then
        score = score - 350
    end
    if role == "scout" and not path.isTeleportAction then
        score = score + 80
    end
    return score
end

local function chooseEscapeMove(input)
    local threats = heroThreatsByHero(input)
    local best
    local bestScore = -1e9

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
                    score = score + nextDistance * 0.5
                    if nextDistance > currentDistance then
                        improvesDistance = true
                    end
                end
                if text(threat.level) == "critical" then
                    score = score + 300
                end
            end

            if option.path and option.path.isTeleportAction then
                score = score - 250
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
    local bestScore = -1e9
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
    local memory = initializeMemory(input)
    markProgress(memory, input.progress)
    rememberOpponentUpdates(input, memory)
    assignRoles(input, memory)

    if hasFailures(input.progress) then
        return {
            status = "fallback",
            memory = memory,
            actions = {},
            intent = "Scripted action failed; delegate the rest of the turn."
        }
    end

    local actions = {}
    local intents = {}

    local pressure = defensePressure(input)
    local build, recruit
    if pressure >= 2 then
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
        build = chooseBuild(input)
        if build then
            actions[#actions + 1] = copyAction(build.planAction)
            intents[#intents + 1] = "build " .. tostring(build.building or build.building_id)
        end
    end

    if not build and not recruit and pressure < 2 then
        recruit = chooseRecruit(input)
        if recruit then
            actions[#actions + 1] = copyAction(recruit.planAction)
            intents[#intents + 1] = "recruit " .. tostring(recruit.creature or recruit.creature_id)
        end
    end

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
        memory.lastIntent = table.concat(intents, "; ")
        return {
            status = "need_replan",
            memory = memory,
            actions = actions,
            intent = memory.lastIntent,
            confidence = 0.55
        }
    end

    return {
        status = "end_turn",
        memory = memory,
        actions = {
            { type = "end_turn" }
        },
        intent = "No useful scripted candidate remains.",
        confidence = 0.5
    }
end

return Script
