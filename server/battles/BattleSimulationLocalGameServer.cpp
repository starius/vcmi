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
	gameState.apply(pack);
}

void BattleSimulationLocalGameServer::sendPack(CPackForClient &, GameConnectionID)
{
}
}
