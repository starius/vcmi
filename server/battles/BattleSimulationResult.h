/*
 * BattleSimulationResult.h, part of VCMI engine
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

namespace BattleSimulation
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
	double attackerWilsonLowerBound(double z = 1.959963984540054) const;
	bool attackerWonAllSamples() const;
	bool defenderWonAllSamples() const;
};

struct BattleSimulationRecordResult
{
	bool shouldReplay = false;
	BattleSimulationSummary summary;
};
}
