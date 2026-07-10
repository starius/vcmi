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

    hire = {
        base = 350,
        firstScout = 5000,
        expansionScout = 1200,
        tooManyPenalty = 700,
        totalStrength = 0.02,
        armyStrength = 0.04,
        lowGoldPenalty = 1000,
        defensePressurePenalty = 1000
    },

    transfer = {
        base = 300,
        sourceStrength = 0.03,
        gatherToMain = 650,
        gatherToScoutPenalty = 250,
        reinforceTownPressure = 550,
        criticalGatherPenalty = 400,
        tinySourcePenalty = 350
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

-- Stable enum values mirrored from the C++ scripted adventure host. Lua policy
-- decisions should use these numeric fields, not localized display names.
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
    mine = 3,
    artifact = 4,
    town = 5,
    hero = 6,
    creatureBank = 7,
    dwelling = 8,
    monster = 9,
    teleport = 10,
    shrine = 11,
    visitBonus = 12,
    market = 13,
    quest = 14
}

local TransferKind = {
    gatherToHero = 1,
    reinforceTown = 2
}

local ThreatLevel = {
    watch = 1,
    high = 2,
    critical = 3
}

local PathAction = {
    unknown = 0,
    embark = 1,
    disembark = 2,
    normal = 3,
    battle = 4,
    visit = 5,
    blockingVisit = 6,
    teleportNormal = 7,
    teleportBlockingVisit = 8,
    teleportBattle = 9
}

-- Raw engine IDs are kept as compatibility fallbacks for older traces/tests
-- that do not yet include host-provided kind IDs.
local Building = {
    mageGuild1 = 0,
    mageGuild5 = 4,
    fort = 7,
    citadel = 8,
    castle = 9,
    villageHall = 10,
    townHall = 11,
    cityHall = 12,
    capitol = 13,
    marketplace = 14,
    resourceSilo = 15,
    blacksmith = 16,
    special1 = 17,
    horde1 = 18,
    horde1Upgrade = 19,
    special2 = 21,
    special3 = 22,
    special4 = 23,
    horde2 = 24,
    horde2Upgrade = 25,
    extraTownHall = 27,
    extraCityHall = 28,
    extraCapitol = 29,
    dwelling1 = 30,
    dwelling7Upgrade5 = 64,
    dwelling8 = 150,
    dwelling8Upgrade5 = 155
}

local Obj = {
    artifact = 5,
    creatureBank = 16,
    mine = 53,
    monster = 54,
    randomArtifact = 65,
    randomRelicArtifact = 69,
    randomMonster = 71,
    randomMonsterLevel4 = 75,
    randomResource = 76,
    randomTown = 77,
    resource = 79,
    seaChest = 82,
    shrineOfMagicIncantation = 88,
    shrineOfMagicThought = 90,
    spellScroll = 93,
    town = 98,
    treasureChest = 101,
    randomMonsterLevel5 = 162,
    randomMonsterLevel7 = 164,
    randomDwelling = 216,
    randomDwellingFaction = 218,
    abandonedMine = 220
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

-- Normalize legacy labels only as a fallback for old traces or hand-written
-- fixtures. Current host input exposes numeric ids for policy decisions.
local function text(value)
    return string.lower(tostring(value or ""))
end

local function pressureFromAlert(alert)
    local level = tonumber(alert and (alert.levelId or alert.level_id) or nil)
    if level then
        if level >= ThreatLevel.critical then
            return Pressure.critical
        end
        if level >= ThreatLevel.high then
            return Pressure.high
        end
        if level >= ThreatLevel.watch then
            return Pressure.low
        end
        return Pressure.none
    end

    local label = text(alert and alert.level)
    if label == "critical" then
        return Pressure.critical
    end
    if label == "high" then
        return Pressure.high
    end
    if label ~= "" then
        return Pressure.low
    end
    return Pressure.none
end

local function pathActionId(path)
    local action = tonumber(path and (path.pathActionId or path.path_action_id) or nil)
    if action then
        return action
    end

    local label = text(path and path.pathAction)
    if label == "embark" then
        return PathAction.embark
    end
    if label == "disembark" then
        return PathAction.disembark
    end
    if label == "normal" then
        return PathAction.normal
    end
    if label == "battle" then
        return PathAction.battle
    end
    if label == "visit" then
        return PathAction.visit
    end
    if label == "blocking_visit" then
        return PathAction.blockingVisit
    end
    if label == "teleport_normal" then
        return PathAction.teleportNormal
    end
    if label == "teleport_blocking_visit" then
        return PathAction.teleportBlockingVisit
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

local function buildingId(option)
    return tonumber(option and option.building_id or nil)
end

local function inferredBuildingKindFromId(id)
    if not id then
        return BuildingKind.unknown
    end
    if id >= Building.mageGuild1 and id <= Building.mageGuild5 then
        return BuildingKind.mageGuild
    end
    if id == Building.fort or id == Building.citadel or id == Building.castle then
        return BuildingKind.fortification
    end
    if id == Building.villageHall or id == Building.townHall or id == Building.cityHall or id == Building.capitol
        or id == Building.extraTownHall or id == Building.extraCityHall or id == Building.extraCapitol
    then
        return BuildingKind.hall
    end
    if id == Building.marketplace then
        return BuildingKind.market
    end
    if id == Building.resourceSilo then
        return BuildingKind.resourceSilo
    end
    if id == Building.blacksmith then
        return BuildingKind.blacksmith
    end
    if id == Building.special1 or id == Building.special2
        or id == Building.special3 or id == Building.special4
    then
        return BuildingKind.special
    end
    if id == Building.horde1 or id == Building.horde1Upgrade
        or id == Building.horde2 or id == Building.horde2Upgrade
    then
        return BuildingKind.horde
    end
    if (id >= Building.dwelling1 and id <= Building.dwelling7Upgrade5)
        or (id >= Building.dwelling8 and id <= Building.dwelling8Upgrade5)
    then
        return BuildingKind.dwelling
    end
    return BuildingKind.unknown
end

local function buildingKindId(option)
    local kind = tonumber(option and (option.buildingKindId or option.building_kind_id) or nil)
    if kind then
        return kind
    end
    return inferredBuildingKindFromId(buildingId(option))
end

local function buildingLevel(option)
    local level = tonumber(option and (option.buildingLevel or option.building_level) or nil)
    if level then
        return level
    end

    local id = buildingId(option)
    if not id then
        return 0
    end
    if id >= Building.mageGuild1 and id <= Building.mageGuild5 then
        return id - Building.mageGuild1 + 1
    end
    if id == Building.villageHall then
        return 1
    end
    if id == Building.townHall or id == Building.extraTownHall then
        return 2
    end
    if id == Building.cityHall or id == Building.extraCityHall then
        return 3
    end
    if id == Building.capitol or id == Building.extraCapitol then
        return 4
    end
    if id == Building.fort then
        return 1
    end
    if id == Building.citadel then
        return 2
    end
    if id == Building.castle then
        return 3
    end
    if id >= Building.dwelling1 and id <= Building.dwelling7Upgrade5 then
        return ((id - Building.dwelling1) % 7) + 1
    end
    if id >= Building.dwelling8 and id <= Building.dwelling8Upgrade5 then
        return 8
    end
    return 0
end

local function isTownHallBuilding(option)
    return buildingKindId(option) == BuildingKind.hall and buildingLevel(option) == 2
end

local function isCityHallOrCapitolBuilding(option)
    return buildingKindId(option) == BuildingKind.hall and buildingLevel(option) >= 3
end

local function isFortificationBuilding(option)
    return buildingKindId(option) == BuildingKind.fortification
end

local function isDwellingBuilding(option)
    return buildingKindId(option) == BuildingKind.dwelling
end

local function isCreatureGrowthBuilding(option)
    return buildingKindId(option) == BuildingKind.horde
end

local function isStrategicSpecialBuilding(option)
    return buildingKindId(option) == BuildingKind.special
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

local function heroCount(input)
    local count = 0
    for _ in ipairs(asArray(input.state and input.state.heroes)) do
        count = count + 1
    end
    return count
end

local function experimentalSupportActionsEnabled(input)
    return input.analysis and input.analysis.experimentalSupportActions == true
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
        pressure = math.max(pressure, pressureFromAlert(alert))
    end
    return pressure
end

-- Hero threat pressure is separate from town pressure. It increases recruitment
-- priority even when no town is under immediate threat.
local function heroThreatPressure(input)
    local pressure = Pressure.none
    for _, alert in ipairs(asArray(input.analysis and input.analysis.heroThreatAlerts)) do
        pressure = math.max(pressure, pressureFromAlert(alert))
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
    local gold = resourceValue(input.state and input.state.resources, "gold")
    local costGold = resourceValue(option.cost, "gold")
    local incomeGold = resourceValue(option.income, "gold")
    local pressure = defensePressure(input)

    -- Income buildings are valuable in normal development because they compound
    -- over future days. City Hall and Capitol get extra priority because their
    -- stable host-provided kind/level marks them as economic milestones.
    score = score + incomeGold * Score.build.incomeGold
    if isCityHallOrCapitolBuilding(option) then
        score = score + Score.build.cityHallOrCapitol
    end
    if isTownHallBuilding(option) then
        score = score + Score.build.townHall
    end
    if isFortificationBuilding(option) then
        score = score + Score.build.castleOrCitadel
    end
    if isDwellingBuilding(option)
        or isCreatureGrowthBuilding(option)
        or isStrategicSpecialBuilding(option)
    then
        score = score + Score.build.dwellingOrPortal
    end
    -- Under pressure, defensive buildings and creature production beat pure
    -- income because surviving the next enemy move matters more than payback.
    if pressure >= Pressure.high then
        if isFortificationBuilding(option) then
            score = score + Score.build.pressuredCastleOrCitadel
        elseif isDwellingBuilding(option) or isCreatureGrowthBuilding(option) then
            score = score + Score.build.pressuredDwelling
        elseif incomeGold > 0 then
            score = score - Score.build.pressuredIncomePenalty
        end
    end
    if gold < Threshold.buildLowGold
        and incomeGold <= 0
        and not isFortificationBuilding(option)
        and not isDwellingBuilding(option)
        and not isCreatureGrowthBuilding(option)
    then
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

local function scoreHire(option, input)
    local gold = resourceValue(input.state and input.state.resources, "gold")
    local costGold = resourceValue(option.cost, "gold")
    local heroes = heroCount(input)
    local pressure = defensePressure(input)
    local score = Score.hire.base

    -- Extra heroes are most valuable early, when they can scout, collect loose
    -- resources, and later participate in chaining. Avoid turning late-game
    -- tavern availability into automatic spending when enough heroes exist.
    if heroes < 2 then
        score = score + Score.hire.firstScout
    elseif heroes < 4 then
        score = score + Score.hire.expansionScout
    else
        score = score - (heroes - 3) * Score.hire.tooManyPenalty
    end

    score = score + (tonumber(option.totalStrength or 0) or 0) * Score.hire.totalStrength
    score = score + (tonumber(option.armyStrength or 0) or 0) * Score.hire.armyStrength
    if costGold > 0 and gold < costGold + Threshold.recruitLowGold then
        score = score - Score.hire.lowGoldPenalty
    end
    if pressure >= Pressure.high then
        score = score - pressure * Score.hire.defensePressurePenalty
    end
    return score
end

local function chooseHire(input)
    if not experimentalSupportActionsEnabled(input) then
        return nil, Score.impossible
    end

    local best
    local bestScore = Score.impossible
    for _, option in ipairs(asArray(input.actionSpace and input.actionSpace.hireHeroOptions)) do
        if option.planAction then
            local score = scoreHire(option, input)
            if score > bestScore then
                best = option
                bestScore = score
            end
        end
    end
    return best, bestScore
end

local function transferKindId(option)
    return tonumber(option and (option.transferKindId or option.transfer_kind_id) or 0) or 0
end

local function scoreTransfer(option, memory, input)
    local pressure = defensePressure(input)
    local sourceStrength = tonumber(option.sourceArmyStrength or option.value or 0) or 0
    local destinationId = tostring(option.destination_id or "")
    local kind = transferKindId(option)
    local score = Score.transfer.base + sourceStrength * Score.transfer.sourceStrength

    -- Reinforcing towns is mostly a defensive reaction. Gathering town/garrison
    -- troops into the main hero is the normal development pattern that lets the
    -- script avoid leaving combat power stranded in static armies.
    if kind == TransferKind.reinforceTown then
        if pressure < Pressure.high then
            return Score.impossible
        end
        score = score + pressure * Score.transfer.reinforceTownPressure
    elseif kind == TransferKind.gatherToHero then
        if roleForHero(memory, destinationId) == "main" then
            score = score + Score.transfer.gatherToMain
        else
            score = score - Score.transfer.gatherToScoutPenalty
        end
        if pressure >= Pressure.critical then
            score = score - Score.transfer.criticalGatherPenalty
        end
    end

    if sourceStrength < 250 then
        score = score - Score.transfer.tinySourcePenalty
    end
    return score
end

local function chooseTransfer(input, memory)
    if not experimentalSupportActionsEnabled(input) then
        return nil, Score.impossible
    end

    local best
    local bestScore = Score.impossible
    for _, option in ipairs(asArray(input.actionSpace and input.actionSpace.armyTransferOptions)) do
        if option.planAction then
            local score = scoreTransfer(option, memory, input)
            if score > bestScore then
                best = option
                bestScore = score
            end
        end
    end
    return best, bestScore
end

local function inferredObjectKindFromTypeId(typeId)
    if not typeId then
        return ObjectKind.unknown
    end
    if typeId == Obj.artifact or typeId == Obj.spellScroll
        or (typeId >= Obj.randomArtifact and typeId <= Obj.randomRelicArtifact)
    then
        return ObjectKind.artifact
    end
    if typeId == Obj.creatureBank then
        return ObjectKind.creatureBank
    end
    if typeId == Obj.mine or typeId == Obj.abandonedMine then
        return ObjectKind.mine
    end
    if typeId == Obj.resource or typeId == Obj.randomResource then
        return ObjectKind.resource
    end
    if typeId == Obj.seaChest or typeId == Obj.treasureChest then
        return ObjectKind.treasure
    end
    if typeId == Obj.town or typeId == Obj.randomTown then
        return ObjectKind.town
    end
    if typeId == Obj.monster
        or (typeId >= Obj.randomMonster and typeId <= Obj.randomMonsterLevel4)
        or (typeId >= Obj.randomMonsterLevel5 and typeId <= Obj.randomMonsterLevel7)
    then
        return ObjectKind.monster
    end
    if typeId >= Obj.shrineOfMagicIncantation and typeId <= Obj.shrineOfMagicThought then
        return ObjectKind.shrine
    end
    if typeId >= Obj.randomDwelling and typeId <= Obj.randomDwellingFaction then
        return ObjectKind.dwelling
    end
    return ObjectKind.unknown
end

local function objectKindId(object)
    local kind = tonumber(object and (object.kindId or object.objectKindId or object.kind_id) or nil)
    if kind then
        return kind
    end
    return inferredObjectKindFromTypeId(tonumber(object and object.typeId or nil))
end

local function scoreObject(target, memory, input)
    local object = target.object or {}
    local path = target.path or {}
    local kind = objectKindId(object)
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

    -- Object preferences use stable host kind IDs. Localized names and hover
    -- text may appear in traces, but they are not strategy inputs.
    if kind == ObjectKind.artifact then
        score = score + Score.object.treasureBonus
    end
    if kind == ObjectKind.town and role == "main" and target.safe ~= false then
        score = score + Score.object.mineBonus + Score.object.treasureBonus
    end
    if kind == ObjectKind.creatureBank and role == "main" and target.safe ~= false then
        score = score + Score.object.treasureBonus
    end
    if kind == ObjectKind.treasure then
        score = score + Score.object.treasureBonus
    end
    if kind == ObjectKind.resource then
        score = score + Score.object.resourceBonus
    end
    if kind == ObjectKind.mine then
        score = score + Score.object.mineBonus + Score.object.repeatedMineWordBonus
    end
    if isBattlePath(path) then
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
                if pressureFromAlert(threat) >= Pressure.critical then
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
    local build, recruit, hire
    -- Town defense pressure changes the daily opening: spend on troops first,
    -- then fall back to construction only if no useful recruitment exists.
    if pressure >= Pressure.high then
        recruit = chooseRecruit(input)
        if recruit then
            actions[#actions + 1] = copyAction(recruit.planAction)
            intents[#intents + 1] = "recruit under pressure creature " .. tostring(recruit.creature_id)
        else
            build = chooseBuild(input)
            if build then
                actions[#actions + 1] = copyAction(build.planAction)
                intents[#intents + 1] = "build under pressure building " .. tostring(build.building_id)
            end
        end
    else
        -- In normal development, early extra heroes compete with construction:
        -- the host still validates tavern, gold, and hero caps, while Lua
        -- decides when map tempo is worth delaying a building.
        local buildScore
        local hireScore
        build, buildScore = chooseBuild(input)
        hire, hireScore = chooseHire(input)
        if hire and hireScore > buildScore then
            actions[#actions + 1] = copyAction(hire.planAction)
            intents[#intents + 1] = "hire hero " .. tostring(hire.hero_type_id)
        elseif build then
            actions[#actions + 1] = copyAction(build.planAction)
            intents[#intents + 1] = "build building " .. tostring(build.building_id)
        end
    end

    if not build and not recruit and not hire and pressure < Pressure.high then
        recruit = chooseRecruit(input)
        if recruit then
            actions[#actions + 1] = copyAction(recruit.planAction)
            intents[#intents + 1] = "recruit creature " .. tostring(recruit.creature_id)
        end
    end

    -- Escape is decided before support-action replanning so hiring or army
    -- transfers cannot hide an urgent hero threat response from the host.
    local escapeMove = chooseEscapeMove(input)

    local transfer = chooseTransfer(input, memory)
    if transfer then
        actions[#actions + 1] = copyAction(transfer.planAction)
        intents[#intents + 1] = "transfer army " .. tostring(transfer.source_id) .. " to " .. tostring(transfer.destination_id)
    end

    if escapeMove then
        actions[#actions + 1] = copyAction(escapeMove.planAction)
        intents[#intents + 1] = "move threatened hero " .. tostring(escapeMove.hero_id)
    end

    if hire or transfer then
        memory.lastIntent = table.concat(intents, "; ")
        return {
            status = "need_replan",
            memory = memory,
            actions = actions,
            intent = memory.lastIntent,
            confidence = Confidence.scriptedPlan
        }
    end

    -- Escape movement has priority over normal target visits. Mixing both in a
    -- single plan could spend movement on the target before the hero is safe.
    local target = chooseObject(input, memory)
    if target and not escapeMove then
        actions[#actions + 1] = copyAction(target.planAction)
        intents[#intents + 1] = "visit object " .. tostring((target.object or {}).id)
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

    -- No high-confidence scripted action remains. The current script interface
    -- still cannot express Nullkiller's full task graph, especially multi-hero
    -- chaining and deeper blocker plans. Delegating the remainder preserves
    -- those capabilities instead of ending with useful but unsupported work.
    return {
        status = "fallback",
        memory = memory,
        actions = {},
        intent = "No high-confidence scripted candidate remains; delegate remaining turn to Nullkiller.",
        confidence = Confidence.idle
    }
end

function Script.runDay(ai, input)
    -- Imperative compatibility entry point. The old policy still computes one
    -- small decision at a time, but this wrapper now executes each selected
    -- action through the checked host API and refreshes visible state after
    -- every side effect. Future policy work should move decisions directly into
    -- this coroutine instead of returning declarative batches.
    local current = input
    local callLimit = (((current or {}).limits or {}).maxScriptCallsPerTurn) or 8

    for _ = 1, callLimit do
        local output = Script.planDay(current)
        ai:setMemory(output.memory or ai:memory())

        if output.status == "fallback" then
            return ai:nullkiller(output.intent)
        end

        local shouldReplan = false
        for _, action in ipairs(output.actions or {}) do
            local ok, result = pcall(function()
                return ai:execute(action)
            end)

            current = ai:refresh()
            if not ok or (type(result) == "table" and result.stop) then
                shouldReplan = true
                break
            end
        end

        if output.status == "end_turn" then
            ai:endTurn()
            return ai:output("end_turn", output.intent, output.confidence)
        end

        if output.status ~= "need_replan" and not shouldReplan then
            return ai:nullkiller(output.intent or "script completed its imperative actions")
        end

        current = current or ai:refresh()
        current.memory = ai:memory()
    end

    return ai:nullkiller("imperative compatibility wrapper reached replan limit")
end

return Script
