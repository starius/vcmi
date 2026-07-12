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
	int commandCount = 0;
	std::string status;
	std::string error;
	std::vector<LuaCommand> commands;
};

struct LuaRunInput
{
	int difficultyLevel = 1;
	std::function<bool(const LuaCommand &)> commandHandler;
};

class LuaNullkiller2Runner
{
public:
	LuaTurnResult runDay(const std::function<void()> & endTurn, const LuaRunInput & input = LuaRunInput());
};

}
