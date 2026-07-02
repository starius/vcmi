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

#include <memory>
#include <mutex>
#include <optional>

class McpHttpServer;

class CMcpPlayerInterface : public CGlobalAI
{
	struct PendingQuery
	{
		QueryID id;
		std::string type;
		JsonNode data;
	};

	std::shared_ptr<CCallback> cb;
	Mcp::Protocol protocol;
	std::unique_ptr<McpHttpServer> httpServer;

	mutable std::mutex interfaceMutex;
	bool turnActive = false;
	std::optional<PendingQuery> pendingQuery;
	BattleID activeBattleID = BattleID::NONE;
	const CStack * activeStackToMove = nullptr;
	BattleID activeTacticsBattleID = BattleID::NONE;

	void configureProtocol();
	void startHttpServer();

	JsonNode makeStateJson() const;
	std::string makeStateText() const;
	JsonNode makePendingQueryJson() const;

	Mcp::Protocol::ToolResult makeOkResult(const std::string & message = "ok") const;
	Mcp::Protocol::ToolResult makeJsonResult(const JsonNode & node) const;
	Mcp::Protocol::ToolResult makeToolError(const std::string & message) const;

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
	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	std::optional<BattleAction> makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState) override;
};
