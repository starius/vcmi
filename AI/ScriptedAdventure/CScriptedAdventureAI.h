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
#include <chrono>
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
	void availableCreaturesChanged(const CGDwelling * town) override;
	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	void buildChanged(const CGTownInstance * town, BuildingID buildingID, int what) override;
	void heroMoved(const TryMoveHero & details, bool verbose = true) override;
	void centerView(int3 pos, int focusTime) override;
	void heroInGarrisonChange(const CGTownInstance * town) override;
	void tileHidden(const FowTilesType & pos) override;
	void bulkArtMovementStart(size_t totalNumOfArts, size_t possibleAssemblyNumOfArts) override;
	void artifactMoved(const ArtifactLocation & src, const ArtifactLocation & dst) override;
	void artifactPut(const ArtifactLocation & location) override;
	void artifactRemoved(const ArtifactLocation & location) override;
	void heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start) override;
	void heroCreated(const CGHeroInstance * hero) override;
	void heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town) override;
	void heroExperienceChanged(const CGHeroInstance * hero, si64 val) override;
	void heroPrimarySkillChanged(const CGHeroInstance * hero, PrimarySkill which, si64 val) override;
	void heroMovePointsChanged(const CGHeroInstance * hero) override;
	void garrisonsChanged(ObjectInstanceID id1, ObjectInstanceID id2) override;
	void showPuzzleMap() override;
	void viewWorldMap() override;
	void showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QueryID queryID) override;
	void showThievesGuildWindow(const CGObjectInstance * obj) override;
	void showShipyardDialog(const IShipyard * obj) override;
	void playerBonusChanged(const Bonus & bonus, bool gain) override;
	void advmapSpellCast(const CGHeroInstance * caster, SpellID spellID) override;
	void heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QueryID queryID) override;
	void showHillFortWindow(const CGObjectInstance * object, const CGHeroInstance * visitor) override;
	void showInfoDialog(EInfoWindowMode type, const std::string & text, const std::vector<Component> & components, int soundID) override;
	void receivedResource() override;
	void showQuestLog() override;
	void showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID) override;
	void heroManaPointsChanged(const CGHeroInstance * hero) override;
	void heroSecondarySkillChanged(const CGHeroInstance * hero, int which, int val) override;
	void heroBonusChanged(const CGHeroInstance * hero, const Bonus & bonus, bool gain) override;
	void showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID) override;
	void availableArtifactsChanged(const CGBlackMarket * bm = nullptr) override;
	void askToAssembleArtifact(const ArtifactLocation & destination) override;
	void artifactAssembled(const ArtifactLocation & location) override;
	void artifactDisassembled(const ArtifactLocation & location) override;
	void responseStatistic(StatisticDataSet & statistic) override;
	void tileRevealed(const FowTilesType & pos) override;
	void newObject(const CGObjectInstance * obj) override;
	void objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator) override;
	void objectRemovedAfter() override;
	void playerBlocked(int reason, bool start) override;
	void gameOver(PlayerColor player, const EVictoryLossCheckResult & victoryLossCheckResult) override;
	void playerStartsTurn(PlayerColor player) override;
	void playerEndsTurn(PlayerColor player) override;
	void battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed) override;
	void battleEnd(const BattleID & battleID, const BattleResult * br, QueryID queryID) override;
	std::optional<BattleAction> makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState) override;
	void battleResultsApplied() override;
	void battleEnded() override;
	void showWorldViewEx(const std::vector<ObjectPosInfo> & objectPositions, bool showTerrain) override;
	void beforeObjectPropertyChanged(const SetObjectProperty * sop) override;
	void objectPropertyChanged(const SetObjectProperty * sop) override;
	void setColorScheme(ColorScheme scheme) override;
	void requestSent(const CPackForServer * pack, int requestID) override;
	void requestRealized(PackageApplied * pa) override;

private:
	struct ScriptConfig
	{
		bool reloadScriptEachTurn = false;
		bool trace = false;
		bool experimentalSupportActions = false;
		size_t maxConsecutiveFailures = 3;
		int disableTurnsAfterFailures = 3;
		std::chrono::milliseconds actionWaitTimeout = std::chrono::seconds(30);
		std::chrono::milliseconds battleActionWaitTimeout = std::chrono::seconds(60);
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

	struct NullkillerTaskHandle
	{
		int32_t id = 0;
		NK2AI::ScriptTaskCandidate candidate;
		JsonNode taskJson;
	};

	JsonNode scriptMemory;
	std::string scriptPath;
	AI::AdventureScriptLimits limits;
	size_t maxScriptCallsPerTurn = 8;
	ScriptConfig scriptConfig;
	std::optional<std::string> cachedScriptSource;
	std::unique_ptr<scripting::LuaAdventureScriptRunner> cachedRunner;
	std::unique_ptr<scripting::LuaAdventureScriptRunner> cachedBattleCallbackRunner;
	std::mutex battleCallbackRunnerMutex;
	size_t traceSequence = 0;
	size_t consecutiveScriptFailures = 0;
	int disabledUntilDay = 0;
	bool failureRecordedThisTurn = false;
	std::string lastPersistedScriptState;
	mutable std::mutex scriptUpdateMutex;
	std::deque<JsonNode> scriptUpdateJournal;
	uint64_t scriptUpdateRevision = 0;
	size_t maxScriptUpdateJournal = 256;
	std::mutex scriptedTurnMutex;
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
	mutable std::mutex scriptQueryMutex;
	std::map<QueryID, JsonNode> scriptQueries;
	std::vector<NullkillerTaskHandle> nullkillerTaskHandles;
	int32_t nextNullkillerTaskHandle = 1;
	int32_t nextScriptDecisionID = -1000;

	void makeScriptedTurn();
	bool tryMakeScriptedTurn();
	bool tryMakeImperativeScriptedTurn(scripting::LuaAdventureScriptRunner & runner);
	bool executeScriptAction(const JsonNode & action, JsonNode & actionResult);
	JsonNode executeScriptInspect(const JsonNode & request);
	JsonNode makeNullkillerTaskCandidates(const JsonNode & action, bool executionMode = false);
	bool executeNullkillerTaskAction(const JsonNode & action, JsonNode & actionResult);
	bool executeNullkillerQueryAction(const JsonNode & action, JsonNode & actionResult);
	bool executeNullkillerDefendTownAction(const JsonNode & action, JsonNode & actionResult);
	bool executeNullkillerStepAction(const JsonNode & action, JsonNode & actionResult);
	bool executeNullkillerPassAction(const JsonNode & action, JsonNode & actionResult);
	bool executeNullkillerTurnSliceAction(const JsonNode & action, JsonNode & actionResult);
	RequestWaitResult submitAndWaitForRequest(const std::type_info & requestType, uint16_t expectedPackType, const std::function<void()> & submit);
	JsonNode jsonRequestWaitResult(const RequestWaitResult & request) const;
	bool waitTillFreeForScriptAction(JsonNode & actionResult, const std::string & actionType);
	void answerQueryWithoutGameStateLock(const std::string & description, QueryID queryID, int selection);
	void pauseForScriptActionQuery(const std::string & queryDescription, QueryID queryID);
	size_t answerPendingAutoQueries();
	void setScriptActionAutoAnswerMode(bool active);
	bool isScriptActionAutoAnswerMode();
	void recordScriptQuery(QueryID queryID, const std::string & type, JsonNode data);
	std::optional<JsonNode> getScriptQuery(QueryID queryID) const;
	void removeScriptQuery(QueryID queryID);
	void removeArtifactAssemblyPrompts(ObjectInstanceID heroID, ArtifactPosition slot);
	JsonNode makeScriptQueries() const;
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
