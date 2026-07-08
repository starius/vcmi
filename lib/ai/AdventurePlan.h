/*
 * AdventurePlan.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../json/JsonNode.h"

#include <string>
#include <vector>

namespace AI
{

/// Shared adventure-day plan contract used by MCP and future script-driven adventure AI.
///
/// Future script callers should provide current visible game state plus recent progress/opponent updates,
/// round-trip script-owned long-term context without interpreting it as game rules state, execute the returned
/// daily actions until completion or partial progress, allow re-entry when an action is invalid or produces an
/// unknown outcome, and fall back to the native adventure AI if the script fails.
JsonNode DLL_LINKAGE makeExecutePlanSchema();

std::vector<std::string> DLL_LINKAGE acceptedPlanActionTypes();
std::string DLL_LINKAGE canonicalPlanActionType(std::string type);
JsonNode DLL_LINKAGE normalizePlanAction(const JsonNode & action);

}
