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

#include "../../lib/networkPacks/PacksForClientBattle.h"

#include <optional>
#include <vector>

class CGameState;

namespace BattleSimulation
{
class IBattleSimulationPackListener
{
public:
	virtual ~IBattleSimulationPackListener() = default;

	virtual void beforeApply(CPackForClient & pack) {}
	virtual void afterApply(CPackForClient & pack) {}
};

class BattleSimulationLocalGameServer final : public IGameServer
{
	CGameState & gameState;
	EServerState state = EServerState::GAMEPLAY;
	std::vector<BattleResult> battleResults;
	std::vector<IBattleSimulationPackListener *> packListeners;

public:
	explicit BattleSimulationLocalGameServer(CGameState & gameState);

	std::optional<BattleResult> lastBattleResult() const;
	std::vector<BattleResult> takeBattleResults();
	void clearBattleResults();
	void addPackListener(IBattleSimulationPackListener & listener);

	void setState(EServerState value) override;
	EServerState getState() const override;
	bool isPlayerHost(const PlayerColor & color) const override;
	bool hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const override;
	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override;
	void applyPack(CPackForClient & pack) override;
	void sendPack(CPackForClient & pack, GameConnectionID connectionID) override;
};
}
