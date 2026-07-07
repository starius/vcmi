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

#include "../lib/logging/CLogger.h"

VCMI_LIB_NAMESPACE_BEGIN

namespace scripting
{

LuaAdventureScriptRunner::LuaAdventureScriptRunner(std::string identifier_, std::string sourceText, AI::AdventureScriptLimits limits_)
	: L(luaL_newstate())
	, identifier(std::move(identifier_))
	, limits(limits_)
{
	if(!L)
		throw std::runtime_error("Failed to create Lua state for adventure script");

	static constexpr std::array<luaL_Reg, 4> STD_LIBS =
	{{
		{"_G", luaopen_base},
		{LUA_TABLIBNAME, luaopen_table},
		{LUA_STRLIBNAME, luaopen_string},
		{LUA_MATHLIBNAME, luaopen_math}
	}};

	for(const luaL_Reg & lib : STD_LIBS)
	{
		lib.func(L);
		lua_setglobal(L, lib.name);
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
	size_t len = 0;
	const auto * raw = lua_tolstring(L, index, &len);
	return raw ? std::string(raw, len) : std::string();
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
