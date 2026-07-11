/*
 * LuaAdventureScriptRunner.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "LuaAdventureScriptRunner.h"

#include "LuaStack.h"

#include "../lib/ScopeGuard.h"
#include "../lib/logging/CLogger.h"

#include <cstring>

VCMI_LIB_NAMESPACE_BEGIN

namespace scripting
{
namespace
{

#ifndef LUA_OK
#define LUA_OK 0
#endif

constexpr const char * IMPERATIVE_API_PRELUDE = R"lua(
local input = ...

local function copyFields(source)
	local result = {}
	if type(source) == "table" then
		for key, value in pairs(source) do
			result[key] = value
		end
	end
	return result
end

local function hostError(response)
	if type(response) == "table" and response.error ~= nil then
		return tostring(response.error)
	end
	return "Lua adventure host rejected command"
end

local ai = {
	input = input or {},
	memoryState = (input and input.memory) or { version = 1 }
}

function ai:state()
	return self.input.state
end

function ai:pendingQueries()
	local state = self:state() or {}
	local turn = state.turn or {}
	return turn.queries or {}
end

function ai:updates()
	return self.input.updates
end

function ai:opponentUpdates()
	return self.input.opponentUpdates
end

function ai:progress()
	return self.input.progress
end

function ai:actionSpace()
	return self.input.actionSpace
end

function ai:analysis()
	return self.input.analysis
end

function ai:nullkillerAnalysis()
	local analysis = self:analysis() or {}
	return analysis.nullkiller or {}
end

function ai:nullkillerState()
	local nullkiller = self:nullkillerAnalysis()
	return nullkiller.state or {}
end

function ai:nullkillerSettings()
	local nullkiller = self:nullkillerAnalysis()
	return nullkiller.settings or {}
end

function ai:nullkillerEconomy()
	local nullkiller = self:nullkillerAnalysis()
	return nullkiller.economy or {}
end

function ai:nullkillerHeroRecruitment()
	local nullkiller = self:nullkillerAnalysis()
	return nullkiller.heroRecruitment or {}
end

function ai:limits()
	return self.input.limits
end

function ai:memory()
	return self.memoryState
end

function ai:setMemory(memory)
	self.memoryState = memory or { version = 1 }
	return self.memoryState
end

function ai:execute(action)
	if type(action) == "table" and action.type_id == nil and type(action.type) == "string" then
		action.type_id = ai.actionTypeIdsByName[action.type]
	end
	local response = coroutine.yield({ kind = "execute", payload = action })
	if type(response) ~= "table" or not response.ok then
		error(hostError(response), 2)
	end
	return response.result or response
end

local function protectedHostCall(fn, ...)
	local ok, result = pcall(fn, ...)
	if ok then
		return { ok = true, result = result }
	end
	return { ok = false, error = tostring(result) }
end

function ai:tryCall(fn, ...)
	if type(fn) ~= "function" then
		return { ok = false, error = "ai:tryCall expects a function" }
	end
	return protectedHostCall(fn, ...)
end

function ai:tryExecute(action)
	return protectedHostCall(function()
		return self:execute(action)
	end)
end

function ai:runAction(action)
	if type(action) ~= "table" then
		error("ai:runAction expects an action table", 2)
	end
	return self:execute(action)
end

function ai:tryRunAction(action)
	return protectedHostCall(function()
		return self:runAction(action)
	end)
end

function ai:runOption(option, actionField)
	if type(option) ~= "table" then
		error("ai:runOption expects an option table", 2)
	end
	local field = actionField or "planAction"
	local action = option[field]
	if type(action) ~= "table" then
		error("ai:runOption option is missing action field '" .. tostring(field) .. "'", 2)
	end
	return self:execute(action)
end

function ai:tryRunOption(option, actionField)
	return protectedHostCall(function()
		return self:runOption(option, actionField)
	end)
end

function ai:refresh()
	local response = coroutine.yield({ kind = "refresh" })
	if type(response) ~= "table" or not response.ok then
		error(hostError(response), 2)
	end
	self.input = response.input or self.input
	if self.updateActionTypeIdsFromActionSpace then
		self:updateActionTypeIdsFromActionSpace()
	end
	return self.input
end

function ai:tryRefresh()
	local attempt = protectedHostCall(function()
		return self:refresh()
	end)
	if attempt.ok then
		attempt.input = attempt.result
	end
	return attempt
end

function ai:inspect(request)
	if type(request) ~= "table" then
		error("ai:inspect expects a request table", 2)
	end
	local response = coroutine.yield({ kind = "inspect", payload = request })
	if type(response) ~= "table" or not response.ok then
		error(hostError(response), 2)
	end
	return response.result or response
end

function ai:tryInspect(request)
	return protectedHostCall(function()
		return self:inspect(request)
	end)
end

function ai:getState()
	return self:inspect({ what = "state" })
end

function ai:getActionSpace()
	return self:inspect({ what = "action_space" })
end

function ai:getAnalysis()
	return self:inspect({ what = "analysis" })
end

function ai:getQueries()
	return self:inspect({ what = "queries" })
end

function ai:getUpdates(opponentOnly)
	return self:inspect({ what = "updates", opponent_only = opponentOnly == true })
end

function ai:getLimits()
	return self:inspect({ what = "limits" })
end

function ai:getGrail()
	return self:inspect({ what = "grail" })
end

function ai:getObject(objectId, heroId)
	local request = { what = "object", object_id = objectId }
	if heroId ~= nil then
		request.hero_id = heroId
	end
	return self:inspect(request)
end

function ai:getHero(heroId)
	return self:inspect({ what = "hero", hero_id = heroId })
end

function ai:getTown(townId)
	return self:inspect({ what = "town", town_id = townId })
end

function ai:getTile(x, y, z)
	return self:inspect({ what = "tile", x = x, y = y, z = z or 0 })
end

function ai:getObjectsAt(x, y, z)
	return self:inspect({ what = "objects_at", x = x, y = y, z = z or 0 })
end

function ai:getAvailableHeroes(sourceId)
	return self:inspect({ what = "available_heroes", source_id = sourceId })
end

function ai:getPath(heroId, x, y, z)
	return self:inspect({ what = "path", hero_id = heroId, x = x, y = y, z = z })
end

function ai:getPathToObject(heroId, objectId)
	return self:inspect({ what = "path", hero_id = heroId, object_id = objectId })
end

function ai:getReachable(heroId, options)
	local request = { what = "reachable", hero_id = heroId }
	if type(options) == "table" then
		request.radius = options.radius
		request.max_movement_options = options.max_movement_options or options.maxMovementOptions
		request.max_object_targets = options.max_object_targets or options.maxObjectTargets
	end
	return self:inspect(request)
end

function ai:getDanger(heroId, target, y, z, options)
	local request = { what = "danger", hero_id = heroId }
	if type(target) == "table" then
		for key, value in pairs(target) do
			request[key] = value
		end
		options = y
	else
		request.x = target
		request.y = y
		request.z = z
	end
	if type(options) == "table" then
		if options.check_guards ~= nil then
			request.check_guards = options.check_guards
		elseif options.checkGuards ~= nil then
			request.check_guards = options.checkGuards
		end
	end
	return self:inspect(request)
end

function ai:getTileDanger(heroId, x, y, z, options)
	if type(z) == "table" and options == nil then
		options = z
		z = nil
	end
	return self:getDanger(heroId, { x = x, y = y, z = z }, options)
end

function ai:getObjectDanger(heroId, objectId, options)
	local request = { object_id = objectId }
	if type(options) == "table" then
		request.check_guards = options.check_guards
		if request.check_guards == nil then
			request.check_guards = options.checkGuards
		end
	end
	return self:getDanger(heroId, request)
end

function ai:getNullkillerTaskCandidates(mode, maxCandidates)
	local request = copyFields(mode)
	if type(mode) ~= "table" then
		request.mode = mode or "all"
		request.max_candidates = maxCandidates
	end
	request.what = "nullkiller_tasks"
	return self:inspect(request)
end

ai.getNullkillerTasks = ai.getNullkillerTaskCandidates
ai.nullkillerTaskCandidates = ai.getNullkillerTaskCandidates
ai.nullkillerCandidates = ai.getNullkillerTaskCandidates

function ai:delegateToNullkiller(intent)
	return coroutine.yield({
		kind = "fallback",
		memory = self.memoryState,
		intent = intent or "script delegated to Nullkiller"
	})
end

ai.nullkiller = ai.delegateToNullkiller
ai.nullkillerForRestOfDay = ai.delegateToNullkiller

ai.marketModes = {
	resourceResource = 0,
	resourcePlayer = 1,
	creatureResource = 2,
	resourceArtifact = 3,
	artifactResource = 4,
	artifactExperience = 5,
	creatureExperience = 6,
	creatureUndead = 7,
	resourceSkill = 8
}

ai.marketTradeKinds = {
	resourceResource = 1,
	resourceSkill = 2
}

ai.artifactSlots = {
	transition = -3,
	firstAvailable = -2,
	altar = 19,
	backpackStart = 19
}

ai.artifactManagementKinds = {
	unknown = 0,
	sortBackpack = 1,
	scrollBackpack = 2,
	loadCostume = 3,
	saveCostume = 4,
	assemble = 5,
	disassemble = 6,
	eraseTransition = 7,
	bulkMoveToHero = 8,
	bulkSwapWithHero = 9,
	moveSingleToHero = 10
}

ai.backpackSortModes = {
	slot = 1,
	cost = 2,
	class = 3
}

ai.formations = {
	loose = 0,
	tight = 1
}

ai.componentTypes = {
	none = -1,
	primarySkill = 0,
	secondarySkill = 1,
	resource = 2,
	resourcePerDay = 3,
	creature = 4,
	artifact = 5,
	spellScroll = 6,
	mana = 7,
	experience = 8,
	level = 9,
	spell = 10,
	morale = 11,
	luck = 12,
	building = 13,
	heroPortrait = 14,
	flag = 15
}

ai.resourceIds = {
	wood = 0,
	mercury = 1,
	ore = 2,
	sulfur = 3,
	crystal = 4,
	gems = 5,
	gold = 6
}

ai.buildingKinds = {
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

ai.objectKinds = {
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

ai.armyTransferKinds = {
	unknown = 0,
	gatherToHero = 1,
	reinforceTown = 2
}

ai.queryTypes = {
	unknown = 0,
	heroLevelUp = 1,
	commanderLevelUp = 2,
	blockingDialog = 3,
	teleportDialog = 4,
	mapObjectSelect = 5,
	tavernWindow = 6,
	heroExchange = 7,
	garrisonDialog = 8,
	recruitmentDialog = 9,
	universityWindow = 10,
	marketWindow = 11,
	artifactAssemblyPrompt = 12
}

ai.actionTypeIds = {
	build = 1,
	recruit = 2,
	hireHero = 3,
	transferArmy = 4,
	moveHero = 5,
	visitObject = 6,
	answerQuery = 7,
	cancelQuery = 8,
	endTurn = 9,
	pickBestCreatures = 20,
	pickBestArtifacts = 21,
	prepareHero = 22,
	swapArtifacts = 23,
	bulkMoveArtifacts = 24,
	sortBackpackArtifacts = 25,
	scrollBackpackArtifacts = 26,
	manageHeroCostume = 27,
	assembleArtifacts = 28,
	ignoreScriptQuery = 29,
	eraseTransitionArtifact = 30,
	swapCreatures = 40,
	mergeStacks = 41,
	mergeOrSwapStacks = 42,
	splitStack = 43,
	bulkMoveArmy = 44,
	bulkSplitStack = 45,
	bulkMergeStacks = 46,
	bulkSplitRebalanceStack = 47,
	dismissCreature = 48,
	upgradeCreature = 49,
	setFormation = 50,
	setTactics = 51,
	setTownName = 52,
	swapGarrisonHero = 53,
	tradeResources = 60,
	marketTrade = 61,
	requestStatistic = 62,
	dismissHero = 63,
	buildBoat = 64,
	castleTeleport = 65,
	dig = 66,
	castSpell = 67,
	buyArtifact = 68,
	spellResearch = 69,
	visitTownBuilding = 70,
	nullkillerReset = 100,
	nullkillerLockResources = 101,
	nullkillerLockHero = 102,
	nullkillerUnlockHero = 103,
	nullkillerTrade = 104,
	nullkillerPriorityPass = 105,
	nullkillerTurnSlice = 106,
	nullkillerBuildArmy = 107,
	nullkillerUpgradeArmy = 108,
	nullkillerRecruitCreatures = 109,
	nullkillerMoveCreaturesToHero = 110,
	nullkillerDismissWeakHero = 111,
	nullkillerOptimizeArtifacts = 112,
	nullkillerAddSingleCreatureStacks = 113,
	nullkillerRearrangeForWhirlpool = 114,
	nullkillerRearrangeForSiege = 115,
	nullkillerTasks = 116,
	nullkillerTask = 117,
	nullkillerStep = 118,
	nullkillerPass = 119,
	nullkillerAnswerQuery = 120,
	nullkillerObjectInteraction = 121,
	nullkillerDefendTown = 122
}

ai.actionTypeIdsByName = {
	build = ai.actionTypeIds.build,
	recruit = ai.actionTypeIds.recruit,
	hire_hero = ai.actionTypeIds.hireHero,
	transfer_army = ai.actionTypeIds.transferArmy,
	move_hero = ai.actionTypeIds.moveHero,
	visit_object = ai.actionTypeIds.visitObject,
	answer_query = ai.actionTypeIds.answerQuery,
	cancel_query = ai.actionTypeIds.cancelQuery,
	end_turn = ai.actionTypeIds.endTurn,
	pick_best_creatures = ai.actionTypeIds.pickBestCreatures,
	pick_best_artifacts = ai.actionTypeIds.pickBestArtifacts,
	prepare_hero = ai.actionTypeIds.prepareHero,
	swap_artifacts = ai.actionTypeIds.swapArtifacts,
	bulk_move_artifacts = ai.actionTypeIds.bulkMoveArtifacts,
	sort_backpack_artifacts = ai.actionTypeIds.sortBackpackArtifacts,
	scroll_backpack_artifacts = ai.actionTypeIds.scrollBackpackArtifacts,
	manage_hero_costume = ai.actionTypeIds.manageHeroCostume,
	assemble_artifacts = ai.actionTypeIds.assembleArtifacts,
	ignore_script_query = ai.actionTypeIds.ignoreScriptQuery,
	erase_transition_artifact = ai.actionTypeIds.eraseTransitionArtifact,
	swap_creatures = ai.actionTypeIds.swapCreatures,
	merge_stacks = ai.actionTypeIds.mergeStacks,
	merge_or_swap_stacks = ai.actionTypeIds.mergeOrSwapStacks,
	split_stack = ai.actionTypeIds.splitStack,
	bulk_move_army = ai.actionTypeIds.bulkMoveArmy,
	bulk_split_stack = ai.actionTypeIds.bulkSplitStack,
	bulk_merge_stacks = ai.actionTypeIds.bulkMergeStacks,
	bulk_split_rebalance_stack = ai.actionTypeIds.bulkSplitRebalanceStack,
	dismiss_creature = ai.actionTypeIds.dismissCreature,
	upgrade_creature = ai.actionTypeIds.upgradeCreature,
	set_formation = ai.actionTypeIds.setFormation,
	set_tactics = ai.actionTypeIds.setTactics,
	set_town_name = ai.actionTypeIds.setTownName,
	swap_garrison_hero = ai.actionTypeIds.swapGarrisonHero,
	trade_resources = ai.actionTypeIds.tradeResources,
	market_trade = ai.actionTypeIds.marketTrade,
	request_statistic = ai.actionTypeIds.requestStatistic,
	dismiss_hero = ai.actionTypeIds.dismissHero,
	build_boat = ai.actionTypeIds.buildBoat,
	castle_teleport = ai.actionTypeIds.castleTeleport,
	dig = ai.actionTypeIds.dig,
	cast_spell = ai.actionTypeIds.castSpell,
	buy_artifact = ai.actionTypeIds.buyArtifact,
	spell_research = ai.actionTypeIds.spellResearch,
	visit_town_building = ai.actionTypeIds.visitTownBuilding,
	nullkiller_reset = ai.actionTypeIds.nullkillerReset,
	nullkiller_lock_resources = ai.actionTypeIds.nullkillerLockResources,
	nullkiller_lock_hero = ai.actionTypeIds.nullkillerLockHero,
	nullkiller_unlock_hero = ai.actionTypeIds.nullkillerUnlockHero,
	nullkiller_trade = ai.actionTypeIds.nullkillerTrade,
	nullkiller_priority_pass = ai.actionTypeIds.nullkillerPriorityPass,
	nullkiller_turn_slice = ai.actionTypeIds.nullkillerTurnSlice,
	nullkiller_build_army = ai.actionTypeIds.nullkillerBuildArmy,
	nullkiller_upgrade_army = ai.actionTypeIds.nullkillerUpgradeArmy,
	nullkiller_recruit_creatures = ai.actionTypeIds.nullkillerRecruitCreatures,
	nullkiller_move_creatures_to_hero = ai.actionTypeIds.nullkillerMoveCreaturesToHero,
	nullkiller_dismiss_weak_hero = ai.actionTypeIds.nullkillerDismissWeakHero,
	nullkiller_optimize_artifacts = ai.actionTypeIds.nullkillerOptimizeArtifacts,
	nullkiller_add_single_creature_stacks = ai.actionTypeIds.nullkillerAddSingleCreatureStacks,
	nullkiller_rearrange_for_whirlpool = ai.actionTypeIds.nullkillerRearrangeForWhirlpool,
	nullkiller_rearrange_for_siege = ai.actionTypeIds.nullkillerRearrangeForSiege,
	nullkiller_tasks = ai.actionTypeIds.nullkillerTasks,
	nullkiller_task = ai.actionTypeIds.nullkillerTask,
	nullkiller_step = ai.actionTypeIds.nullkillerStep,
	nullkiller_pass = ai.actionTypeIds.nullkillerPass,
	nullkiller_answer_query = ai.actionTypeIds.nullkillerAnswerQuery,
	nullkiller_object_interaction = ai.actionTypeIds.nullkillerObjectInteraction,
	nullkiller_defend_town = ai.actionTypeIds.nullkillerDefendTown
}

function ai:updateActionTypeIdsFromActionSpace()
	local actionSpace = self:actionSpace() or {}
	for _, action in ipairs(actionSpace.acceptedActions or {}) do
		if type(action) == "table" and type(action.type) == "string" then
			local typeId = action.type_id or action.typeId
			if type(typeId) == "number" then
				self.actionTypeIdsByName[action.type] = typeId
			end
		end
	end
	return self.actionTypeIdsByName
end

ai:updateActionTypeIdsFromActionSpace()

ai.pathActions = {
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

ai.threatLevels = {
	unknown = 0,
	watch = 1,
	high = 2,
	critical = 3
}

ai.riskLevels = {
	none = 0,
	acceptable = 1,
	risky = 2,
	high = 3,
	critical = 4
}

ai.specialActionKinds = {
	unknown = 0,
	composite = 1,
	dimensionDoor = 2,
	townPortal = 3,
	summonBoat = 4,
	buildBoat = 5,
	whirlpool = 6,
	quest = 7,
	adventureCast = 8
}

ai.nullkillerStepOutcomes = {
	failed = 0,
	executed = 1,
	replan = 2,
	stopTurn = 3,
	exhaustedCandidates = 4
}

ai.nullkillerFailureActions = {
	tryNextTask = 0,
	replan = 1,
	stopTurn = 2
}

ai.nullkillerTaskModes = {
	priority = 0,
	adventure = 1,
	all = 2,
	recruitHero = 3,
	buyArmy = 4,
	building = 5,
	capture = 6,
	cluster = 7,
	defense = 8,
	escape = 9,
	gatherArmy = 10,
	exploration = 11,
	startup = 12
}

ai.nullkillerPriorityTiers = {
	buildings = 0,
	instakill = 1,
	instaDefend = 2,
	kill = 3,
	escape = 4,
	exploreAndGather = 5,
	defend = 6
}

ai.nullkillerHeroLockReasons = {
	none = 0,
	startup = 1,
	defense = 2,
	heroChain = 3
}

ai.nullkillerHeroRoles = {
	scout = 0,
	main = 1
}

ai.adventureSpellKinds = {
	unknown = 0,
	generic = 1,
	dimensionDoor = 2,
	townPortal = 3,
	summonBoat = 4,
	removeObject = 5,
	reinforcements = 6,
	viewWorld = 7,
	waterWalk = 8,
	fly = 9
}

function ai:nullkillerReset()
	return self:execute({ type = "nullkiller_reset" })
end

function ai:nullkillerTasks(mode, maxCandidates)
	local action = copyFields(mode)
	if type(mode) ~= "table" then
		action.mode = mode or "all"
		action.max_candidates = maxCandidates
	end
	action.type = "nullkiller_tasks"
	local result = self:execute(action)
	return result.nullkiller or result
end

function ai:runNullkillerTask(taskId)
	local action = copyFields(taskId)
	if type(taskId) ~= "table" then
		action.task_id = taskId
	end
	action.type = "nullkiller_task"
	return self:execute(action)
end

function ai:nullkillerDefendTown(townId, options)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
		if type(options) == "table" then
			for key, value in pairs(options) do
				action[key] = value
			end
		end
	end
	action.type = "nullkiller_defend_town"
	return self:execute(action)
end

function ai:runBestNullkillerTask(mode, selector, maxCandidates)
	local request = copyFields(mode)
	local selectorFn = selector
	if type(mode) ~= "table" then
		request.mode = mode or ai.nullkillerTaskModes.all
		if type(selector) == "number" and maxCandidates == nil then
			request.max_candidates = selector
			selectorFn = nil
		else
			request.max_candidates = maxCandidates
		end
	else
		selectorFn = request.selector or request.predicate or selectorFn
		request.selector = nil
		request.predicate = nil
	end

	local candidates = self:getNullkillerTaskCandidates(request)
	local tasks = candidates.tasks or {}
	local selected
	local selectedIndex = 0
	if type(selectorFn) == "function" then
		for index, task in ipairs(tasks) do
			if selectorFn(task, index, candidates) then
				selected = task
				selectedIndex = index
				break
			end
		end
	else
		selected = tasks[1]
		selectedIndex = selected and 1 or 0
	end

	if type(selected) ~= "table" or selected.task_id == nil then
		return {
			ok = true,
			executed = false,
			selected = false,
			candidateCount = #tasks,
			reason = "no_matching_task",
			nullkiller = candidates
		}
	end

	local result = self:runNullkillerTask(selected.task_id)
	result.selected = true
	result.selectedTask = selected
	result.selectedTaskIndex = selectedIndex
	result.candidateCount = #tasks
	return result
end

ai.runNullkillerCandidate = ai.runBestNullkillerTask
ai.runFirstNullkillerTask = ai.runBestNullkillerTask

function ai:nullkillerStep(mode, maxCandidates, maxAttempts)
	local action = copyFields(mode)
	if type(mode) ~= "table" then
		action.mode = mode or "all"
		action.max_candidates = maxCandidates
		action.max_attempts = maxAttempts
	end
	action.type = "nullkiller_step"
	return self:execute(action)
end

function ai:nullkillerPass(mode, maxSteps, maxCandidates, maxAttempts)
	local action = copyFields(mode)
	if type(mode) ~= "table" then
		action.mode = mode or ai.nullkillerTaskModes.adventure
		action.max_steps = maxSteps
		action.max_candidates = maxCandidates
		action.max_attempts = maxAttempts
	end
	action.type = "nullkiller_pass"
	return self:execute(action)
end

function ai:nullkillerAdventurePass(maxSteps, maxCandidates, maxAttempts)
	return self:nullkillerPass({
		mode = ai.nullkillerTaskModes.adventure,
		max_steps = maxSteps,
		max_candidates = maxCandidates,
		max_attempts = maxAttempts
	})
end

function ai:nullkillerTurnSlice(maxPasses, maxCandidates, maxAttempts)
	local action = copyFields(maxPasses)
	if type(maxPasses) ~= "table" then
		action.max_passes = maxPasses
		action.max_candidates = maxCandidates
		action.max_attempts = maxAttempts
	end
	action.type = "nullkiller_turn_slice"
	return self:execute(action)
end

function ai:nullkillerNativePass(passIndex, maxCandidates, maxAttempts)
	local action = copyFields(passIndex)
	if type(passIndex) ~= "table" then
		action.first_pass_index = passIndex or 1
		action.max_candidates = maxCandidates
		action.max_attempts = maxAttempts
	end
	action.max_passes = action.max_passes or 1
	action.include_priority = action.include_priority ~= false
	action.include_adventure = action.include_adventure ~= false
	action.include_trade = action.include_trade ~= false
	action.optimize_artifacts = action.optimize_artifacts ~= false
	action.type = "nullkiller_turn_slice"
	return self:execute(action)
end

function ai:nullkillerNativePasses(maxPasses, maxCandidates, maxAttempts)
	local action = copyFields(maxPasses)
	if type(maxPasses) ~= "table" then
		action.max_passes = maxPasses
		action.max_candidates = maxCandidates
		action.max_attempts = maxAttempts
	end
	action.max_passes = action.max_passes or 1
	return self:nullkillerNativePass(action)
end

local function numberOr(value, fallback)
	local result = tonumber(value)
	if result == nil then
		return fallback
	end
	return result
end

local function boolOption(config, snakeName, camelName, default)
	if config[snakeName] ~= nil then
		return config[snakeName] == true
	end
	if config[camelName] ~= nil then
		return config[camelName] == true
	end
	return default
end

local function addCounter(target, field, amount)
	target[field] = (tonumber(target[field] or 0) or 0) + (tonumber(amount or 0) or 0)
end

local function nullkillerSliceDidPriorityWork(result)
	return (tonumber(result.priorityTasksExecuted or 0) or 0) > 0
end

local function nullkillerSliceDidAdventureWork(result)
	return (tonumber(result.adventureStepsExecuted or 0) or 0) > 0
		or (tonumber(result.adventureReplanSteps or 0) or 0) > 0
end

local function nullkillerSliceDidTrade(result)
	return (tonumber(result.tradePasses or 0) or 0) > 0
end

local function nullkillerSliceDidWork(result)
	return result.didWork == true
		or nullkillerSliceDidPriorityWork(result)
		or nullkillerSliceDidAdventureWork(result)
		or nullkillerSliceDidTrade(result)
		or result.paused == true
end

local function nullkillerSliceShouldEndTurn(result)
	return result.shouldStopTurn == true
		or (tonumber(result.adventureStopTurnSteps or 0) or 0) > 0
		or (result.exhaustedCandidates == true and not nullkillerSliceDidWork(result))
end

function ai:nullkillerBoundedDay(options)
	local config = copyFields(options)
	local settings = self:nullkillerSettings()
	local maxPasses = math.max(1, math.floor(numberOr(config.max_passes or config.maxPasses, numberOr(settings.maxPass, 1))))
	local firstPassIndex = math.max(1, math.floor(numberOr(config.first_pass_index or config.firstPassIndex, 1)))
	local maxQueriesPerPass = math.max(0, math.floor(numberOr(config.max_queries_per_pass or config.maxQueriesPerPass, 16)))
	local answerQueries = boolOption(config, "answer_queries", "answerQueries", true)
	local refreshBetweenPasses = boolOption(config, "refresh_between_passes", "refreshBetweenPasses", true)

	local summary = {
		ok = true,
		status = "budget_exhausted",
		shouldEndTurn = false,
		exhaustedBudget = true,
		passCount = 0,
		passes = {},
		answeredQueries = 0,
		refreshes = 0,
		didWork = false,
		priorityTasksExecuted = 0,
		adventureStepsExecuted = 0,
		adventureReplanSteps = 0,
		adventureStopTurnSteps = 0,
		adventureExhaustedSteps = 0,
		tradePasses = 0,
		artifactCleanupPasses = 0
	}

	for passOffset = 0, maxPasses - 1 do
		if answerQueries and maxQueriesPerPass > 0 then
			local answered = self:nullkillerAnswerPendingQueries(config.default_answer or config.defaultAnswer, maxQueriesPerPass)
			addCounter(summary, "answeredQueries", answered.count)
			if answered.truncated == true then
				summary.status = "queries_remaining"
				summary.queryLimitReached = true
				summary.exhaustedBudget = false
				return summary
			end
		end

		local passAction = copyFields(config)
		passAction.first_pass_index = firstPassIndex + passOffset
		passAction.max_passes = 1
		passAction.max_candidates = passAction.max_candidates or passAction.maxCandidates
		passAction.max_attempts = passAction.max_attempts or passAction.maxAttempts
		passAction.answer_queries = nil
		passAction.answerQueries = nil
		passAction.default_answer = nil
		passAction.defaultAnswer = nil
		passAction.max_queries_per_pass = nil
		passAction.maxQueriesPerPass = nil
		passAction.refresh_between_passes = nil
		passAction.refreshBetweenPasses = nil

		local pass = self:nullkillerNativePass(passAction)
		summary.passes[#summary.passes + 1] = pass
		summary.passCount = #summary.passes
		addCounter(summary, "priorityTasksExecuted", pass.priorityTasksExecuted)
		addCounter(summary, "adventureStepsExecuted", pass.adventureStepsExecuted)
		addCounter(summary, "adventureReplanSteps", pass.adventureReplanSteps)
		addCounter(summary, "adventureStopTurnSteps", pass.adventureStopTurnSteps)
		addCounter(summary, "adventureExhaustedSteps", pass.adventureExhaustedSteps)
		addCounter(summary, "tradePasses", pass.tradePasses)
		addCounter(summary, "artifactCleanupPasses", pass.artifactCleanupPasses)

		if nullkillerSliceDidWork(pass) then
			summary.didWork = true
		end
		if pass.paused == true then
			summary.status = "paused"
			summary.paused = true
			summary.exhaustedBudget = false
			return summary
		end
		if nullkillerSliceShouldEndTurn(pass) then
			summary.status = "stop_turn"
			summary.shouldEndTurn = true
			summary.exhaustedBudget = false
			return summary
		end
		if not nullkillerSliceDidWork(pass) then
			summary.status = "idle"
			summary.shouldEndTurn = true
			summary.exhaustedBudget = false
			return summary
		end

		if refreshBetweenPasses and passOffset < maxPasses - 1 then
			self:refresh()
			addCounter(summary, "refreshes", 1)
		end
	end

	return summary
end

ai.nullkillerRunDay = ai.nullkillerBoundedDay
ai.nullkillerNativeDay = ai.nullkillerBoundedDay

local function defineNullkillerModeHelpers(name, mode)
	ai["nullkiller" .. name .. "Tasks"] = function(self, maxCandidates)
		return self:nullkillerTasks({
			mode = mode,
			max_candidates = maxCandidates
		})
	end

	ai["nullkiller" .. name .. "Step"] = function(self, maxCandidates, maxAttempts)
		return self:nullkillerStep({
			mode = mode,
			max_candidates = maxCandidates,
			max_attempts = maxAttempts
		})
	end

	ai["nullkiller" .. name .. "Pass"] = function(self, maxSteps, maxCandidates, maxAttempts)
		return self:nullkillerPass({
			mode = mode,
			max_steps = maxSteps,
			max_candidates = maxCandidates,
			max_attempts = maxAttempts
		})
	end
end

defineNullkillerModeHelpers("All", ai.nullkillerTaskModes.all)
defineNullkillerModeHelpers("Priority", ai.nullkillerTaskModes.priority)
defineNullkillerModeHelpers("Adventure", ai.nullkillerTaskModes.adventure)
defineNullkillerModeHelpers("RecruitHero", ai.nullkillerTaskModes.recruitHero)
defineNullkillerModeHelpers("BuyArmy", ai.nullkillerTaskModes.buyArmy)
defineNullkillerModeHelpers("Building", ai.nullkillerTaskModes.building)
defineNullkillerModeHelpers("Capture", ai.nullkillerTaskModes.capture)
defineNullkillerModeHelpers("Cluster", ai.nullkillerTaskModes.cluster)
defineNullkillerModeHelpers("Defense", ai.nullkillerTaskModes.defense)
defineNullkillerModeHelpers("Escape", ai.nullkillerTaskModes.escape)
defineNullkillerModeHelpers("GatherArmy", ai.nullkillerTaskModes.gatherArmy)
defineNullkillerModeHelpers("Exploration", ai.nullkillerTaskModes.exploration)
defineNullkillerModeHelpers("Startup", ai.nullkillerTaskModes.startup)

local function queryTypeId(query)
	if type(query) ~= "table" then
		return nil
	end
	return query.typeId or query.type_id
end

function ai:queryHasType(query, typeId)
	local actual = queryTypeId(query)
	return actual ~= nil and typeId ~= nil and tonumber(actual) == tonumber(typeId)
end

local function optionAnswer(option)
	if type(option) == "table" and option.answer ~= nil then
		return option.answer
	end
	return nil
end

local function firstOptionAnswer(options)
	for _, option in ipairs(options or {}) do
		local answer = optionAnswer(option)
		if answer ~= nil then
			return answer
		end
	end
	return nil
end

local function preferenceMatchesComponent(component, preference)
	if type(component) ~= "table" then
		return false
	end

	if type(preference) == "number" then
		return tonumber(component.typeId or component.type_id) == tonumber(preference)
	end

	if type(preference) ~= "table" then
		return false
	end

	local expectedType = preference.type_id or preference.typeId or preference[1]
	local expectedSubtype = preference.subtype_id or preference.subtypeId or preference[2]
	local minValue = preference.min_value or preference.minValue
	local maxValue = preference.max_value or preference.maxValue

	if expectedType ~= nil and tonumber(component.typeId or component.type_id) ~= tonumber(expectedType) then
		return false
	end
	if expectedSubtype ~= nil and tonumber(component.subtypeId or component.subtype_id) ~= tonumber(expectedSubtype) then
		return false
	end
	if minValue ~= nil and (tonumber(component.value or 0) or 0) < tonumber(minValue) then
		return false
	end
	if maxValue ~= nil and (tonumber(component.value or 0) or 0) > tonumber(maxValue) then
		return false
	end

	return true
end

local function skillPriority(skillId, preferredSkillIds)
	if skillId == nil or type(preferredSkillIds) ~= "table" then
		return nil
	end
	for index, preferred in ipairs(preferredSkillIds) do
		if tonumber(preferred) == tonumber(skillId) then
			return index
		end
	end
	return nil
end

local function optionFieldNumber(option, ...)
	for _, field in ipairs({ ... }) do
		local value = option[field]
		if value ~= nil then
			return tonumber(value)
		end
	end
	return nil
end

local function optionHasPlanAction(option)
	return type(option) == "table" and type(option.planAction) == "table"
end

local function queryOptionAllowed(option, config)
	if not optionHasPlanAction(option) then
		return false
	end
	if config.allow_unaffordable == true or config.allowUnaffordable == true then
		return true
	end
	if option.buyable == false then
		return false
	end
	if option.affordable == false then
		return false
	end
	if option.canLearn == false then
		return false
	end
	if option.amount ~= nil and (tonumber(option.amount) or 0) <= 0 then
		return false
	end
	if option.affordable ~= nil and type(option.affordable) ~= "boolean" and (tonumber(option.affordable) or 0) <= 0 then
		return false
	end
	return true
end

local function preferredIdBonus(value, preferredIds)
	if value == nil or type(preferredIds) ~= "table" then
		return 0
	end
	for index, preferred in ipairs(preferredIds) do
		if tonumber(preferred) == tonumber(value) then
			return 100000 - index
		end
	end
	return 0
end

local function scoreQueryPlanOption(option, config)
	local score = 0
	score = score + preferredIdBonus(option.skill_id or option.skillId, config.preferred_skill_ids or config.preferredSkillIds)
	score = score + preferredIdBonus(option.creature_id or option.creatureId, config.preferred_creature_ids or config.preferredCreatureIds)
	score = score + preferredIdBonus(option.hero_type_id or option.heroTypeId, config.preferred_hero_type_ids or config.preferredHeroTypeIds)
	score = score + (tonumber(option.nullkillerSkillScore or option.nullkiller_skill_score or 0) or 0)
	score = score + ((optionFieldNumber(option, "totalStrength", "heroStrength", "armyStrength") or 0) / 1000)
	score = score + (optionFieldNumber(option, "amount", "available") or 0)
	score = score - ((optionFieldNumber(option, "goldCost") or 0) / 100000)
	return score
end

local function selectQueryPlanOption(options, config)
	local best
	local bestScore
	for _, option in ipairs(options or {}) do
		if queryOptionAllowed(option, config) then
			local score = scoreQueryPlanOption(option, config)
			if best == nil or score > bestScore then
				best = option
				bestScore = score
			end
		end
	end
	return best
end

function ai:defaultQueryDecision(query, policy)
	local config = copyFields(policy)
	if type(query) ~= "table" then
		return nil
	end

	local typeId = queryTypeId(query)
	if typeId == self.queryTypes.artifactAssemblyPrompt then
		return { ignore = true }
	end

	if config.use_plan_actions == false or config.usePlanActions == false then
		return nil
	end

	if typeId == self.queryTypes.marketWindow or typeId == self.queryTypes.universityWindow then
		local option = selectQueryPlanOption(query.skillOptions or query.skill_options, config)
		if option then
			return { planAction = option.planAction, selectedOption = option }
		end
	end

	if typeId == self.queryTypes.recruitmentDialog then
		local option = selectQueryPlanOption(query.recruitOptions or query.recruit_options, config)
		if option then
			return { planAction = option.planAction, selectedOption = option }
		end
	end

	if typeId == self.queryTypes.tavernWindow then
		local option = selectQueryPlanOption(query.hireHeroOptions or query.hire_hero_options, config)
		if option then
			return { planAction = option.planAction, selectedOption = option }
		end
	end

	return nil
end

function ai:defaultQueryAnswer(query, policy)
	local config = copyFields(policy)
	local defaultAnswer = config.default_answer
	if defaultAnswer == nil then
		defaultAnswer = config.defaultAnswer
	end
	if type(query) ~= "table" then
		return defaultAnswer or 0
	end

	local typeId = queryTypeId(query)
	if typeId == self.queryTypes.artifactAssemblyPrompt then
		return nil
	end

	if typeId == self.queryTypes.heroLevelUp or typeId == self.queryTypes.commanderLevelUp then
		local options = query.skill_options or query.skills or {}
		local preferredSkillIds = config.preferred_skill_ids or config.preferredSkillIds
		local best
		local bestPriority
		local bestScore
		for _, option in ipairs(options) do
			local answer = optionAnswer(option)
			if answer ~= nil and best == nil then
				best = answer
			end

			local priority = skillPriority(option.skill_id or option.skillId, preferredSkillIds)
			local score = tonumber(option.nullkillerSkillScore or option.nullkiller_skill_score or 0) or 0
			if answer ~= nil and priority ~= nil and (bestPriority == nil or priority < bestPriority) then
				best = answer
				bestPriority = priority
				bestScore = score
			elseif answer ~= nil and bestPriority == nil and config.use_nullkiller_skill_score ~= false and config.useNullkillerSkillScore ~= false then
				if bestScore == nil or score > bestScore then
					best = answer
					bestScore = score
				end
			end
		end
		return best or defaultAnswer or 0
	end

	if typeId == self.queryTypes.blockingDialog then
		local components = query.components or {}
		local preferences = config.component_preferences or config.componentPreferences
		if preferences == nil then
			preferences = {
				{ type_id = self.componentTypes.experience },
				{ type_id = self.componentTypes.resource, subtype_id = self.resourceIds.gold }
			}
		end

		for _, preference in ipairs(preferences) do
			for _, component in ipairs(components) do
				if preferenceMatchesComponent(component, preference) then
					local answer = optionAnswer(component)
					if answer ~= nil then
						return answer
					end
				end
			end
		end

		if query.selection and #components > 0 then
			return optionAnswer(components[#components]) or firstOptionAnswer(components) or defaultAnswer or 0
		end
		if query.cancel then
			return config.cancel_answer or config.cancelAnswer or 1
		end
		return defaultAnswer or 0
	end

	if typeId == self.queryTypes.teleportDialog then
		if query.impassable then
			return config.impassable_answer or config.impassableAnswer or -1
		end
		return firstOptionAnswer(query.exits or {}) or defaultAnswer or 0
	end

	if typeId == self.queryTypes.mapObjectSelect then
		return firstOptionAnswer(query.objects or {}) or defaultAnswer or 0
	end

	return defaultAnswer
end

local function dispatchQueryDecision(aiObject, query, decision, policy)
	if decision == nil then
		return nil
	end
	if decision == false then
		return { ok = true, handled = false, query_id = query and query.query_id, reason = "query_deferred" }
	end
	if type(decision) == "number" then
		return aiObject:answerQuery(query.query_id, decision)
	end
	if type(decision) ~= "table" then
		return nil
	end

	if decision.handled == false then
		return decision
	end
	if type(decision.action) == "table" then
		return aiObject:runAction(decision.action)
	end
	if type(decision.planAction) == "table" then
		return aiObject:runAction(decision.planAction)
	end
	if decision.answer ~= nil then
		return aiObject:answerQuery(query.query_id, decision.answer)
	end
	if decision.cancel == true then
		return aiObject:cancelQuery(query.query_id)
	end
	if decision.ignore == true or decision.ignore_script_query == true or decision.ignoreScriptQuery == true then
		return aiObject:ignoreScriptDecision(query.query_id)
	end
	if decision.native == true or decision.use_nullkiller == true or decision.useNullkiller == true then
		local defaultAnswer = decision.default_answer or decision.defaultAnswer
		if defaultAnswer == nil and type(policy) == "table" then
			defaultAnswer = policy.default_answer or policy.defaultAnswer
		end
		return aiObject:nullkillerAnswerQuery(query, defaultAnswer)
	end

	return nil
end

function ai:answerQueryByPolicy(query, policy)
	local config = {}
	if type(policy) == "function" then
		config.on_query = policy
	else
		config = copyFields(policy)
	end

	if type(query) ~= "table" or query.query_id == nil then
		error("ai:answerQueryByPolicy expects a pending query with query_id", 2)
	end

	local resolver = config.on_query or config.onQuery or config.resolve or config.resolver
	if type(resolver) == "function" then
		local resolved = dispatchQueryDecision(self, query, resolver(self, query, config), config)
		if resolved ~= nil then
			return resolved
		end
	end

	local decision = dispatchQueryDecision(self, query, self:defaultQueryDecision(query, config), config)
	if decision ~= nil then
		return decision
	end

	local answer = self:defaultQueryAnswer(query, config)
	if answer ~= nil then
		return self:answerQuery(query.query_id, answer)
	end

	if self:queryHasType(query, self.queryTypes.artifactAssemblyPrompt) and config.ignore_script_decisions ~= false and config.ignoreScriptDecisions ~= false then
		return self:ignoreScriptDecision(query.query_id)
	end

	if config.native_fallback ~= false and config.nativeFallback ~= false then
		return self:nullkillerAnswerQuery(query, config.default_answer or config.defaultAnswer)
	end

	return { ok = true, handled = false, query_id = query.query_id, reason = "unsupported_query_type" }
end

ai.answerQueryWithPolicy = ai.answerQueryByPolicy
ai.handleQuery = ai.answerQueryByPolicy

function ai:answerPendingQueriesByPolicy(policy, options)
	local config = {}
	if type(policy) == "function" then
		config.on_query = policy
	else
		config = copyFields(policy)
	end

	local opts = copyFields(options)
	if type(options) == "number" then
		opts.max_queries = options
	end

	local limit = tonumber(opts.max_queries or opts.maxQueries or config.max_queries or config.maxQueries or 16) or 16
	limit = math.max(0, math.floor(limit))
	local handled = {}

	for _ = 1, limit do
		local queries = self:pendingQueries()
		if #queries == 0 then
			return {
				count = #handled,
				handled = handled,
				truncated = false
			}
		end

		local result = self:answerQueryByPolicy(queries[1], config)
		handled[#handled + 1] = result
		if type(result) == "table" and result.handled == false then
			return {
				count = #handled - 1,
				handled = handled,
				deferred = true,
				truncated = true
			}
		end
		self:refresh()
	end

	return {
		count = #handled,
		handled = handled,
		truncated = #self:pendingQueries() > 0
	}
end

ai.answerPendingQueriesWithPolicy = ai.answerPendingQueriesByPolicy
ai.handlePendingQueries = ai.answerPendingQueriesByPolicy

function ai:nullkillerAnswerQuery(query, defaultAnswer)
	local action = copyFields(query)
	if type(query) ~= "table" then
		action.query_id = query
	elseif query.query_id ~= nil then
		action.query_id = query.query_id
	end
	if defaultAnswer ~= nil then
		action.default_answer = defaultAnswer
	end
	action.allow_expired = true
	action.type = "nullkiller_answer_query"
	return self:execute(action)
end

function ai:nullkillerAnswerPendingQueries(defaultAnswer, maxQueries)
	local handled = {}
	local limit = maxQueries or 16
	for _ = 1, limit do
		local queries = self:pendingQueries()
		if #queries == 0 then
			return {
				count = #handled,
				handled = handled,
				truncated = false
			}
		end

		handled[#handled + 1] = self:nullkillerAnswerQuery(queries[1], defaultAnswer)
		self:refresh()
	end

	return {
		count = #handled,
		handled = handled,
		truncated = #self:pendingQueries() > 0
	}
end

function ai:nullkillerObjectInteraction(heroId, objectId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.object_id = objectId
	end
	action.type = "nullkiller_object_interaction"
	return self:execute(action)
end

ai.nullkillerInteract = ai.nullkillerObjectInteraction

function ai:nullkillerLockResources(resources)
	local action = copyFields(resources)
	if type(resources) ~= "table" or (resources.resources == nil and resources.resource_entries == nil) then
		action = { resources = resources }
	end
	action.type = "nullkiller_lock_resources"
	return self:execute(action)
end

function ai:nullkillerLockHero(heroId, reasonId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.reason_id = reasonId or ai.nullkillerHeroLockReasons.defense
	end
	action.type = "nullkiller_lock_hero"
	return self:execute(action)
end

function ai:nullkillerUnlockHero(heroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
	end
	action.type = "nullkiller_unlock_hero"
	return self:execute(action)
end

function ai:output(status, intent, confidence)
	return {
		status = status or "continue",
		memory = self.memoryState,
		actions = {},
		intent = intent,
		confidence = confidence
	}
end

function ai:build(townId, buildingId)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
		action.building_id = buildingId
	end
	action.type = "build"
	return self:execute(action)
end

function ai:visitTownBuilding(townId, buildingId)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
		action.building_id = buildingId
	end
	action.type = "visit_town_building"
	return self:execute(action)
end

function ai:recruit(sourceId, level, amount, creatureId, destinationId)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.level = level
		action.amount = amount
		action.creature_id = creatureId
		action.destination_id = destinationId
	end
	action.type = "recruit"
	return self:execute(action)
end

function ai:hireHero(sourceId, heroTypeId, nextHeroTypeId)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.town_id = sourceId
		action.hero_type_id = heroTypeId
		action.next_hero_type_id = nextHeroTypeId
	end
	action.type = "hire_hero"
	return self:execute(action)
end

function ai:transferArmy(sourceId, destinationId, sourceSlot)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.destination_id = destinationId
		action.source_slot = sourceSlot
	end
	action.type = "transfer_army"
	return self:execute(action)
end

function ai:bulkMoveArmy(sourceId, destinationId, sourceSlot)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.destination_id = destinationId
		action.source_slot = sourceSlot
	end
	action.type = "bulk_move_army"
	return self:execute(action)
end

function ai:swapCreatures(sourceId, sourceSlot, destinationId, destinationSlot)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.source_slot = sourceSlot
		action.destination_id = destinationId
		action.destination_slot = destinationSlot
	end
	action.type = "swap_creatures"
	return self:execute(action)
end

function ai:mergeStacks(sourceId, sourceSlot, destinationId, destinationSlot)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.source_slot = sourceSlot
		action.destination_id = destinationId
		action.destination_slot = destinationSlot
	end
	action.type = "merge_stacks"
	return self:execute(action)
end

function ai:mergeOrSwapStacks(sourceId, sourceSlot, destinationId, destinationSlot)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.source_slot = sourceSlot
		action.destination_id = destinationId
		action.destination_slot = destinationSlot
	end
	action.type = "merge_or_swap_stacks"
	return self:execute(action)
end

function ai:splitStack(sourceId, sourceSlot, destinationId, destinationSlot, amount)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.source_slot = sourceSlot
		action.destination_id = destinationId
		action.destination_slot = destinationSlot
		action.amount = amount
	end
	action.type = "split_stack"
	return self:execute(action)
end

function ai:bulkSplitStack(armyId, sourceSlot, amount)
	local action = copyFields(armyId)
	if type(armyId) ~= "table" then
		action.army_id = armyId
		action.source_slot = sourceSlot
		action.amount = amount
	end
	action.type = "bulk_split_stack"
	return self:execute(action)
end

function ai:bulkMergeStacks(armyId, sourceSlot)
	local action = copyFields(armyId)
	if type(armyId) ~= "table" then
		action.army_id = armyId
		action.source_slot = sourceSlot
	end
	action.type = "bulk_merge_stacks"
	return self:execute(action)
end

function ai:bulkSplitAndRebalanceStack(armyId, sourceSlot)
	local action = copyFields(armyId)
	if type(armyId) ~= "table" then
		action.army_id = armyId
		action.source_slot = sourceSlot
	end
	action.type = "bulk_split_rebalance_stack"
	return self:execute(action)
end

function ai:dismissCreature(armyId, slot)
	local action = copyFields(armyId)
	if type(armyId) ~= "table" then
		action.army_id = armyId
		action.slot = slot
	end
	action.type = "dismiss_creature"
	return self:execute(action)
end

function ai:upgradeCreature(armyId, slot, creatureId)
	local action = copyFields(armyId)
	if type(armyId) ~= "table" then
		action.army_id = armyId
		action.slot = slot
		action.creature_id = creatureId
	end
	action.type = "upgrade_creature"
	return self:execute(action)
end

function ai:setFormation(heroId, formationId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.formation_id = formationId
	end
	action.type = "set_formation"
	return self:execute(action)
end

function ai:setTactics(heroId, enabled)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.enabled = enabled == true
	end
	action.type = "set_tactics"
	return self:execute(action)
end

function ai:setTownName(townId, name)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
		action.name = name
	end
	action.type = "set_town_name"
	return self:execute(action)
end

function ai:swapGarrisonHero(townId)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
	end
	action.type = "swap_garrison_hero"
	return self:execute(action)
end

function ai:pickBestCreatures(destinationId, sourceId)
	local action = copyFields(destinationId)
	if type(destinationId) ~= "table" then
		action.destination_id = destinationId
		action.source_id = sourceId
	end
	action.type = "pick_best_creatures"
	return self:execute(action)
end

function ai:pickBestArtifacts(heroId, otherHeroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.other_hero_id = otherHeroId
	end
	action.type = "pick_best_artifacts"
	return self:execute(action)
end

function ai:prepareHero(heroId, sourceId, otherHeroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.source_id = sourceId
		action.other_hero_id = otherHeroId
	end
	action.type = "prepare_hero"
	if action.include_artifacts == nil then
		action.include_artifacts = true
	end
	if action.include_creatures == nil then
		action.include_creatures = true
	end
	return self:execute(action)
end

function ai:swapArtifacts(src, dst)
	return self:execute({ type = "swap_artifacts", src = src, dst = dst })
end

function ai:bulkMoveArtifacts(srcHeroId, dstHeroId, swap, equipped, backpack)
	local action = copyFields(srcHeroId)
	if type(srcHeroId) ~= "table" then
		action.src_hero_id = srcHeroId
		action.dst_hero_id = dstHeroId
		action.src_id = srcHeroId
		action.dst_id = dstHeroId
		action.swap = swap
		action.equipped = equipped
		action.backpack = backpack
	end
	action.type = "bulk_move_artifacts"
	return self:execute(action)
end

function ai:moveArtifactToAltar(marketId, heroId, slot)
	return self:swapArtifacts(
		{ holder_id = heroId, slot = slot },
		{ holder_id = marketId, slot = self.artifactSlots.altar }
	)
end

function ai:returnArtifactFromAltar(marketId, heroId, altarSlot)
	return self:swapArtifacts(
		{ holder_id = marketId, slot = altarSlot or self.artifactSlots.altar },
		{ holder_id = heroId, slot = self.artifactSlots.firstAvailable }
	)
end

function ai:moveArtifactsToAltar(marketId, heroId, equipped, backpack)
	return self:bulkMoveArtifacts({
		src_id = heroId,
		dst_id = marketId,
		src_hero_id = heroId,
		swap = false,
		equipped = equipped ~= false,
		backpack = backpack ~= false
	})
end

function ai:returnArtifactsFromAltar(marketId, heroId, equipped, backpack)
	return self:bulkMoveArtifacts({
		src_id = marketId,
		dst_id = heroId,
		dst_hero_id = heroId,
		swap = false,
		equipped = equipped ~= false,
		backpack = backpack ~= false
	})
end

function ai:sortBackpackArtifacts(heroId, mode)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.mode = mode or "slot"
	end
	action.type = "sort_backpack_artifacts"
	return self:execute(action)
end

function ai:scrollBackpackArtifacts(heroId, left)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.left = left == true
	end
	action.type = "scroll_backpack_artifacts"
	return self:execute(action)
end

function ai:manageHeroCostume(heroId, costumeIndex, saveCostume)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.costume_index = costumeIndex
		action.save = saveCostume == true
	end
	action.type = "manage_hero_costume"
	return self:execute(action)
end

function ai:assembleArtifacts(heroId, slot, artifactId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.slot = slot
		action.assemble = true
		action.artifact_id = artifactId
	end
	action.type = "assemble_artifacts"
	return self:execute(action)
end

function ai:disassembleArtifact(heroId, slot)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.slot = slot
		action.assemble = false
	end
	action.type = "assemble_artifacts"
	return self:execute(action)
end

function ai:eraseTransitionArtifact(heroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
	end
	action.type = "erase_transition_artifact"
	return self:execute(action)
end

function ai:nullkillerTrade()
	return self:execute({ type = "nullkiller_trade" })
end

function ai:nullkillerPriorityPass(passIndex)
	local action = copyFields(passIndex)
	if type(passIndex) ~= "table" then
		action.pass_index = passIndex
	end
	action.type = "nullkiller_priority_pass"
	return self:execute(action)
end

function ai:nullkillerBuildArmy(townId)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
	end
	action.type = "nullkiller_build_army"
	return self:execute(action)
end

function ai:nullkillerUpgradeArmy(armyId)
	local action = copyFields(armyId)
	if type(armyId) ~= "table" then
		action.army_id = armyId
	end
	action.type = "nullkiller_upgrade_army"
	return self:execute(action)
end

function ai:nullkillerRecruitCreatures(sourceId, destinationId)
	local action = copyFields(sourceId)
	if type(sourceId) ~= "table" then
		action.source_id = sourceId
		action.destination_id = destinationId
	end
	action.type = "nullkiller_recruit_creatures"
	return self:execute(action)
end

function ai:nullkillerMoveCreaturesToHero(townId)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
	end
	action.type = "nullkiller_move_creatures_to_hero"
	return self:execute(action)
end

function ai:nullkillerDismissWeakHero(options)
	local action = copyFields(options)
	action.type = "nullkiller_dismiss_weak_hero"
	return self:execute(action)
end

function ai:nullkillerOptimizeArtifacts(heroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" and heroId ~= nil then
		action.hero_id = heroId
	end
	action.type = "nullkiller_optimize_artifacts"
	return self:execute(action)
end

function ai:nullkillerAddSingleCreatureStacks(heroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
	end
	action.type = "nullkiller_add_single_creature_stacks"
	return self:execute(action)
end

function ai:nullkillerRearrangeForWhirlpool(heroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
	end
	action.type = "nullkiller_rearrange_for_whirlpool"
	return self:execute(action)
end

function ai:nullkillerRearrangeForSiege(heroId, townId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.town_id = townId
	end
	action.type = "nullkiller_rearrange_for_siege"
	return self:execute(action)
end

function ai:requestStatistic()
	return self:execute({ type = "request_statistic" })
end

function ai:tradeResources(marketId, sellResourceId, buyResourceId, amount, heroId)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.sell_resource_id = sellResourceId
		action.buy_resource_id = buyResourceId
		action.amount = amount
		action.hero_id = heroId
	end
	action.type = "trade_resources"
	return self:execute(action)
end

function ai:marketTrade(action)
	local payload = copyFields(action)
	payload.type = "market_trade"
	return self:execute(payload)
end

function ai:sendResources(marketId, sellResourceId, targetPlayerId, amount, heroId)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.resourcePlayer
		action.sell_resource_id = sellResourceId
		action.target_player_id = targetPlayerId
		action.amount = amount
		action.hero_id = heroId
	end
	return self:marketTrade(action)
end

function ai:sellCreatures(marketId, heroId, slot, buyResourceId, amount)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.creatureResource
		action.hero_id = heroId
		action.slot = slot
		action.buy_resource_id = buyResourceId
		action.amount = amount
	end
	return self:marketTrade(action)
end

function ai:buyMarketArtifact(marketId, heroId, sellResourceId, artifactId)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.resourceArtifact
		action.hero_id = heroId
		action.sell_resource_id = sellResourceId
		action.artifact_id = artifactId
	end
	return self:marketTrade(action)
end

function ai:sellArtifact(marketId, heroId, artifactInstanceId, buyResourceId)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.artifactResource
		action.hero_id = heroId
		action.artifact_instance_id = artifactInstanceId
		action.buy_resource_id = buyResourceId
	end
	return self:marketTrade(action)
end

function ai:sacrificeArtifact(marketId, heroId, artifactInstanceId)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.artifactExperience
		action.hero_id = heroId
		action.artifact_instance_id = artifactInstanceId
	end
	return self:marketTrade(action)
end

function ai:sacrificeArtifacts(marketId, heroId, artifactInstanceIds)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.artifactExperience
		action.hero_id = heroId
		action.artifact_instance_ids = artifactInstanceIds
	end
	return self:marketTrade(action)
end

function ai:sacrificeCreatures(marketId, heroId, slot, amount)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.creatureExperience
		action.hero_id = heroId
		action.slot = slot
		action.amount = amount
	end
	return self:marketTrade(action)
end

function ai:sacrificeCreatureStacks(marketId, heroId, slots, amounts)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.creatureExperience
		action.hero_id = heroId
		action.slots = slots
		action.amounts = amounts
	end
	return self:marketTrade(action)
end

function ai:transformToUndead(marketId, heroId, slot)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.creatureUndead
		action.hero_id = heroId
		action.slot = slot
	end
	return self:marketTrade(action)
end

function ai:buySkill(marketId, heroId, skillId)
	local action = copyFields(marketId)
	if type(marketId) ~= "table" then
		action.market_id = marketId
		action.mode_id = self.marketModes.resourceSkill
		action.hero_id = heroId
		action.skill_id = skillId
	end
	return self:marketTrade(action)
end

function ai:dismissHero(heroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
	end
	action.type = "dismiss_hero"
	return self:execute(action)
end

function ai:buildBoat(shipyardId)
	local action = copyFields(shipyardId)
	if type(shipyardId) ~= "table" then
		action.shipyard_id = shipyardId
	end
	action.type = "build_boat"
	return self:execute(action)
end

function ai:castleTeleport(heroId, destinationTownId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.destination_town_id = destinationTownId
	end
	action.type = "castle_teleport"
	return self:execute(action)
end

function ai:dig(heroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
	end
	action.type = "dig"
	return self:execute(action)
end

function ai:castSpell(heroId, spellId, x, y, z)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.spell_id = spellId
		action.x = x
		action.y = y
		action.z = z
	end
	action.type = "cast_spell"
	return self:execute(action)
end

function ai:buyArtifact(heroId, artifactId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.artifact_id = artifactId
	end
	action.type = "buy_artifact"
	return self:execute(action)
end

function ai:spellResearch(townId, spellId, accept)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
		action.spell_id = spellId
		action.accept = accept ~= false
	end
	action.type = "spell_research"
	return self:execute(action)
end

function ai:moveHero(heroId, x, y, z, routeId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.x = x
		action.y = y
		action.z = z
		action.route_id = routeId
	end
	action.type = "move_hero"
	return self:execute(action)
end

function ai:visitObject(heroId, objectId, routeId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.object_id = objectId
		action.route_id = routeId
	end
	action.type = "visit_object"
	return self:execute(action)
end

function ai:answerQuery(queryId, answer)
	local action = copyFields(queryId)
	if type(queryId) ~= "table" then
		action.query_id = queryId
		action.answer = answer
	end
	action.type = "answer_query"
	return self:execute(action)
end

function ai:cancelQuery(queryId)
	local action = copyFields(queryId)
	if type(queryId) ~= "table" then
		action.query_id = queryId
	end
	action.type = "cancel_query"
	return self:execute(action)
end

function ai:chooseChestReward(query, preference)
	local preferred = preference or "experience"
	local components = (query and query.components) or {}
	local fallback
	local preferredAnswer

	for _, component in ipairs(components) do
		local answer = component.answer
		if answer ~= nil and fallback == nil then
			fallback = answer
		end

		local typeId = component.typeId
		local subtypeId = component.subtypeId
		local wantsExperience = preferred == "experience" or preferred == self.componentTypes.experience
		local wantsGold = preferred == "gold" or preferred == "resource" or preferred == self.componentTypes.resource
		if wantsExperience and typeId == self.componentTypes.experience then
			preferredAnswer = answer
		elseif wantsGold and typeId == self.componentTypes.resource and subtypeId == self.resourceIds.gold then
			preferredAnswer = answer
		end
	end

	if not query or query.query_id == nil then
		error("Chest reward query is missing query_id", 2)
	end
	return self:answerQuery(query.query_id, preferredAnswer or fallback or 0)
end

function ai:ignoreScriptDecision(queryId)
	local action = copyFields(queryId)
	if type(queryId) ~= "table" then
		action.query_id = queryId
	end
	action.type = "ignore_script_query"
	return self:execute(action)
end

ai.ignoreScriptQuery = ai.ignoreScriptDecision

function ai:endTurn()
	return self:execute({ type = "end_turn" })
end

return ai
)lua";

bool hasJsonField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

int resumeLuaThread(lua_State * thread, lua_State * parent, int nargs, int & nresults)
{
#if LUA_VERSION_NUM >= 504
	return lua_resume(thread, parent, nargs, &nresults);
#elif LUA_VERSION_NUM >= 502
	const int result = lua_resume(thread, parent, nargs);
	nresults = lua_gettop(thread);
	return result;
#else
	const int result = lua_resume(thread, nargs);
	nresults = lua_gettop(thread);
	return result;
#endif
}

}

LuaAdventureScriptRunner::LuaAdventureScriptRunner(std::string identifier_, std::string sourceText, AI::AdventureScriptLimits limits_)
	: L(luaL_newstate())
	, identifier(std::move(identifier_))
	, limits(limits_)
{
	if(!L)
		throw std::runtime_error("Failed to create Lua state for adventure script");

	static constexpr luaL_Reg STD_LIBS[] =
	{
		{"_G", luaopen_base},
		{LUA_TABLIBNAME, luaopen_table},
		{LUA_STRLIBNAME, luaopen_string},
		{LUA_MATHLIBNAME, luaopen_math},
#if LUA_VERSION_NUM >= 502
		{LUA_COLIBNAME, luaopen_coroutine}
		,
#endif
		{nullptr, nullptr}
	};

	for(const luaL_Reg * lib = STD_LIBS; lib->func; ++lib)
	{
		lib->func(L);
		lua_setglobal(L, lib->name);
	}

	cleanupGlobals();

	if(luaL_loadbuffer(L, sourceText.c_str(), sourceText.size(), identifier.c_str()) != 0)
	{
		std::string error = toStringRaw(-1);
		lua_settop(L, 0);
		throw std::runtime_error("Failed to compile adventure script '" + identifier + "': " + error);
	}

	if(lua_pcall(L, 0, 1, 0) != 0)
	{
		std::string error = toStringRaw(-1);
		lua_settop(L, 0);
		throw std::runtime_error("Failed to initialize adventure script '" + identifier + "': " + error);
	}

	if(!lua_istable(L, -1))
	{
		lua_settop(L, 0);
		throw std::runtime_error("Adventure script '" + identifier + "' must return a table");
	}

	scriptTableRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

LuaAdventureScriptRunner::~LuaAdventureScriptRunner()
{
	if(L)
	{
		if(scriptTableRef != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, scriptTableRef);
		lua_close(L);
	}
}

bool LuaAdventureScriptRunner::hasRunDay()
{
	LuaStack stack(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, scriptTableRef);
	lua_getfield(L, -1, "runDay");
	const bool result = lua_isfunction(L, -1);
	stack.restoreInitialTop();
	return result;
}

AI::AdventureScriptOutput LuaAdventureScriptRunner::planDay(const AI::AdventureScriptInput & input)
{
	LuaStack stack(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, scriptTableRef);
	lua_getfield(L, -1, "planDay");
	lua_remove(L, -2);

	if(!lua_isfunction(L, -1))
	{
		stack.clear();
		throw std::runtime_error("Adventure script '" + identifier + "' does not define planDay");
	}

	stack.push(input.toJson());

	if(lua_pcall(L, 1, 1, 0) != 0)
	{
		std::string error = toStringRaw(-1);
		stack.clear();
		throw std::runtime_error("Adventure script '" + identifier + "' planDay failed: " + error);
	}

	JsonNode rawOutput;
	try
	{
		stack.get(stack.absindex(-1), rawOutput);
	}
	catch(const LuaApiException & e)
	{
		stack.clear();
		throw std::runtime_error("Adventure script '" + identifier + "' returned unsupported value: " + e.what());
	}

	stack.restoreInitialTop();
	return AI::parseAdventureScriptOutput(rawOutput, limits);
}

AI::AdventureScriptOutput LuaAdventureScriptRunner::runDayImperative(
	const AI::AdventureScriptInput & input,
	const std::function<JsonNode(const JsonNode &)> & commandHandler)
{
	LuaStack stack(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, scriptTableRef);
	lua_getfield(L, -1, "runDay");
	lua_remove(L, -2);

	if(!lua_isfunction(L, -1))
	{
		stack.clear();
		throw std::runtime_error("Adventure script '" + identifier + "' does not define runDay");
	}

	lua_State * thread = lua_newthread(L);
	const int threadRef = luaL_ref(L, LUA_REGISTRYINDEX);
	auto unrefThread = vstd::makeScopeGuard([&]
	{
		luaL_unref(L, LUA_REGISTRYINDEX, threadRef);
	});

	lua_xmove(L, thread, 1);
	pushImperativeApi(thread, input);
	LuaStack threadStack(thread);
	threadStack.push(input.toJson());

	int nargs = 2;
	size_t commands = 0;

	while(true)
	{
		int nresults = 0;
		const int status = resumeLuaThread(thread, L, nargs, nresults);
		nargs = 0;

		if(status == LUA_OK)
		{
			JsonNode rawOutput;
			if(nresults == 0 || lua_isnil(thread, -1))
			{
				rawOutput["status"] = JsonNode("continue");
				rawOutput["memory"] = input.memory;
				rawOutput["actions"].Vector();
			}
			else
			{
				try
				{
					threadStack.get(threadStack.absindex(-1), rawOutput);
				}
				catch(const LuaApiException & e)
				{
					threadStack.clear();
					throw std::runtime_error("Adventure script '" + identifier + "' returned unsupported value: " + e.what());
				}
				if(rawOutput.isStruct() && !hasJsonField(rawOutput, "memory"))
					rawOutput["memory"] = input.memory;
			}

			threadStack.clear();
			stack.restoreInitialTop();
			return AI::parseAdventureScriptOutput(rawOutput, limits);
		}

		if(status != LUA_YIELD)
		{
			std::string error = toStringRaw(thread, -1);
			threadStack.clear();
			stack.restoreInitialTop();
			throw std::runtime_error("Adventure script '" + identifier + "' runDay failed: " + error);
		}

		if(nresults != 1)
		{
			threadStack.clear();
			stack.restoreInitialTop();
			throw std::runtime_error("Adventure script '" + identifier + "' yielded an invalid command count");
		}

		JsonNode command;
		try
		{
			threadStack.get(threadStack.absindex(-1), command);
		}
		catch(const LuaApiException & e)
		{
			threadStack.clear();
			stack.restoreInitialTop();
			throw std::runtime_error("Adventure script '" + identifier + "' yielded unsupported command: " + e.what());
		}
		threadStack.clear();

		if(!command.isStruct() || !command["kind"].isString())
		{
			stack.restoreInitialTop();
			throw std::runtime_error("Adventure script '" + identifier + "' yielded a command without string kind");
		}

		const std::string kind = command["kind"].String();
		if(kind == "fallback")
		{
			JsonNode rawOutput;
			rawOutput["status"] = JsonNode("fallback");
			rawOutput["memory"] = hasJsonField(command, "memory") ? command["memory"] : input.memory;
			rawOutput["actions"].Vector();
			if(command["intent"].isString())
				rawOutput["intent"] = command["intent"];

			stack.restoreInitialTop();
			return AI::parseAdventureScriptOutput(rawOutput, limits);
		}

		if(kind != "execute" && kind != "refresh" && kind != "inspect")
		{
			stack.restoreInitialTop();
			throw std::runtime_error("Adventure script '" + identifier + "' yielded unsupported command kind: " + kind);
		}

		if(++commands > limits.maxActions)
		{
			stack.restoreInitialTop();
			throw std::runtime_error("Adventure script '" + identifier + "' exceeded imperative command limit");
		}

		JsonNode response = commandHandler(command);
		threadStack.push(response);
		nargs = 1;
	}
}

std::optional<JsonNode> LuaAdventureScriptRunner::decideBattleRetreat(const JsonNode & input)
{
	LuaStack stack(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, scriptTableRef);
	lua_getfield(L, -1, "decideBattleRetreat");
	lua_remove(L, -2);

	if(lua_isnil(L, -1))
	{
		stack.restoreInitialTop();
		return std::nullopt;
	}

	if(!lua_isfunction(L, -1))
	{
		stack.clear();
		throw std::runtime_error("Adventure script '" + identifier + "' field decideBattleRetreat is not a function");
	}

	stack.push(input);

	if(lua_pcall(L, 1, 1, 0) != 0)
	{
		std::string error = toStringRaw(-1);
		stack.clear();
		throw std::runtime_error("Adventure script '" + identifier + "' decideBattleRetreat failed: " + error);
	}

	if(lua_isnil(L, -1))
	{
		stack.restoreInitialTop();
		return std::nullopt;
	}

	JsonNode rawOutput;
	try
	{
		stack.get(stack.absindex(-1), rawOutput);
	}
	catch(const LuaApiException & e)
	{
		stack.clear();
		throw std::runtime_error("Adventure script '" + identifier + "' decideBattleRetreat returned unsupported value: " + e.what());
	}

	stack.restoreInitialTop();
	return rawOutput;
}

void LuaAdventureScriptRunner::cleanupGlobals()
{
	LuaStack stack(L);
	stack.clear();

	stack.pushNil();
	lua_setglobal(L, "collectgarbage");
	stack.pushNil();
	lua_setglobal(L, "dofile");
	stack.pushNil();
	lua_setglobal(L, "load");
	stack.pushNil();
	lua_setglobal(L, "loadfile");
	stack.pushNil();
	lua_setglobal(L, "loadstring");

	lua_pushcfunction(L, luaPrint);
	lua_setglobal(L, "print");
	lua_pushcfunction(L, luaError);
	lua_setglobal(L, "error");
	lua_pushcfunction(L, luaAssert);
	lua_setglobal(L, "assert");

	lua_getglobal(L, LUA_STRLIBNAME);
	stack.push("dump");
	stack.pushNil();
	lua_rawset(L, -3);
	stack.clear();

	lua_getglobal(L, LUA_MATHLIBNAME);
	stack.push("random");
	stack.pushNil();
	lua_rawset(L, -3);
	stack.push("randomseed");
	stack.pushNil();
	lua_rawset(L, -3);
	stack.clear();
}

std::string LuaAdventureScriptRunner::toStringRaw(int index) const
{
	return toStringRaw(L, index);
}

std::string LuaAdventureScriptRunner::toStringRaw(lua_State * state, int index) const
{
	size_t len = 0;
	const auto * raw = lua_tolstring(state, index, &len);
	return raw ? std::string(raw, len) : std::string();
}

void LuaAdventureScriptRunner::pushImperativeApi(lua_State * state, const AI::AdventureScriptInput & input) const
{
	if(luaL_loadbuffer(state, IMPERATIVE_API_PRELUDE, std::strlen(IMPERATIVE_API_PRELUDE), "AdventureScriptAI API") != 0)
	{
		std::string error = toStringRaw(state, -1);
		lua_settop(state, 0);
		throw std::runtime_error("Failed to compile AdventureScriptAI API: " + error);
	}

	LuaStack stack(state);
	stack.push(input.toJson());
	if(lua_pcall(state, 1, 1, 0) != 0)
	{
		std::string error = toStringRaw(state, -1);
		lua_settop(state, 0);
		throw std::runtime_error("Failed to initialize AdventureScriptAI API: " + error);
	}
}

int LuaAdventureScriptRunner::luaPrint(lua_State * L)
{
	int n = lua_gettop(L);
	lua_getglobal(L, "tostring");
	std::string out;
	for(int i = 1; i <= n; ++i)
	{
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);
		const char * text = lua_tostring(L, -1);
		if(text)
			out += text;
		if(i > 1)
			out += '\t';
		lua_pop(L, 1);
	}

	logScript->info("%s", out);
	return 0;
}

int LuaAdventureScriptRunner::luaError(lua_State * L)
{
	int level = luaL_optinteger(L, 2, 1);

	if(level > 0 && lua_isstring(L, 1))
	{
		luaL_where(L, level);
		lua_pushvalue(L, 1);
		lua_concat(L, 2);
		lua_replace(L, 1);
	}

	const char * msg = lua_tostring(L, 1);
	if(msg)
		logScript->warn("%s", msg);

	lua_settop(L, 1);
	return lua_error(L);
}

int LuaAdventureScriptRunner::luaAssert(lua_State * L)
{
	if(lua_toboolean(L, 1))
		return lua_gettop(L);

	luaL_where(L, 1);
	lua_pushstring(L, luaL_optstring(L, 2, "assertion failed!"));
	lua_concat(L, 2);

	const char * msg = lua_tostring(L, -1);
	if(msg)
		logScript->warn("%s", msg);

	return lua_error(L);
}

}

VCMI_LIB_NAMESPACE_END
