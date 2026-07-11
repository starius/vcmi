/*
 * BattleSimulationIsolatedState.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleStartInfo.h"

#include <memory>
#include <optional>

class CGameState;

namespace BattleSimulation
{
std::shared_ptr<CGameState> cloneGameStateForSimulation(const CGameState & source);

std::optional<BattleStartInfo> remapBattleStartInfo(
	const CGameState & gameState,
	const BattleStartInfo & source);
}
