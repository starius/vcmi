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
#include <string>

namespace LuaNullkiller2AI
{

struct LuaTurnResult
{
	bool ok = false;
	bool requestedEndTurn = false;
	std::string status;
	std::string error;
};

class LuaNullkiller2Runner
{
public:
	LuaTurnResult runDay(const std::function<void()> & endTurn);
};

}
