/*
 * BattleSimulationActionCapture.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/battle/BattleAction.h"
#include "../../lib/callback/IClient.h"

#include <optional>

namespace BattleSimulation
{
class BattleSimulationActionCaptureClient final : public IClient
{
public:
	std::optional<BattleAction> makeSurrenderRetreatDecision(
		PlayerColor player,
		const BattleID & battleID,
		const BattleStateInfoForRetreat & battleState) override;

	int sendRequest(const CPackForServer & request, PlayerColor player, bool waitTillRealize) override;

	std::optional<BattleAction> lastAction() const;
	std::optional<BattleAction> takeAction();
	void clearAction();

private:
	std::optional<BattleAction> capturedAction;
};
}
