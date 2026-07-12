/*
 * LuaNullkiller2Runner.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "LuaNullkiller2Runner.h"

#include "../../lib/ScopeGuard.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/json/JsonNode.h"
#include "../../lib/json/JsonUtils.h"

#include <array>

#if __has_include(<lua.hpp>)
#  include <lua.hpp>
#else
#  include <lua.h>
#  include <lauxlib.h>
#  include <lualib.h>
#endif

namespace LuaNullkiller2AI
{
namespace
{

constexpr const char * SCRIPT_ROOT = "ai/nullkiller2/";
constexpr const char * MAIN_MODULE = "ai/nullkiller2/main";
constexpr const char * SETTINGS_MODULE = "config/ai/nk2ai/nk2ai-settings";

struct RunContext
{
	const std::function<void()> * endTurn = nullptr;
	const std::function<bool(const LuaCommand &)> * commandHandler = nullptr;
	bool requestedEndTurn = false;
	std::vector<LuaCommand> commands;
};

std::string toStringRaw(lua_State * state, int index)
{
	size_t len = 0;
	const auto * raw = lua_tolstring(state, index, &len);
	return raw ? std::string(raw, len) : std::string();
}

void readIntegerPayloadField(lua_State * state, int tableIndex, LuaCommand & command, const char * field)
{
	lua_getfield(state, tableIndex, field);
	if(lua_isnumber(state, -1))
		command.integers[field] = static_cast<int>(lua_tointeger(state, -1));
	lua_pop(state, 1);
}

LuaCommand readCommand(lua_State * state, const std::string & commandName, int payloadIndex)
{
	LuaCommand command;
	command.name = commandName;

	if(payloadIndex < 0)
		payloadIndex = lua_gettop(state) + payloadIndex + 1;

	if(lua_istable(state, payloadIndex))
	{
		static constexpr std::array<const char *, 11> INTEGER_FIELDS =
		{
			"town",
			"hero",
			"bid",
			"shipyard",
			"spell",
			"x",
			"y",
			"z",
			"objid",
			"creature",
			"count"
		};

		for(const char * field : INTEGER_FIELDS)
			readIntegerPayloadField(state, payloadIndex, command, field);
	}

	return command;
}

std::string moduleToResourcePath(const std::string & moduleName)
{
	std::string modulePath = moduleName;
	std::ranges::replace(modulePath, '.', '/');

	if(modulePath.rfind(SCRIPT_ROOT, 0) == 0)
		return modulePath;

	return std::string(SCRIPT_ROOT) + modulePath;
}

std::string difficultyName(int difficultyLevel)
{
	if(difficultyLevel >= 0 && difficultyLevel < std::size(GameConstants::DIFFICULTY_NAMES))
		return GameConstants::DIFFICULTY_NAMES[difficultyLevel];

	logAi->warn("LuaNullkiller2 got invalid difficulty %d, using knight", difficultyLevel);
	return GameConstants::DIFFICULTY_NAMES[1];
}

bool loadScriptResource(lua_State * state, const std::string & modulePath)
{
	auto * loader = CResourceHandler::get();
	ScriptPath id = ScriptPath::builtinTODO(modulePath).addPrefix("SCRIPTS/");

	if(!loader->existsResource(id))
	{
		lua_pushfstring(state, "LuaNullkiller2 module not found: %s", modulePath.c_str());
		return false;
	}

	auto rawData = loader->load(id)->readAll();
	auto sourceText = std::string(reinterpret_cast<char *>(rawData.first.get()), rawData.second);
	return luaL_loadbuffer(state, sourceText.c_str(), sourceText.size(), modulePath.c_str()) == 0;
}

void pushJsonNode(lua_State * state, const JsonNode & node)
{
	switch(node.getType())
	{
	case JsonNode::JsonType::DATA_NULL:
		lua_pushnil(state);
		break;
	case JsonNode::JsonType::DATA_BOOL:
		lua_pushboolean(state, node.Bool());
		break;
	case JsonNode::JsonType::DATA_FLOAT:
		lua_pushnumber(state, node.Float());
		break;
	case JsonNode::JsonType::DATA_INTEGER:
		lua_pushinteger(state, static_cast<lua_Integer>(node.Integer()));
		break;
	case JsonNode::JsonType::DATA_STRING:
		lua_pushlstring(state, node.String().c_str(), node.String().size());
		break;
	case JsonNode::JsonType::DATA_VECTOR:
	{
		lua_newtable(state);
		lua_Integer index = 1;
		for(const auto & item : node.Vector())
		{
			pushJsonNode(state, item);
			lua_rawseti(state, -2, index++);
		}
		break;
	}
	case JsonNode::JsonType::DATA_STRUCT:
	{
		lua_newtable(state);
		for(const auto & item : node.Struct())
		{
			pushJsonNode(state, item.second);
			lua_setfield(state, -2, item.first.c_str());
		}
		break;
	}
	}
}

int luaRequire(lua_State * state)
{
	if(lua_gettop(state) != 1 || !lua_isstring(state, 1))
	{
		lua_pushstring(state, "require: module name must be a string");
		return lua_error(state);
	}

	const std::string moduleName = toStringRaw(state, 1);
	const std::string modulePath = moduleToResourcePath(moduleName);

	if(!loadScriptResource(state, modulePath))
		return lua_error(state);

	if(lua_pcall(state, 0, 1, 0) != 0)
		return lua_error(state);

	if(!lua_istable(state, -1))
	{
		lua_pushfstring(state, "require: module '%s' did not return a table", moduleName.c_str());
		return lua_error(state);
	}

	return 1;
}

int luaEndTurn(lua_State * state)
{
	auto * context = static_cast<RunContext *>(lua_touserdata(state, lua_upvalueindex(1)));
	if(!context || !context->endTurn)
	{
		lua_pushstring(state, "endTurn: missing run context");
		return lua_error(state);
	}

	(*context->endTurn)();
	context->requestedEndTurn = true;

	lua_newtable(state);
	lua_pushboolean(state, true);
	lua_setfield(state, -2, "ok");
	return 1;
}

int luaTrace(lua_State * state)
{
	const char * event = lua_tostring(state, 2);
	if(!event)
		event = lua_tostring(state, 1);

	if(event)
		logAi->debug("LuaNullkiller2 trace: %s", event);

	return 0;
}

int luaCommand(lua_State * state)
{
	auto * context = static_cast<RunContext *>(lua_touserdata(state, lua_upvalueindex(1)));
	if(!context)
	{
		lua_pushstring(state, "command: missing run context");
		return lua_error(state);
	}

	const char * command = lua_tostring(state, 2);
	if(!command)
		command = lua_tostring(state, 1);
	if(!command)
	{
		lua_pushstring(state, "command: name must be a string");
		return lua_error(state);
	}

	LuaCommand luaCommand = readCommand(state, command, 3);
	context->commands.push_back(luaCommand);
	logAi->debug("LuaNullkiller2 command: %s", command);

	bool executed = false;
	if(context->commandHandler)
	{
		try
		{
			executed = (*context->commandHandler)(luaCommand);
		}
		catch(const std::exception & e)
		{
			lua_pushfstring(state, "command '%s' failed: %s", command, e.what());
			return lua_error(state);
		}

		if(!executed)
		{
			lua_pushfstring(state, "command '%s' failed", command);
			return lua_error(state);
		}
	}

	lua_newtable(state);
	lua_pushboolean(state, true);
	lua_setfield(state, -2, "ok");
	lua_pushboolean(state, executed);
	lua_setfield(state, -2, "executed");
	lua_pushboolean(state, !executed);
	lua_setfield(state, -2, "queued");
	lua_pushinteger(state, static_cast<lua_Integer>(context->commands.size()));
	lua_setfield(state, -2, "commandIndex");
	return 1;
}

void openSafeLibraries(lua_State * state)
{
	static constexpr luaL_Reg STD_LIBS[] =
	{
		{"_G", luaopen_base},
		{LUA_TABLIBNAME, luaopen_table},
		{LUA_STRLIBNAME, luaopen_string},
		{LUA_MATHLIBNAME, luaopen_math},
		{nullptr, nullptr}
	};

	for(const luaL_Reg * lib = STD_LIBS; lib->func; ++lib)
	{
		lib->func(state);
		lua_setglobal(state, lib->name);
	}

	lua_pushcfunction(state, luaRequire);
	lua_setglobal(state, "require");
}

void pushAiFacade(lua_State * state, RunContext & context)
{
	lua_newtable(state);

	lua_pushlightuserdata(state, &context);
	lua_pushcclosure(state, luaEndTurn, 1);
	lua_setfield(state, -2, "endTurn");

	lua_pushcfunction(state, luaTrace);
	lua_setfield(state, -2, "trace");

	lua_pushlightuserdata(state, &context);
	lua_pushcclosure(state, luaCommand, 1);
	lua_setfield(state, -2, "command");
}

void pushSettingsInput(lua_State * state, const LuaRunInput & input)
{
	const auto name = difficultyName(input.difficultyLevel);

	lua_newtable(state);

	lua_pushlstring(state, name.c_str(), name.size());
	lua_setfield(state, -2, "difficultyName");

	lua_pushinteger(state, input.difficultyLevel);
	lua_setfield(state, -2, "difficultyLevel");

	try
	{
		const JsonNode rootNode = JsonUtils::assembleFromFiles(SETTINGS_MODULE);
		pushJsonNode(state, rootNode[name]);
		lua_setfield(state, -2, "values");
	}
	catch(const std::exception & e)
	{
		logAi->error("LuaNullkiller2 failed to load settings: %s", e.what());
		lua_newtable(state);
		lua_setfield(state, -2, "values");
	}
}

void pushInput(lua_State * state, const LuaRunInput & input)
{
	lua_newtable(state);

	pushSettingsInput(state, input);
	lua_setfield(state, -2, "settings");

	lua_newtable(state);
	lua_setfield(state, -2, "memory");
}

LuaTurnResult makeError(std::string error, bool requestedEndTurn)
{
	LuaTurnResult result;
	result.ok = false;
	result.requestedEndTurn = requestedEndTurn;
	result.error = std::move(error);
	return result;
}

}

LuaTurnResult LuaNullkiller2Runner::runDay(const std::function<void()> & endTurn, const LuaRunInput & input)
{
	lua_State * state = luaL_newstate();
	if(!state)
		return makeError("failed to create Lua state", false);

	auto closeState = vstd::makeScopeGuard([&]
	{
		lua_close(state);
	});

	RunContext context;
	context.endTurn = &endTurn;
	context.commandHandler = input.commandHandler ? &input.commandHandler : nullptr;

	openSafeLibraries(state);

	if(!loadScriptResource(state, MAIN_MODULE))
		return makeError(toStringRaw(state, -1), context.requestedEndTurn);

	if(lua_pcall(state, 0, 1, 0) != 0)
		return makeError(toStringRaw(state, -1), context.requestedEndTurn);

	if(!lua_istable(state, -1))
		return makeError("main script did not return a table", context.requestedEndTurn);

	lua_getfield(state, -1, "runDay");
	if(!lua_isfunction(state, -1))
		return makeError("main script does not define runDay", context.requestedEndTurn);

	pushAiFacade(state, context);
	pushInput(state, input);

	if(lua_pcall(state, 2, 1, 0) != 0)
		return makeError(toStringRaw(state, -1), context.requestedEndTurn);

	if(!lua_istable(state, -1))
		return makeError("runDay did not return a table", context.requestedEndTurn);

	LuaTurnResult result;
	result.ok = true;
	result.requestedEndTurn = context.requestedEndTurn;
	result.commandCount = static_cast<int>(context.commands.size());
	result.commands = std::move(context.commands);

	lua_getfield(state, -1, "status");
	if(lua_isstring(state, -1))
		result.status = toStringRaw(state, -1);
	lua_pop(state, 1);

	return result;
}

}
