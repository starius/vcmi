/*
 * BattleSimulationLocalGameServer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationLocalGameServer.h"

#include "../../lib/gameState/CGameState.h"

namespace BattleSimulation
{
BattleSimulationLocalGameServer::BattleSimulationLocalGameServer(CGameState & gameState)
	: gameState(gameState)
{
}

std::optional<BattleResult> BattleSimulationLocalGameServer::lastBattleResult() const
{
	if(battleResults.empty())
		return std::nullopt;

	return battleResults.back();
}

std::vector<BattleResult> BattleSimulationLocalGameServer::takeBattleResults()
{
	return std::move(battleResults);
}

void BattleSimulationLocalGameServer::clearBattleResults()
{
	battleResults.clear();
}

void BattleSimulationLocalGameServer::addPackListener(IBattleSimulationPackListener & listener)
{
	packListeners.push_back(&listener);
}

void BattleSimulationLocalGameServer::setState(EServerState value)
{
	state = value;
}

EServerState BattleSimulationLocalGameServer::getState() const
{
	return state;
}

bool BattleSimulationLocalGameServer::isPlayerHost(const PlayerColor &) const
{
	return false;
}

bool BattleSimulationLocalGameServer::hasPlayerAt(PlayerColor, GameConnectionID) const
{
	return false;
}

bool BattleSimulationLocalGameServer::hasBothPlayersAtSameConnection(PlayerColor, PlayerColor) const
{
	return false;
}

void BattleSimulationLocalGameServer::applyPack(CPackForClient & pack)
{
	for(auto * listener : packListeners)
		listener->beforeApply(pack);

	if(const auto * battleResult = dynamic_cast<const BattleResult *>(&pack))
		battleResults.push_back(*battleResult);

	gameState.apply(pack);

	for(auto * listener : packListeners)
		listener->afterApply(pack);
}

void BattleSimulationLocalGameServer::sendPack(CPackForClient &, GameConnectionID)
{
}
}
