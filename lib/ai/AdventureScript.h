/*
 * AdventureScript.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "AdventurePlan.h"

#include <optional>
#include <string>
#include <vector>

namespace AI
{

enum class AdventureScriptStatus
{
	CONTINUE,
	END_TURN,
	NEED_REPLAN,
	FALLBACK
};

struct AdventureScriptLimits
{
	size_t maxActions = 64;
	size_t maxMemoryBytes = 256 * 1024;
};

struct AdventureScriptInput
{
	JsonNode state;
	JsonNode updates;
	JsonNode opponentUpdates;
	JsonNode progress;
	JsonNode memory;
	JsonNode actionSpace;
	JsonNode analysis;
	JsonNode limits;

	JsonNode toJson() const;
};

struct AdventureScriptOutput
{
	AdventureScriptStatus status = AdventureScriptStatus::CONTINUE;
	JsonNode memory;
	std::vector<JsonNode> actions;
	std::vector<std::string> returnSelect;
	std::optional<std::string> intent;
	std::optional<double> confidence;
};

std::string DLL_LINKAGE adventureScriptStatusToString(AdventureScriptStatus status);
AdventureScriptStatus DLL_LINKAGE adventureScriptStatusFromString(const std::string & status);

AdventureScriptOutput DLL_LINKAGE parseAdventureScriptOutput(const JsonNode & output, const AdventureScriptLimits & limits = {});
JsonNode DLL_LINKAGE makeAdventureScriptOutputJson(const AdventureScriptOutput & output);

}
