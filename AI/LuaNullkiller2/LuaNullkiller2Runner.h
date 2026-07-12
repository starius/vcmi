/*
 * LuaNullkiller2Runner.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../../lib/json/JsonNode.h"

namespace LuaNullkiller2AI
{

struct LuaCommand
{
	std::string name;
	std::map<std::string, int> integers;
};

struct LuaTurnResult
{
	bool ok = false;
	bool requestedEndTurn = false;
	bool hasMemory = false;
	int commandCount = 0;
	std::string status;
	std::string error;
	JsonNode memory;
	std::map<std::string, int> integers;
	std::vector<LuaCommand> commands;
};

struct LuaRunInput
{
	int difficultyLevel = 1;
	std::function<bool(const LuaCommand &)> commandHandler;
	JsonNode memory;
	JsonNode snapshot;
};

class LuaNullkiller2Runner
{
public:
	LuaTurnResult runFunction(const std::string & functionName, const std::function<void()> & endTurn, const LuaRunInput & input = LuaRunInput());
	LuaTurnResult runDay(const std::function<void()> & endTurn, const LuaRunInput & input = LuaRunInput());
};

}
