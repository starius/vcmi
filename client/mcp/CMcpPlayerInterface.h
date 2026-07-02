/*
 * CMcpPlayerInterface.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/callback/CGlobalAI.h"
#include "McpProtocol.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeinfo>
#include <vector>

class McpHttpServer;

class CMcpPlayerInterface : public CGlobalAI
{
	struct PendingQuery
	{
		QueryID id;
		std::string type;
		JsonNode data;
	};

	struct RequestWaitResult
	{
		bool sent = false;
		bool realized = false;
		bool applied = false;
		int requestID = -1;
		uint16_t packType = 0;
	};

	struct RoutePlan
	{
		bool ok = false;
		std::string error;
		std::string routeID;
		int3 destination;
		EPathfindingLayer layer = EPathfindingLayer::AUTO;
		bool transit = false;
		std::vector<int3> requestPath;
		JsonNode pathPreview;
	};

	struct PendingRequest
	{
		uint64_t token = 0;
		std::string typeName;
		int requestID = -1;
		uint16_t packType = 0;
		bool realized = false;
		bool applied = false;
	};

	std::shared_ptr<CCallback> cb;
	Mcp::Protocol protocol;
	std::unique_ptr<McpHttpServer> httpServer;

	mutable std::mutex actionMutex;
	mutable std::mutex interfaceMutex;
	mutable std::mutex requestMutex;
	mutable std::mutex traceMutex;
	mutable std::mutex updatesMutex;
	std::condition_variable requestCv;
	bool turnActive = false;
	std::optional<PendingQuery> pendingQuery;
	std::optional<PendingRequest> pendingRequest;
	BattleID activeBattleID = BattleID::NONE;
	const CStack * activeStackToMove = nullptr;
	BattleID activeTacticsBattleID = BattleID::NONE;
	std::string tracePath;
	uint64_t nextRequestToken = 0;
	mutable uint64_t traceSequence = 0;
	mutable uint64_t stateRevision = 0;
	mutable std::deque<JsonNode> updateJournal;

	void configureProtocol();
	void startHttpServer();

	JsonNode makeStateJson() const;
	JsonNode makeStateJson(const JsonNode & arguments) const;
	JsonNode makeActionSpaceJson() const;
	JsonNode makeVisibleMapJson(const JsonNode & arguments) const;
	JsonNode makeMovementOptionsJson(const JsonNode & arguments) const;
	JsonNode makeReachableJson(const JsonNode & arguments) const;
	JsonNode makeBattleStateJson() const;
	JsonNode makeUpdatesJson(uint64_t sinceRevision, size_t maxUpdates) const;
	std::string makeAgentGuideText() const;
	std::string makeStateText() const;
	JsonNode makePendingQueryJson() const;
	RoutePlan makeRoutePlan(const CGHeroInstance * hero, const int3 & destination, std::optional<std::string> expectedRouteID) const;
	Mcp::Protocol::ToolResult executePlan(const JsonNode & arguments);

	Mcp::Protocol::ToolResult makeOkResult(const std::string & message = "ok") const;
	Mcp::Protocol::ToolResult makeJsonResult(const JsonNode & node) const;
	Mcp::Protocol::ToolResult makeToolError(const std::string & message) const;
	Mcp::Protocol::ToolResult traceToolResult(const std::string & toolName, const JsonNode & arguments, Mcp::Protocol::ToolResult result) const;
	void appendTraceEvent(JsonNode event) const;
	void appendStateUpdate(const std::string & type, JsonNode data) const;
	RequestWaitResult submitAndWaitForRequest(const std::type_info & requestType, const std::function<void()> & submit);
	JsonNode jsonRequestWaitResult(const RequestWaitResult & request) const;
	void performAutoBattleAction(const BattleID & battleID, const CStack * stack);

	void setPendingQuery(QueryID queryID, const std::string & type, JsonNode data);
	void clearPendingQuery(QueryID queryID);

public:
	CMcpPlayerInterface();
	~CMcpPlayerInterface() override;

	void initGameInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CCallback> CB) override;
	void finish() override;

	void yourTurn(QueryID queryID) override;
	void yourTacticPhase(const BattleID & battleID, int distance) override;
	void activeStack(const BattleID & battleID, const CStack * stack) override;
	void battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed) override;
	void battleEnd(const BattleID & battleID, const BattleResult * br, QueryID queryID) override;
	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	std::optional<BattleAction> makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState) override;
	void buildChanged(const CGTownInstance * town, BuildingID buildingID, int what) override;
	void heroMoved(const TryMoveHero & details, bool verbose = true) override;
	void heroCreated(const CGHeroInstance * hero) override;
	void heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start) override;
	void heroManaPointsChanged(const CGHeroInstance * hero) override;
	void heroMovePointsChanged(const CGHeroInstance * hero) override;
	void tileHidden(const FowTilesType & pos) override;
	void tileRevealed(const FowTilesType & pos) override;
	void availableCreaturesChanged(const CGDwelling * town) override;
	void beforeObjectPropertyChanged(const SetObjectProperty * sop) override;
	void objectPropertyChanged(const SetObjectProperty * sop) override;
	void objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator) override;
	void playerStartsTurn(PlayerColor player) override;
	void playerEndsTurn(PlayerColor player) override;
	void requestSent(const CPackForServer * pack, int requestID) override;
	void requestRealized(PackageApplied * pa) override;
};
