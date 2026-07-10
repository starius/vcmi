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
	local response = coroutine.yield({ kind = "execute", payload = action })
	if type(response) ~= "table" or not response.ok then
		error(hostError(response), 2)
	end
	return response.result or response
end

function ai:refresh()
	local response = coroutine.yield({ kind = "refresh" })
	if type(response) ~= "table" or not response.ok then
		error(hostError(response), 2)
	end
	self.input = response.input or self.input
	return self.input
end

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
	exploration = 11
}

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
	action.type = "nullkiller_answer_query"
	return self:execute(action)
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

function ai:hireHero(townId, heroTypeId, nextHeroTypeId)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
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

function ai:swapArtifacts(src, dst)
	return self:execute({ type = "swap_artifacts", src = src, dst = dst })
end

function ai:bulkMoveArtifacts(srcHeroId, dstHeroId, swap, equipped, backpack)
	local action = copyFields(srcHeroId)
	if type(srcHeroId) ~= "table" then
		action.src_hero_id = srcHeroId
		action.dst_hero_id = dstHeroId
		action.swap = swap
		action.equipped = equipped
		action.backpack = backpack
	end
	action.type = "bulk_move_artifacts"
	return self:execute(action)
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

function ai:nullkillerBuildArmy(townId)
	local action = copyFields(townId)
	if type(townId) ~= "table" then
		action.town_id = townId
	end
	action.type = "nullkiller_build_army"
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

		if(kind != "execute" && kind != "refresh")
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
