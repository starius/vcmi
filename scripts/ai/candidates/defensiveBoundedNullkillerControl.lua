local Script = {}

--[[
Defensive bounded Nullkiller control
------------------------------------

This is an experimental candidate derived from boundedNullkillerControl.lua.
It keeps the same main contract: Lua does not delegate the whole day to
Nullkiller, and normal map play still runs through bounded native day slices.

The only added policy is a narrow emergency-town-defense opening. Critical
matching recruitment may preempt movement because queued creatures defend this
turn and trace losses repeatedly show zero-strength towns while scouts still
have ordinary movement options. Construction is more expensive and slower, so
building remains gated on the scripted player having no practical map tempo
left.

This is deliberately conservative:

* It uses numeric threat levels and town/source ids, not localized labels.
* It executes host-provided planAction payloads through ai:runOption.
* It does not fire merely because a defense alert exists; ordinary turns stay
  under bounded Nullkiller control unless a critical threatened town can recruit
  immediately.
* It prefers recruitment over construction because creatures can defend now.
* It does not invent prices, availability, paths, or game mechanics in Lua.
* After the emergency action it refreshes and returns to bounded Nullkiller.

The candidate exists to test trace-mined defense_pressure_without_response
losses without promoting the previously rejected unconditional defense pass.
]]

local MemoryVersion = 1
local DefaultMaxCommandsPerDay = 256
local SafetyMaxSlicesPerDay = 256
local DefaultMaxPassesPerSlice = 16
local SafetyMaxPassesPerSlice = 64
local DefaultMaxCandidates = 0
local DefaultMaxAttempts = 0
local MaxQueriesPerSlice = 16

local ThreatLevel = {
    watch = 1,
    high = 2,
    critical = 3
}

local BuildingKind = {
    unknown = 0,
    fortification = 4,
    hall = 5,
    horde = 10,
    dwelling = 11
}

local DefenseScore = {
    recruitLevel = 1000,
    recruitAmount = 10,
    criticalBonus = 5000,
    fortification = 4500,
    dwelling = 3200,
    horde = 2600,
    nonDefenseBuild = 300,
    hallPenalty = 1500,
    goldCost = 0.02
}

local DefenseTrigger = {
    -- Broad pressure responses regressed winning seeds. Only intervene when
    -- native planning has little map tempo left and the town threat is severe.
    -- Recruitment is the exception: it is cheap, immediate, and trace-mined
    -- losses repeatedly show critical zero-garrison towns while scouts still
    -- have ordinary movement options.
    maxMovementOptionsWithTempo = 0
}

local function adoptHostConstants(ai)
    if type(ai) ~= "table" then
        return
    end
    if type(ai.threatLevels) == "table" then
        ThreatLevel = ai.threatLevels
    end
    if type(ai.buildingKinds) == "table" then
        BuildingKind = ai.buildingKinds
    end
end

local function asArray(value)
    if type(value) == "table" then
        return value
    end
    return {}
end

local function normalizeMemory(input)
    local memory = (input and input.memory) or {}
    if memory.version ~= MemoryVersion then
        memory = { version = MemoryVersion }
    end

    memory.days = memory.days or 0
    memory.totalSlices = memory.totalSlices or 0
    memory.totalQueriesAnswered = memory.totalQueriesAnswered or 0
    memory.totalEmergencyDefenseActions = memory.totalEmergencyDefenseActions or 0
    return memory
end

local function turnIsActive(input)
    local turn = input and input.state and input.state.turn
    if not turn then
        return true
    end
    return turn.active ~= false
end

local function commandBudget(input)
    local limits = (input and input.limits) or {}
    local actions = tonumber(limits.maxActions)
    local scriptCalls = tonumber(limits.maxScriptCallsPerTurn)
    local budget = actions or scriptCalls or DefaultMaxCommandsPerDay

    return math.max(1, math.min(SafetyMaxSlicesPerDay, budget))
end

local function nullkillerSettings(input)
    local analysis = (input and input.analysis) or {}
    local nullkiller = analysis.nullkiller or {}
    return nullkiller.settings or {}
end

local function pendingQueries(input)
    local turn = input and input.state and input.state.turn
    return (turn and turn.queries) or {}
end

local function hasPendingQueries(input)
    return #pendingQueries(input) > 0
end

local function heroCount(input)
    return #asArray(input.state and input.state.heroes)
end

local function movementOptionCount(input)
    return #asArray(input.actionSpace and input.actionSpace.movementOptions)
end

local function lacksMapTempo(input)
    return heroCount(input) == 0
        or movementOptionCount(input) <= DefenseTrigger.maxMovementOptionsWithTempo
end

local function answerPendingQueries(ai, current, memory)
    local answered = ai:answerPendingQueriesByPolicy(function()
        return { useNullkiller = true, default_answer = 0 }
    end, { max_queries = MaxQueriesPerSlice })

    memory.totalQueriesAnswered = memory.totalQueriesAnswered + answered.count
    current = ai.input or current

    if answered.truncated or hasPendingQueries(current) then
        error("defensive bounded control could not clear pending queries within one slice")
    end

    if answered.count == 1 then
        memory.lastIntent = "answered one pending query before bounded native planning"
    elseif answered.count > 1 then
        memory.lastIntent = "answered " .. tostring(answered.count) .. " pending queries before bounded native planning"
    end

    return current
end

local function threatLevel(alert)
    local numeric = tonumber(alert and (alert.levelId or alert.level_id) or nil)
    if numeric then
        return numeric
    end

    -- Label fallback is only for old fixtures and hand-written traces. Live
    -- host input should use the numeric level id.
    local label = string.lower(tostring(alert and alert.level or ""))
    if label == "critical" then
        return ThreatLevel.critical
    end
    if label == "high" then
        return ThreatLevel.high
    end
    if label ~= "" then
        return ThreatLevel.watch
    end
    return 0
end

local function emergencyDefenseAlerts(input)
    local result = {}

    for _, alert in ipairs(asArray(input.analysis and input.analysis.defenseAlerts)) do
        local townId = tonumber(alert.town_id or alert.townId or nil)
        local level = threatLevel(alert)
        if townId and level >= ThreatLevel.critical then
            result[#result + 1] = {
                townId = townId,
                level = level
            }
        end
    end
    return result
end

local function resourceGold(cost)
    if type(cost) ~= "table" then
        return 0
    end
    return tonumber(cost.gold or cost[7] or 0) or 0
end

local function optionTownId(option)
    return tonumber(option.town_id or option.townId or option.source_id or option.sourceId or nil)
end

local function matchesAlert(option, alert)
    return optionTownId(option) == alert.townId
end

local function hasMatchingAlert(option, alerts)
    for _, alert in ipairs(alerts) do
        if matchesAlert(option, alert) then
            return true
        end
    end
    return false
end

local function recruitScore(option, alert)
    local score = 0
    score = score + (tonumber(option.level or 0) or 0) * DefenseScore.recruitLevel
    score = score + (tonumber(option.amount or 0) or 0) * DefenseScore.recruitAmount
    if alert.level >= ThreatLevel.critical then
        score = score + DefenseScore.criticalBonus
    end
    return score
end

local function buildingScore(option, alert)
    local kind = tonumber(option.buildingKindId or option.building_kind_id or BuildingKind.unknown) or BuildingKind.unknown
    local score = DefenseScore.nonDefenseBuild
    if kind == BuildingKind.fortification then
        score = DefenseScore.fortification
    elseif kind == BuildingKind.dwelling then
        score = DefenseScore.dwelling
    elseif kind == BuildingKind.horde then
        score = DefenseScore.horde
    elseif kind == BuildingKind.hall then
        score = score - DefenseScore.hallPenalty
    end
    if alert.level >= ThreatLevel.critical then
        score = score + DefenseScore.criticalBonus
    end
    score = score - resourceGold(option.cost) * DefenseScore.goldCost
    return score
end

local function bestAlertForOption(option, alerts)
    for _, alert in ipairs(alerts) do
        if matchesAlert(option, alert) then
            return alert
        end
    end
    return alerts[1]
end

local function chooseEmergencyRecruit(input, alerts)
    local best
    local bestScore = -math.huge
    for _, option in ipairs(asArray(input.actionSpace and input.actionSpace.recruitOptions)) do
        if option.planAction and (tonumber(option.amount or 0) or 0) > 0 then
            local alert = bestAlertForOption(option, alerts)
            local score = recruitScore(option, alert)
            if hasMatchingAlert(option, alerts) then
                score = score + DefenseScore.criticalBonus
            end
            if score > bestScore then
                best = option
                bestScore = score
            end
        end
    end
    return best
end

local function chooseEmergencyBuild(input, alerts)
    local best
    local bestScore = -math.huge
    for _, option in ipairs(asArray(input.actionSpace and input.actionSpace.buildOptions)) do
        if option.planAction then
            for _, alert in ipairs(alerts) do
                if matchesAlert(option, alert) then
                    local score = buildingScore(option, alert)
                    if score > bestScore then
                        best = option
                        bestScore = score
                    end
                end
            end
        end
    end
    return best
end

local function runEmergencyDefense(ai, current, memory)
    local alerts = emergencyDefenseAlerts(current)
    if #alerts == 0 then
        return current, false
    end

    local lowTempo = lacksMapTempo(current)
    local option = chooseEmergencyRecruit(current, alerts)
    local intent = "emergency town defense recruitment"
    if not option and lowTempo then
        option = chooseEmergencyBuild(current, alerts)
        intent = "emergency town defense construction"
    end
    if not option then
        return current, false
    end

    ai:runOption(option)
    memory.totalEmergencyDefenseActions = memory.totalEmergencyDefenseActions + 1
    memory.lastIntent = intent
    ai:setMemory(memory)
    return ai:refresh(), true
end

local function sliceDidAdventureWork(result)
    return (tonumber(result.adventureStepsExecuted or 0) or 0) > 0
        or (tonumber(result.adventureReplanSteps or 0) or 0) > 0
end

local function sliceDidPriorityWork(result)
    return (tonumber(result.priorityTasksExecuted or 0) or 0) > 0
end

local function sliceDidTrade(result)
    return (tonumber(result.tradePasses or 0) or 0) > 0
end

local function sliceDidWork(result)
    return result.didWork == true
        or sliceDidPriorityWork(result)
        or sliceDidAdventureWork(result)
        or sliceDidTrade(result)
        or result.paused == true
end

local function sliceShouldEndTurn(result)
    return result.shouldEndTurn == true
        or result.shouldStopTurn == true
        or (tonumber(result.adventureStopTurnSteps or 0) or 0) > 0
        or (result.exhaustedCandidates == true and not sliceDidWork(result))
end

local function sliceReachedNativePassLimit(result)
    return result.exhaustedBudget == true or result.status == "budget_exhausted"
end

local function runNativeSlice(ai, current)
    local settings = nullkillerSettings(current)
    local maxPasses = tonumber(settings.maxPass) or DefaultMaxPassesPerSlice

    return ai:nullkillerBoundedDay({
        max_passes = math.max(1, math.min(SafetyMaxPassesPerSlice, maxPasses)),
        max_candidates = DefaultMaxCandidates,
        max_attempts = DefaultMaxAttempts,
        max_queries_per_pass = MaxQueriesPerSlice,
        default_answer = 0
    })
end

function Script.runDay(ai, input)
    adoptHostConstants(ai)

    local memory = normalizeMemory(input)
    memory.days = memory.days + 1
    memory.slicesToday = 0
    ai:setMemory(memory)

    local current = input
    local maxSlices = commandBudget(input)

    while turnIsActive(current) and memory.slicesToday < maxSlices do
        current = answerPendingQueries(ai, current, memory)

        if not turnIsActive(current) then
            return ai:output("end_turn", "turn ended while answering defensive bounded-control queries", 0.5)
        end

        local defended
        current, defended = runEmergencyDefense(ai, current, memory)
        if defended then
            if not turnIsActive(current) then
                return ai:output("end_turn", "turn ended after emergency town defense", 0.5)
            end
        end

        local result = runNativeSlice(ai, current)
        local passCount = tonumber(result.passCount or 1) or 1
        memory.slicesToday = memory.slicesToday + passCount
        memory.totalSlices = memory.totalSlices + passCount
        ai:setMemory(memory)

        if sliceShouldEndTurn(result) then
            ai:endTurn()
            local intent = "defensive bounded control accepted native stop-turn signal"
            if result.status == "idle" then
                intent = "defensive bounded control found no remaining native work"
            end
            return ai:output("end_turn", intent, 0.5)
        end

        if sliceReachedNativePassLimit(result) then
            ai:endTurn()
            return ai:output("end_turn", "defensive bounded control accepted native max-pass limit", 0.5)
        end

        if sliceDidWork(result) then
            current = ai:refresh()
        else
            ai:endTurn()
            return ai:output("end_turn", "defensive bounded control found no remaining native work", 0.5)
        end
    end

    if not turnIsActive(current) then
        return ai:output("end_turn", "turn ended during defensive bounded control", 0.5)
    end

    error("defensive bounded control exhausted its command budget before the day was idle")
end

function Script.planDay(input)
    return {
        status = "fallback",
        memory = normalizeMemory(input),
        actions = {},
        intent = "defensive bounded control requires imperative runDay",
        confidence = 0.5
    }
end

function Script.decideBattleRetreat()
    return {
        decision_id = 0,
        intent = "defensive bounded control delegates battle retreat policy"
    }
end

return Script
