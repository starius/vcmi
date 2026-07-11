/*
 * BattleSimulationActionCapture.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationActionCapture.h"

#include "../../lib/networkPacks/PacksForServer.h"

#include <utility>

namespace BattleSimulation
{
std::optional<BattleAction> BattleSimulationActionCaptureClient::makeSurrenderRetreatDecision(
	PlayerColor,
	const BattleID &,
	const BattleStateInfoForRetreat &)
{
	return std::nullopt;
}

int BattleSimulationActionCaptureClient::sendRequest(const CPackForServer & request, PlayerColor, bool)
{
	if(const auto * action = dynamic_cast<const MakeAction *>(&request))
		capturedAction = action->ba;

	return 0;
}

std::optional<BattleAction> BattleSimulationActionCaptureClient::lastAction() const
{
	return capturedAction;
}

std::optional<BattleAction> BattleSimulationActionCaptureClient::takeAction()
{
	return std::exchange(capturedAction, std::nullopt);
}

void BattleSimulationActionCaptureClient::clearAction()
{
	capturedAction.reset();
}
}
