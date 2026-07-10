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

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

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
	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	void buildChanged(const CGTownInstance * town, BuildingID buildingID, int what) override;
	void heroMoved(const TryMoveHero & details, bool verbose = true) override;
	void heroCreated(const CGHeroInstance * hero) override;
	void heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town) override;
	void showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QueryID queryID) override;
	void heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QueryID queryID) override;
	void showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID) override;
	void showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID) override;
	void tileRevealed(const FowTilesType & pos) override;
	void newObject(const CGObjectInstance * obj) override;
	void objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator) override;
	void requestSent(const CPackForServer * pack, int requestID) override;
	void requestRealized(PackageApplied * pa) override;

private:
	struct ScriptConfig
	{
		bool reloadScriptEachTurn = true;
		bool trace = false;
		bool experimentalSupportActions = false;
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
		bool transit = false;
		std::vector<int3> requestPath;
	};

	struct PendingRequest
	{
		uint64_t token = 0;
		std::string typeName;
		uint16_t expectedPackType = 0;
		int requestID = -1;
		bool realized = false;
		bool applied = false;
		uint16_t packType = 0;
	};

	struct RequestWaitResult
	{
		bool sent = false;
		bool realized = false;
		bool applied = false;
		int requestID = -1;
		uint16_t packType = 0;
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
	std::mutex requestMutex;
	std::condition_variable requestCv;
	std::optional<PendingRequest> pendingRequest;
	uint64_t nextRequestToken = 0;
	std::mutex queryReplyMutex;
	std::map<int, QueryID> queryReplyRequests;
	std::map<int, bool> earlyQueryReplyResults;
	std::mutex autoAnswerMutex;
	std::map<QueryID, int> pendingAutoAnswers;
	bool scriptActionAutoAnswerMode = false;
	std::vector<std::pair<int32_t, NK2AI::Goals::TTask>> nullkillerTaskHandles;
	int32_t nextNullkillerTaskHandle = 1;

	void makeScriptedTurn();
	bool tryMakeScriptedTurn();
	bool tryMakeImperativeScriptedTurn(scripting::LuaAdventureScriptRunner & runner);
	bool executeScriptAction(const JsonNode & action, JsonNode & actionResult);
	JsonNode makeNullkillerTaskCandidates(const JsonNode & action);
	bool executeNullkillerTaskAction(const JsonNode & action, JsonNode & actionResult);
	RequestWaitResult submitAndWaitForRequest(const std::type_info & requestType, uint16_t expectedPackType, const std::function<void()> & submit);
	JsonNode jsonRequestWaitResult(const RequestWaitResult & request) const;
	bool waitTillFreeForScriptAction(JsonNode & actionResult, const std::string & actionType);
	void answerQueryWithoutGameStateLock(const std::string & description, QueryID queryID, int selection);
	void answerScriptActionDialog(const std::string & queryDescription, const std::string & asyncDescription, QueryID queryID, int selection);
	void answerPendingAutoQueries();
	void setScriptActionAutoAnswerMode(bool active);
	bool isScriptActionAutoAnswerMode();
	JsonNode makeScriptInputState();
	AI::AdventureScriptInput makeAdventureScriptInput(const JsonNode & progress);
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
	void fallbackToNullkiller(const std::string & reason, bool recordFailure = true);
};

}
