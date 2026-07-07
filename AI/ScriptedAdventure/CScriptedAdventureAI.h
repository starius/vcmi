/*
 * CScriptedAdventureAI.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../Nullkiller2/AIGateway.h"
#include "../../lib/ai/AdventureScript.h"

#include <optional>

VCMI_LIB_NAMESPACE_BEGIN
namespace scripting
{
class LuaAdventureScriptRunner;
}
VCMI_LIB_NAMESPACE_END

namespace ScriptedAdventureAI
{

class CScriptedAdventureAI final : public NK2AI::AIGateway
{
public:
	CScriptedAdventureAI();
	~CScriptedAdventureAI() override;

	void yourTurn(QueryID queryID) override;

private:
	struct RoutePlan
	{
		bool ok = false;
		bool stopAfterMove = false;
		std::string error;
		std::string routeID;
		int3 destination;
		EPathfindingLayer layer = EPathfindingLayer::WRONG;
	};

	JsonNode scriptMemory;
	std::string scriptPath;
	AI::AdventureScriptLimits limits;
	size_t maxScriptCallsPerTurn = 8;

	void makeScriptedTurn();
	bool tryMakeScriptedTurn();
	bool executeScriptAction(const JsonNode & action, JsonNode & actionResult);
	JsonNode makeScriptInputState();
	JsonNode makeScriptActionSpace() const;
	JsonNode makeScriptAnalysis() const;
	JsonNode makeScriptInputLimits() const;
	JsonNode makeProgressJson(const JsonNode & executed, const JsonNode & failed, const JsonNode & remaining) const;
	RoutePlan makeRoutePlan(const CGHeroInstance * hero, const int3 & destination, const std::optional<std::string> & expectedRouteID) const;
	std::optional<std::string> loadScriptSource() const;
	std::unique_ptr<scripting::LuaAdventureScriptRunner> makeRunner(const std::string & source) const;
	void fallbackToNullkiller(const std::string & reason);
};

}
