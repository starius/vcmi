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

function ai:nullkillerStep(mode, maxCandidates)
	local action = copyFields(mode)
	if type(mode) ~= "table" then
		action.mode = mode or "all"
		action.max_candidates = maxCandidates
	end
	action.type = "nullkiller_step"
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

function ai:pickBestArtifacts(heroId, otherHeroId)
	local action = copyFields(heroId)
	if type(heroId) ~= "table" then
		action.hero_id = heroId
		action.other_hero_id = otherHeroId
	end
	action.type = "pick_best_artifacts"
	return self:execute(action)
end

function ai:nullkillerTrade()
	return self:execute({ type = "nullkiller_trade" })
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
