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

#include <deque>
#include <mutex>
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

	void initGameInterface(std::shared_ptr<Environment> env, std::shared_ptr<CCallback> callback) override;
	void yourTurn(QueryID queryID) override;
	void buildChanged(const CGTownInstance * town, BuildingID buildingID, int what) override;
	void heroMoved(const TryMoveHero & details, bool verbose = true) override;
	void heroCreated(const CGHeroInstance * hero) override;
	void heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town) override;
	void tileRevealed(const FowTilesType & pos) override;
	void newObject(const CGObjectInstance * obj) override;
	void objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator) override;

private:
	struct ScriptConfig
	{
		bool reloadScriptEachTurn = true;
		bool trace = false;
		size_t maxConsecutiveFailures = 3;
		int disableTurnsAfterFailures = 3;
	};

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
	ScriptConfig scriptConfig;
	std::optional<std::string> cachedScriptSource;
	size_t traceSequence = 0;
	size_t consecutiveScriptFailures = 0;
	int disabledUntilDay = 0;
	bool failureRecordedThisTurn = false;
	std::string lastPersistedScriptState;
	mutable std::mutex scriptUpdateMutex;
	std::deque<JsonNode> scriptUpdateJournal;
	uint64_t scriptUpdateRevision = 0;
	size_t maxScriptUpdateJournal = 256;

	void makeScriptedTurn();
	bool tryMakeScriptedTurn();
	bool executeScriptAction(const JsonNode & action, JsonNode & actionResult);
	JsonNode makeScriptInputState();
	JsonNode makeScriptActionSpace() const;
	JsonNode makeScriptAnalysis() const;
	JsonNode makeScriptUpdates(bool opponentOnly) const;
	JsonNode makeScriptInputLimits() const;
	JsonNode makeProgressJson(const JsonNode & executed, const JsonNode & failed, const JsonNode & remaining) const;
	RoutePlan makeRoutePlan(const CGHeroInstance * hero, const int3 & destination, const std::optional<std::string> & expectedRouteID) const;
	void loadConfig();
	void applyConfig(const JsonNode & config, const std::string & sourceLabel);
	void loadScriptMemoryFromLocalState();
	void saveScriptMemoryToLocalState();
	JsonNode makeScriptMemoryLocalState() const;
	std::optional<std::string> getScriptSource();
	std::optional<std::string> loadScriptSource() const;
	std::unique_ptr<scripting::LuaAdventureScriptRunner> makeRunner(const std::string & source) const;
	bool isOpponent(const PlayerColor & owner) const;
	void appendScriptUpdate(const std::string & type, JsonNode data, bool opponent);
	void writeTraceEvent(const std::string & label, const JsonNode & payload);
	void fallbackToNullkiller(const std::string & reason);
};

}
