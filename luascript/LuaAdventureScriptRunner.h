/*
 * LuaAdventureScriptRunner.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../lib/ai/AdventureScript.h"

#if __has_include(<lua.hpp>)
#  include <lua.hpp>
#else
#  include <lua.h>
#  include <lauxlib.h>
#  include <lualib.h>
#endif

VCMI_LIB_NAMESPACE_BEGIN

namespace scripting
{

class LuaAdventureScriptRunner
{
public:
	LuaAdventureScriptRunner(std::string identifier, std::string sourceText, AI::AdventureScriptLimits limits = {});
	~LuaAdventureScriptRunner();

	LuaAdventureScriptRunner(const LuaAdventureScriptRunner &) = delete;
	LuaAdventureScriptRunner & operator=(const LuaAdventureScriptRunner &) = delete;

	AI::AdventureScriptOutput planDay(const AI::AdventureScriptInput & input);

private:
	lua_State * L = nullptr;
	std::string identifier;
	AI::AdventureScriptLimits limits;
	int scriptTableRef = LUA_NOREF;

	void cleanupGlobals();
	std::string toStringRaw(int index) const;

	static int luaPrint(lua_State * L);
	static int luaError(lua_State * L);
	static int luaAssert(lua_State * L);
};

}

VCMI_LIB_NAMESPACE_END
