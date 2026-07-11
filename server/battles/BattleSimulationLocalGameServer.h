/*
 * BattleSimulationLocalGameServer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../IGameServer.h"

class CGameState;

namespace BattleSimulation
{
class BattleSimulationLocalGameServer final : public IGameServer
{
	CGameState & gameState;
	EServerState state = EServerState::GAMEPLAY;

public:
	explicit BattleSimulationLocalGameServer(CGameState & gameState);

	void setState(EServerState value) override;
	EServerState getState() const override;
	bool isPlayerHost(const PlayerColor & color) const override;
	bool hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const override;
	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override;
	void applyPack(CPackForClient & pack) override;
	void sendPack(CPackForClient & pack, GameConnectionID connectionID) override;
};
}
