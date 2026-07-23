/*
 * TestGameServer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "../../server/IGameServer.h"
#include "../../lib/callback/IClient.h"

class CGameHandler;
class CGameState;

class TestGameServer : public IGameServer
{
public:
	void useGameState(const std::shared_ptr<CGameState> & value);

	void setState(EServerState value) override;
	EServerState getState() const override;
	bool isPlayerHost(const PlayerColor &) const override;
	bool hasPlayerAt(PlayerColor, GameConnectionID) const override;
	bool hasBothPlayersAtSameConnection(PlayerColor, PlayerColor) const override;
	void applyPack(CPackForClient & pack) override;
	void sendPack(CPackForClient &, GameConnectionID) override;

private:
	EServerState state{};
	std::shared_ptr<CGameState> gameState;
};

class ServerApplyingClient : public IClient
{
public:
	explicit ServerApplyingClient(const std::shared_ptr<CGameState> & gameState);
	~ServerApplyingClient() override;

	std::optional<BattleAction> makeSurrenderRetreatDecision(
		PlayerColor player,
		const BattleID & battleID,
		const BattleStateInfoForRetreat & battleState) override;
	int sendRequest(const CPackForServer & request, PlayerColor player, bool waitTillRealize) override;

	int getTradePhases() const;
	int getTradeRequests() const;
	int getRecruitmentRequests() const;
	int getGarrisonSwapRequests() const;
	int getUpgradeRequests() const;

private:
	TestGameServer server;
	std::unique_ptr<CGameHandler> gameHandler;
	int lastRequestID = 0;
	int tradePhases = 0;
	int tradeRequests = 0;
	int recruitmentRequests = 0;
	int garrisonSwapRequests = 0;
	int upgradeRequests = 0;
	bool previousRequestWasTrade = false;
};
