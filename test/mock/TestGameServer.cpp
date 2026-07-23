/*
 * TestGameServer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "TestGameServer.h"

#include "../../server/CGameHandler.h"

#include "../../lib/gameState/CGameState.h"
#include "../../lib/networkPacks/PacksForServer.h"

void TestGameServer::useGameState(const std::shared_ptr<CGameState> & value)
{
	gameState = value;
}

void TestGameServer::setState(EServerState value)
{
	state = value;
}

EServerState TestGameServer::getState() const
{
	return state;
}

bool TestGameServer::isPlayerHost(const PlayerColor &) const
{
	return false;
}

bool TestGameServer::hasPlayerAt(PlayerColor, GameConnectionID) const
{
	return false;
}

bool TestGameServer::hasBothPlayersAtSameConnection(PlayerColor, PlayerColor) const
{
	return false;
}

void TestGameServer::applyPack(CPackForClient & pack)
{
	if(gameState)
		gameState->apply(pack);
}

void TestGameServer::sendPack(CPackForClient &, GameConnectionID)
{
}

ServerApplyingClient::ServerApplyingClient(const std::shared_ptr<CGameState> & gameState)
{
	server.useGameState(gameState);
	gameHandler = std::make_unique<CGameHandler>(server, gameState);
}

ServerApplyingClient::~ServerApplyingClient() = default;

std::optional<BattleAction> ServerApplyingClient::makeSurrenderRetreatDecision(
	PlayerColor,
	const BattleID &,
	const BattleStateInfoForRetreat &)
{
	return std::nullopt;
}

int ServerApplyingClient::sendRequest(const CPackForServer & request, PlayerColor player, bool)
{
	const bool isTrade = dynamic_cast<const TradeOnMarketplace *>(&request) != nullptr;
	if(isTrade && !previousRequestWasTrade)
		++tradePhases;
	previousRequestWasTrade = isTrade;

	if(const auto * trade = dynamic_cast<const TradeOnMarketplace *>(&request))
	{
		const auto * market = gameHandler->gameState().getMarket(trade->marketId);
		EXPECT_NE(market, nullptr);
		if(market)
		{
			for(size_t index = 0; index < trade->r1.size(); ++index)
			{
				gameHandler->tradeResources(
					market,
					trade->val[index],
					player,
					trade->r1[index].as<GameResID>(),
					trade->r2[index].as<GameResID>());
			}
		}
		++tradeRequests;
	}
	else if(const auto * recruit = dynamic_cast<const RecruitCreatures *>(&request))
	{
		gameHandler->recruitCreatures(
			recruit->tid,
			recruit->dst,
			recruit->crid,
			recruit->amount,
			recruit->level,
			player);
		++recruitmentRequests;
	}
	else if(const auto * swap = dynamic_cast<const GarrisonHeroSwap *>(&request))
	{
		gameHandler->garrisonSwap(swap->tid);
		++garrisonSwapRequests;
	}
	else if(const auto * upgrade = dynamic_cast<const UpgradeCreature *>(&request))
	{
		gameHandler->upgradeCreature(upgrade->id, upgrade->pos, upgrade->cid);
		++upgradeRequests;
	}
	else if(dynamic_cast<const EndTurn *>(&request) == nullptr)
	{
		ADD_FAILURE() << "Unsupported server request in test client: " << typeid(request).name();
	}

	return ++lastRequestID;
}

int ServerApplyingClient::getTradePhases() const
{
	return tradePhases;
}

int ServerApplyingClient::getTradeRequests() const
{
	return tradeRequests;
}

int ServerApplyingClient::getRecruitmentRequests() const
{
	return recruitmentRequests;
}

int ServerApplyingClient::getGarrisonSwapRequests() const
{
	return garrisonSwapRequests;
}

int ServerApplyingClient::getUpgradeRequests() const
{
	return upgradeRequests;
}
