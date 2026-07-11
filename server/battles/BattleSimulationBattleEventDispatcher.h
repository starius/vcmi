/*
 * BattleSimulationBattleEventDispatcher.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationLocalGameServer.h"

#include "../../lib/battle/BattleAction.h"
#include "../../lib/constants/EntityIdentifiers.h"

#include <map>
#include <memory>
#include <optional>

class CBattleCallback;
class CBattleGameInterface;
class CGameState;

namespace BattleSimulation
{
class BattleSimulationBattleEventDispatcher final : public IBattleSimulationPackListener
{
public:
	explicit BattleSimulationBattleEventDispatcher(const CGameState & gameState);

	void registerBattleCallback(PlayerColor player, std::shared_ptr<CBattleCallback> callback);
	void registerBattleInterface(PlayerColor player, std::shared_ptr<CBattleGameInterface> battleInterface);

	void beforeApply(CPackForClient & pack) override;
	void afterApply(CPackForClient & pack) override;

private:
	const CGameState & gameState;
	std::map<PlayerColor, std::shared_ptr<CBattleCallback>> battleCallbacks;
	std::map<PlayerColor, std::shared_ptr<CBattleGameInterface>> battleInterfaces;
	std::optional<BattleAction> currentBattleAction;
};
}
