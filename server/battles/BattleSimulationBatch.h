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

#include "../../lib/battle/BattleSide.h"

#include <cstdint>

class CGameHandler;
class CBattleInfoCallback;
class CGHeroInstance;
struct BattleResult;

namespace BattleSimulationBatch
{
struct BattleSimulationSummary
{
	int64_t rows = 0;
	int64_t attackerWins = 0;
	int64_t defenderWins = 0;
	int64_t noWinner = 0;
	int64_t otherWinner = 0;

	void recordWinner(BattleSide winner);
	bool hasSamples() const;
	double attackerWinRate() const;
	bool attackerWonAllSamples() const;
	bool defenderWonAllSamples() const;
};

struct BattleSimulationRecordResult
{
	bool shouldReplay = false;
	BattleSimulationSummary summary;
};

bool isEnabled();
bool hasRecordedRows();
BattleSimulationSummary getSummary();
int32_t getReplayInitialMana(const CGHeroInstance * hero, int32_t fallback);
BattleSimulationRecordResult recordResult(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result);
bool recordResultAndShouldReplay(CGameHandler & gameHandler, const CBattleInfoCallback & battle, const BattleResult & result);
}
