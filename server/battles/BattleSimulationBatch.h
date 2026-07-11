/*
 * BattleSimulationBatch.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationResult.h"

#include <cstdint>

class CGameHandler;
class CBattleInfoCallback;
class CGHeroInstance;
struct BattleResult;

namespace BattleSimulationBatch
{
using BattleSimulation::BattleSimulationRecordResult;
using BattleSimulation::BattleSimulationSummary;

bool isEnabled();
bool hasRecordedRows();
BattleSimulationSummary getSummary();
int32_t getReplayInitialMana(const CGHeroInstance * hero, int32_t fallback);
BattleSimulationRecordResult recordResult(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result);
bool recordResultAndShouldReplay(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result);
}
