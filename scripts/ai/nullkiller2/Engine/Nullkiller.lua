-- Mirrors AI/Nullkiller2/Engine/Nullkiller.cpp: Nullkiller::makeTurn.

local BuildingBehavior = require("Behaviors.BuildingBehavior")
local BuyArmyBehavior = require("Behaviors.BuyArmyBehavior")
local CaptureObjectsBehavior = require("Behaviors.CaptureObjectsBehavior")
local ClusterBehavior = require("Behaviors.ClusterBehavior")
local DeepDecomposer = require("Engine.DeepDecomposer")
local DefenceBehavior = require("Behaviors.DefenceBehavior")
local EscapeBehavior = require("Behaviors.EscapeBehavior")
local ExplorationBehavior = require("Behaviors.ExplorationBehavior")
local ExchangeSwapTownHeroes = require("Goals.ExchangeSwapTownHeroes")
local GatherArmyBehavior = require("Behaviors.GatherArmyBehavior")
local GatewayPolicy = require("Actions.GatewayPolicy")
local HostCommands = require("Actions.HostCommands")
local PriorityEvaluator = require("Engine.PriorityEvaluator")
local RecruitHero = require("Goals.RecruitHero")
local RecruitHeroBehavior = require("Behaviors.RecruitHeroBehavior")
local ResourceTrader = require("Engine.ResourceTrader")
local Settings = require("Engine.Settings")
local State = require("Engine.State")
local TaskPlan = require("Engine.TaskPlan")

local Nullkiller = {}
local MAX_DEPTH = 10

local function call(object, name, ...)
	if object and type(object[name]) == "function" then
		return object[name](object, ...)
	end
	return nil
end

local function objectID(value)
	if type(value) == "number" then
		return value
	end
	if type(value) == "table" then
		return value.id or value.objectID or value.objectId or value.num or value[1]
	end
	return value
end

local function ownerID(value)
	if type(value) ~= "table" then
		return nil
	end
	return objectID(value.tempOwner or value.owner)
end

local function sameOwner(left, right)
	local leftOwner = ownerID(left)
	local rightOwner = ownerID(right)
	return leftOwner ~= nil and rightOwner ~= nil and leftOwner == rightOwner
end

local function trace(ai, event, data)
	if ai and ai.trace then
		ai:trace(event, data or {})
	end
end

local function endTurn(ai)
	if ai and ai.endTurn then
		return ai:endTurn()
	end

	return { ok = true, skippedHostEndTurn = true }
end

local function loadSettings(input)
	local settingsInput = input.settings or {}

	if settingsInput.root and settingsInput.difficultyName then
		return Settings.fromDifficultyConfig(settingsInput.root, settingsInput.difficultyName)
	end

	return Settings.withDefaults(settingsInput.values)
end

local function maxPass(settings)
	return call(settings, "getMaxPass") or settings.maxPass or 1
end

local function maxPriorityPass(settings)
	return call(settings, "getMaxPriorityPass") or settings.maxPriorityPass or 1
end

local function playerInGame(aiNk)
	local status = call(aiNk.cc, "getPlayerStatus", aiNk.playerID) or aiNk.playerStatus or "INGAME"
	return status == "INGAME" or status == true or status == 0
end

local function movementPoints(hero)
	local result = call(hero, "movementPointsRemaining")
	if result ~= nil then
		return result
	end
	return hero and (hero.movementPointsRemaining or hero.movementPoints or 0) or 0
end

local function isGarrisoned(hero)
	local result = call(hero, "isGarrisoned")
	if result ~= nil then
		return result
	end
	return hero and hero.garrisoned == true
end

local function heroesInfo(aiNk)
	return call(aiNk.cc, "getHeroesInfo") or aiNk.heroesInfo or {}
end

local function townsInfo(aiNk)
	return call(aiNk.cc, "getTownsInfo") or aiNk.townsInfo or {}
end

local function addByID(index, object)
	local id = objectID(object)
	if id ~= nil and object ~= nil then
		index[id] = object
	end
end

local function copyIndex(index)
	local result = {}
	for key, value in pairs(index or {}) do
		result[key] = value
	end
	return result
end

local function indexPathHeroes(index, path)
	addByID(index, path and path.targetHero)
	for _, node in ipairs(path and path.nodes or {}) do
		addByID(index, node.targetHero)
	end
end

local function indexHeroSnapshot(index, hero)
	addByID(index, hero)
end

local function indexObjectSnapshot(objectIndex, heroIndex, object)
	addByID(objectIndex, object)
	for _, path in ipairs(object and object.paths or {}) do
		indexPathHeroes(heroIndex, path)
	end
end

local function ensureSnapshotIndexes(input)
	input = input or {}
	input.heroesByID = copyIndex(input.heroesByID)
	input.objectsByID = copyIndex(input.objectsByID)

	for _, hero in ipairs(input.heroesInfo or {}) do
		indexHeroSnapshot(input.heroesByID, hero)
	end

	for _, town in ipairs(input.townsInfo or {}) do
		indexObjectSnapshot(input.objectsByID, input.heroesByID, town)
		indexHeroSnapshot(input.heroesByID, call(town, "getGarrisonHero") or town.garrisonHero)
		indexHeroSnapshot(input.heroesByID, call(town, "getVisitingHero") or town.visitingHero)
		for _, hero in ipairs(town.availableHeroes or {}) do
			indexHeroSnapshot(input.heroesByID, hero)
		end
	end

	for _, listName in ipairs({ "nearbyObjects", "farObjects", "visitableObjects", "objects" }) do
		for _, object in ipairs(input[listName] or {}) do
			indexObjectSnapshot(input.objectsByID, input.heroesByID, object)
		end
	end

	return input
end

local function getObj(aiNk, object)
	local id = objectID(object)
	local result = call(aiNk.cc, "getObj", id, false)
	if result ~= nil then
		return result
	end
	local objects = aiNk.objectsByID or {}
	return objects[id]
end

local function getHero(aiNk, object)
	local id = objectID(object)
	local result = call(aiNk.cc, "getHero", id)
	if result ~= nil then
		return result
	end
	local heroes = aiNk.heroesByID or {}
	return heroes[id]
end

local function makeCallbackFacade(input)
	local explicit = input.cc or {}
	return setmetatable({}, {
		__index = function(_, key)
			if explicit[key] ~= nil then
				return explicit[key]
			end
			if key == "getTownsInfo" then
				return function()
					return input.townsInfo or {}
				end
			end
			if key == "getHeroesInfo" then
				return function()
					return input.heroesInfo or {}
				end
			end
			if key == "getAvailableHeroes" then
				return function(_, town)
					return town and town.availableHeroes or {}
				end
			end
			if key == "getPlayerStatus" then
				return function()
					return input.playerStatus or "INGAME"
				end
			end
			if key == "getObj" then
				return function(_, id)
					return input.objectsByID and input.objectsByID[objectID(id)] or nil
				end
			end
			if key == "getHero" then
				return function(_, id)
					return input.heroesByID and input.heroesByID[objectID(id)] or nil
				end
			end
			if key == "getResourceAmount" then
				return function()
					return input.freeResources or input.resources or {}
				end
			end
			return nil
		end
	})
end

local function makeEvaluator(aiNk)
	return {
		evaluate = function(task, priorityTier)
			return PriorityEvaluator.evaluate(task, priorityTier, aiNk)
		end
	}
end

local function buildAiState(input, host, settings, state)
	input = ensureSnapshotIndexes(input)
	local aiNk = {}
	for key, value in pairs(input or {}) do
		aiNk[key] = value
	end

	aiNk.host = host
	aiNk.settings = settings
	aiNk.cc = makeCallbackFacade(input or {})
	aiNk.lockedHeroes = state.lockedHeroes
	aiNk.lockedResources = state.lockedResources
	aiNk.scanDepth = state.scanDepth
	aiNk.useHeroChain = state.useHeroChain
	aiNk.openMap = state.openMap
	aiNk.useObjectGraph = state.useObjectGraph
	aiNk.pathfinderInvalidated = state.pathfinderInvalidated
	aiNk.decomposer = DeepDecomposer.new(aiNk)

	function aiNk:getFreeResources()
		return State.getFreeResources(self, self.freeResources or self.resources or call(self.cc, "getResourceAmount") or {})
	end

	function aiNk:getLockedResources()
		return self.lockedResources
	end

	function aiNk:lockResources(resources)
		return State.lockResources(self, resources)
	end

	function aiNk:lockHero(hero, lockReason)
		return State.lockHero(self, objectID(hero), lockReason)
	end

	function aiNk:unlockHero(hero)
		return State.unlockHero(self, objectID(hero))
	end

	function aiNk:getHeroLockedReason(hero)
		return State.getHeroLockedReason(self, objectID(hero))
	end

	function aiNk:isHeroLocked(hero)
		return self:getHeroLockedReason(hero) ~= State.HeroLockedReason.NOT_LOCKED
	end

	function aiNk:arePathHeroesLocked(path)
		if self:getHeroLockedReason(path and path.targetHero) == State.HeroLockedReason.STARTUP then
			return true
		end
		for _, node in ipairs(path and path.nodes or {}) do
			if self:getHeroLockedReason(node.targetHero) ~= State.HeroLockedReason.NOT_LOCKED then
				return true
			end
		end
		return false
	end

	function aiNk:setActive(hero, tile)
		self.activeHero = hero
		self.targetTile = tile
	end

	function aiNk:getActiveHero()
		return self.activeHero
	end

	function aiNk:getTargetTile()
		return self.targetTile
	end

	function aiNk:setTargetObject(objid)
		self.targetObject = objid
	end

	function aiNk:getTargetObject()
		return self.targetObject
	end

	function aiNk:invalidatePathfinderData()
		self.pathfinderInvalidated = true
	end

	host.nullkiller = aiNk
	return aiNk
end

local function updateState(aiNk, passIndex)
	if type(aiNk.updateState) == "function" then
		return aiNk:updateState(passIndex)
	end

	call(aiNk.buildAnalyzer, "update")
	call(aiNk.heroManager, "update")
	call(aiNk.armyManager, "update")
	if aiNk.decomposer then
		aiNk.decomposer:reset()
	end
end

local function decompose(results, behavior, depthLimit, aiNk)
	trace(aiNk.host, "Nullkiller.decompose", {
		behavior = behavior:toString(),
		depthLimit = depthLimit
	})
	aiNk.decomposer:decompose(results, behavior, depthLimit)
end

local function affectedObjectsPresent(task, aiNk)
	if not task.getAffectedObjects then
		return true
	end

	for _, id in ipairs(task:getAffectedObjects()) do
		if getObj(aiNk, id) == nil and getHero(aiNk, id) == nil then
			return false
		end
	end
	return true
end

local function taskRole(task, aiNk)
	local hero = task.getHero and task:getHero() or nil
	local role = call(aiNk.heroManager, "getHeroRoleOrDefault", hero)
		or call(aiNk.heroManager, "getHeroRoleOrDefaultInefficient", hero)
		or hero and hero.role
		or PriorityEvaluator.HeroRole.MAIN
	return role
end

local function taskHeroes(task, aiNk)
	local result = {}
	local seen = {}
	local function add(hero)
		local id = objectID(hero)
		if hero and id and not seen[id] then
			seen[id] = true
			table.insert(result, hero)
		end
	end

	if task.getHero then
		add(task:getHero())
	end
	if task.getAffectedObjects then
		for _, id in ipairs(task:getAffectedObjects()) do
			add(getHero(aiNk, id))
		end
	end
	return result
end

local function lockTaskHeroes(task, lockReason, aiNk)
	for _, hero in ipairs(taskHeroes(task, aiNk)) do
		if hero.owner == nil or hero.owner == aiNk.playerID then
			aiNk:lockHero(hero, lockReason)
		end
	end
end

local function hasUnlockedHeroWithMovement(aiNk)
	for _, hero in ipairs(heroesInfo(aiNk)) do
		if not isGarrisoned(hero) and not aiNk:isHeroLocked(hero) and movementPoints(hero) > 100 then
			return true
		end
	end
	return false
end

local function executeTask(task, host, aiNk)
	local ok, result = pcall(function()
		return task:accept(host)
	end)

	if not ok then
		aiNk.pathfinderInvalidated = true
		trace(host, "Nullkiller.executeTask.failed", {
			task = task:toString(),
			error = tostring(result)
		})
		return false
	end

	trace(host, "Nullkiller.executeTask.completed", {
		task = task:toString()
	})
	return true
end

local function townThreats(aiNk, town)
	local threats = call(aiNk.dangerHitMap, "getTownThreats", town) or town.threats or {}
	local result = {}
	for _, threat in ipairs(threats) do
		table.insert(result, threat)
	end
	local objectThreat = call(aiNk.dangerHitMap, "getObjectThreat", town) or town.threatNode
	if objectThreat and objectThreat.fastestDanger then
		table.insert(result, objectThreat.fastestDanger)
	end
	return result
end

local function armyStrength(hero)
	return call(hero, "getTotalStrength") or hero and (hero.totalStrength or hero.armyStrength or hero.strength) or 0
end

local function garrisonHero(town)
	return call(town, "getGarrisonHero") or town.garrisonHero
end

local function visitingHero(town)
	return call(town, "getVisitingHero") or town.visitingHero
end

local function reserveRequiredTownDefenders(aiNk)
	for _, town in ipairs(townsInfo(aiNk)) do
		local threats = townThreats(aiNk, town)
		local bestHero = nil
		local bestCoveredThreats = 0
		local bestStrength = 0

		for _, hero in ipairs({ garrisonHero(town), visitingHero(town) }) do
			if hero and DefenceBehavior.shouldReserveTownDefender(town, hero, threats, aiNk.settings.safeAttackRatio or 1.1) then
				local coveredThreats = DefenceBehavior.countTownThreatsCoveredByDefender(town, hero, threats, aiNk.settings.safeAttackRatio or 1.1)
				local strength = armyStrength(hero)
				if coveredThreats > bestCoveredThreats or (coveredThreats == bestCoveredThreats and strength > bestStrength) then
					bestHero = hero
					bestCoveredThreats = coveredThreats
					bestStrength = strength
				end
			end
		end

		if bestHero and not aiNk:isHeroLocked(bestHero) then
			aiNk:lockHero(bestHero, State.HeroLockedReason.DEFENCE)
		end
	end

	for _, town in ipairs(townsInfo(aiNk)) do
		local hero = garrisonHero(town)
		if hero and not aiNk:isHeroLocked(hero) then
			aiNk:lockHero(hero, State.HeroLockedReason.DEFENCE)
		end
	end
end

local function pickBestArtifacts(aiNk, host)
	local commandCount = 0
	for _, hero in ipairs(heroesInfo(aiNk)) do
		commandCount = commandCount + GatewayPolicy.pickBestArtifacts(host, hero)
	end
	if commandCount > 0 then
		trace(host, "Nullkiller.pickBestArtifacts", {
			commandCount = commandCount
		})
	end
	return commandCount
end

local function runPriorityPass(aiNk, passIndex)
	local evaluator = makeEvaluator(aiNk)
	local results = {}

	updateState(aiNk, passIndex)
	for index = 1, maxPriorityPass(aiNk.settings) do
		results = {}
		decompose(results, RecruitHeroBehavior.new(), 1, aiNk)
		decompose(results, BuyArmyBehavior.new(), 1, aiNk)
		decompose(results, BuildingBehavior.new(), 1, aiNk)

		local bestTask = TaskPlan.choseBestTask(results, evaluator)
		if bestTask.priority <= 0 then
			break
		end

		trace(aiNk.host, "Nullkiller.priorityPass.task", {
			passIndex = passIndex,
			priorityPass = index,
			task = bestTask:toString(),
			priority = bestTask.priority
		})

		local isRecruitHeroGoal = bestTask.goalType == RecruitHero.new().goalType
		if (not isRecruitHeroGoal) and bestTask.getHero and bestTask:getHero() and bestTask:getHero().verified == false then
			break
		end

		if not executeTask(bestTask, aiNk.host, aiNk) then
			break
		end

		updateState(aiNk, passIndex)
	end
	return true
end

local function regularBehaviors(aiNk)
	local behaviors = {
		{ goal = CaptureObjectsBehavior.new(), depth = 1 },
		{ goal = ClusterBehavior.new(), depth = MAX_DEPTH },
		{ goal = DefenceBehavior.new(), depth = MAX_DEPTH },
		{ goal = EscapeBehavior.new(), depth = 1 },
		{ goal = GatherArmyBehavior.new(), depth = MAX_DEPTH }
	}

	if not aiNk.openMap then
		table.insert(behaviors, { goal = ExplorationBehavior.new(), depth = MAX_DEPTH })
	end

	return behaviors
end

local function buildRegularTasks(aiNk)
	local tasks = {}
	for _, behavior in ipairs(regularBehaviors(aiNk)) do
		decompose(tasks, behavior.goal, behavior.depth, aiNk)
	end
	return tasks
end

local function selectTasksByPriorityTier(tasks, aiNk)
	local selectedTasks = {}
	local selectedTier = PriorityEvaluator.PriorityTier.INSTAKILL
	local evaluator = makeEvaluator(aiNk)

	for tier = PriorityEvaluator.PriorityTier.INSTAKILL, PriorityEvaluator.PriorityTier.MAX_PRIORITY_TIER do
		selectedTier = tier
		selectedTasks = TaskPlan.buildPlanAndFilter(tasks, tier, evaluator)
		if #selectedTasks > 0 then
			break
		end
	end

	table.sort(selectedTasks, function(lhs, rhs)
		return lhs.priority > rhs.priority
	end)

	return selectedTasks, selectedTier
end

function Nullkiller.chooseTaskFailureAction(hasAnySuccess, hasRemainingTasks, canReplan)
	return TaskPlan.chooseTaskFailureAction(hasAnySuccess, hasRemainingTasks, canReplan)
end

function Nullkiller.updateStateAndExecutePriorityPass(ai, state, settings, passIndex)
	trace(ai, "Nullkiller.updateStateAndExecutePriorityPass", {
		passIndex = passIndex,
		maxPriorityPass = settings.maxPriorityPass
	})

	return true
end

function Nullkiller.makeTurn(ai, input)
	input = input or {}
	local host = HostCommands.new(ai)
	local settings = loadSettings(input)
	local state = State.new()
	local executedTasks = {}

	State.resetState(state)
	local aiNk = buildAiState(input, host, settings, state)
	trace(host, "Nullkiller.makeTurn.start", {
		maxPass = settings.maxPass,
		maxPriorityPass = settings.maxPriorityPass
	})

	for passIndex = 1, maxPass(settings) do
		if not playerInGame(aiNk) then
			break
		end

		if not runPriorityPass(aiNk, passIndex) then
			break
		end

		reserveRequiredTownDefenders(aiNk)
		local tasks = buildRegularTasks(aiNk)
		local selectedTasks, priorityTier = selectTasksByPriorityTier(tasks, aiNk)
		local hasAnySuccess = false

		for selectedTaskIndex, selectedTask in ipairs(selectedTasks) do
			if not playerInGame(aiNk) then
				break
			end

			if affectedObjectsPresent(selectedTask, aiNk) then
				local heroRole = taskRole(selectedTask, aiNk)
				if heroRole ~= PriorityEvaluator.HeroRole.MAIN or selectedTask:getHeroExchangeCount() <= 1 then
					aiNk.useHeroChain = false
				end

				if selectedTask.priority <= 0 then
					if hasUnlockedHeroWithMovement(aiNk) and aiNk.scanDepth ~= State.ScanDepth.ALL_FULL then
						aiNk.scanDepth = State.ScanDepth.ALL_FULL
						aiNk.useHeroChain = false
						hasAnySuccess = true
						break
					end
				elseif executeTask(selectedTask, host, aiNk) then
					hasAnySuccess = true
					table.insert(executedTasks, selectedTask:toString())
				else
					lockTaskHeroes(selectedTask, State.HeroLockedReason.HERO_CHAIN, aiNk)
					local hasRemainingTasks = selectedTaskIndex + 1 < #selectedTasks
					local failureAction = TaskPlan.chooseTaskFailureAction(hasAnySuccess, hasRemainingTasks, hasUnlockedHeroWithMovement(aiNk))
					if failureAction == TaskPlan.TaskFailureAction.TRY_NEXT_TASK then
						-- Continue with the next selected task.
					elseif failureAction == TaskPlan.TaskFailureAction.REPLAN then
						hasAnySuccess = true
						break
					else
						local stopResult = endTurn(host)
						trace(host, "Nullkiller.makeTurn.end", {
							status = "end_turn",
							actionResult = stopResult
						})
						return {
							status = "end_turn",
							intent = "lua-nullkiller2 stopped after task failure",
							memory = input.memory or {},
							commandJournal = host:getJournal(),
							executedTasks = executedTasks,
							priorityTier = priorityTier,
							trace = {
								implemented = "turn_loop"
							}
						}
					end
				end
			end
		end

		hasAnySuccess = ResourceTrader.trade(aiNk.buildAnalyzer, aiNk.cc, aiNk:getFreeResources()) or hasAnySuccess
		if not hasAnySuccess then
			break
		end

		pickBestArtifacts(aiNk, host)
	end

	local actionResult = endTurn(host)

	trace(host, "Nullkiller.makeTurn.end", {
		status = "end_turn",
		actionResult = actionResult
	})

	return {
		status = "end_turn",
		intent = "lua-nullkiller2 turn loop completed without native fallback",
		memory = input.memory or {},
		commandJournal = host:getJournal(),
		executedTasks = executedTasks,
		trace = {
			implemented = "turn_loop"
		}
	}
end

function Nullkiller.heroExchangeStarted(ai, input)
	input = input or {}
	local host = HostCommands.new(ai)
	local firstHero = input.firstHero or input.hero1
	local secondHero = input.secondHero or input.hero2
	local activeHeroIDValue = input.activeHeroID or input.activeHero or objectID(input.activeHeroObject)

	if firstHero and secondHero and sameOwner(firstHero, secondHero) then
		local destination = firstHero
		local source = secondHero
		if activeHeroIDValue ~= nil and objectID(firstHero) == activeHeroIDValue then
			destination = secondHero
			source = firstHero
		end

		ExchangeSwapTownHeroes.moveCreaturesToHero(host, {
			id = objectID(source),
			owner = source.owner,
			tempOwner = source.tempOwner,
			upperArmy = source,
			bestArmy = input.bestArmy,
			settings = input.settings and input.settings.values or input.settings
		}, destination)
		GatewayPolicy.pickBestArtifacts(host, destination, source)
	end

	if input.queryID ~= nil and type(host.answerQuery) == "function" then
		host:answerQuery(input.queryID, 0)
	end

	return {
		status = "answered",
		commandJournal = host:getJournal()
	}
end

function Nullkiller.showMapObjectSelectDialog(ai, input)
	input = input or {}
	local host = HostCommands.new(ai)
	local selection = GatewayPolicy.chooseMapObjectSelection({
		selectedObject = input.selectedObject or input.selectedObjectID or input.targetObject or input.targetObjectID,
		objects = input.objects
	})

	if input.queryID ~= nil and type(host.answerQuery) == "function" then
		host:answerQuery(input.queryID, selection or 0)
	end

	return {
		status = "answered",
		selection = selection or 0,
		commandJournal = host:getJournal()
	}
end

function Nullkiller.showGarrisonDialog(ai, input)
	input = input or {}
	local host = HostCommands.new(ai)
	local settings = input.settings and input.settings.values or input.settings
	local up = input.up or input.upperArmy
	local down = input.down or input.hero
	local moved = false

	if GatewayPolicy.shouldUseGarrisonTroops({
		up = up,
		down = down,
		removableUnits = input.removableUnits,
		settings = settings,
		restrictedGarrisonsForAI = input.restrictedGarrisonsForAI
	}) then
		moved = ExchangeSwapTownHeroes.moveCreaturesToHero(host, {
			id = objectID(up),
			owner = up and up.owner,
			tempOwner = up and up.tempOwner,
			upperArmy = up,
			bestArmy = input.bestArmy,
			settings = settings
		}, down)
	end

	if input.queryID ~= nil and type(host.answerQuery) == "function" then
		host:answerQuery(input.queryID, 0)
	end

	return {
		status = "answered",
		moved = moved,
		commandJournal = host:getJournal()
	}
end

function Nullkiller.makeSurrenderRetreatDecision(ai, input)
	input = input or {}
	local decision = GatewayPolicy.makeSurrenderRetreatDecision({
		townsCount = input.townsCount,
		settings = input.settings and input.settings.values or input.settings,
		battleState = input.battleState or input
	})

	if not decision then
		return {
			status = "none"
		}
	end

	return {
		status = decision.action,
		side = decision.side
	}
end

function Nullkiller.showBlockingDialog(ai, input)
	input = input or {}
	local host = HostCommands.new(ai)
	local selection = 0

	if input.selection == false and input.cancel == true then
		selection = GatewayPolicy.chooseBlockingDialogAnswer(input)
	else
		selection = GatewayPolicy.chooseBlockingDialogSelection(input)
	end

	if input.queryID ~= nil and type(host.answerQuery) == "function" then
		host:answerQuery(input.queryID, selection)
	end

	return {
		status = "answered",
		selection = selection,
		commandJournal = host:getJournal()
	}
end

return Nullkiller
