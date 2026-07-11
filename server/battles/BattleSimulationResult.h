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

struct BattleSimulationDecisionThresholds
{
	double likelyWinProbability = 0.5;
	double safeWinProbability = 0.95;
	double wilsonZ = 1.2815515655446004;
	double wilsonSafeProbability = 0.60;
};

struct BattleSimulationEvaluation
{
	int64_t sampleCount = 0;
	double attackerWinProbability = 0.0;
	double attackerWilsonLowerBound = 0.0;
	bool attackerLikelyWins = false;
	bool attackerProbabilitySafe = false;
	bool attackerAllWinsSafe = false;
	bool attackerWilsonSafe = false;
	bool defenderWonAllSamples = false;
};

BattleSimulationEvaluation evaluateSummary(
	const BattleSimulationSummary & summary,
	const BattleSimulationDecisionThresholds & thresholds = {});
}
