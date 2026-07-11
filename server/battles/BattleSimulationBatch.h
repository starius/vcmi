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

#include <cstdint>

class CGameHandler;
class CBattleInfoCallback;
class CGHeroInstance;
struct BattleResult;

namespace BattleSimulationBatch
{
bool isEnabled();
bool hasRecordedRows();
int32_t getReplayInitialMana(const CGHeroInstance * hero, int32_t fallback);
bool recordResultAndShouldReplay(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result);
}
