/*
 * BattleSimulationResult.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationResult.h"

#include <algorithm>
#include <cmath>

namespace BattleSimulation
{
void BattleSimulationSummary::recordWinner(BattleSide winner)
{
	++rows;

	switch(winner)
	{
		case BattleSide::ATTACKER:
			++attackerWins;
			break;
		case BattleSide::DEFENDER:
			++defenderWins;
			break;
		case BattleSide::NONE:
			++noWinner;
			break;
		default:
			++otherWinner;
			break;
	}
}

bool BattleSimulationSummary::hasSamples() const
{
	return rows > 0;
}

double BattleSimulationSummary::attackerWinRate() const
{
	return hasSamples() ? static_cast<double>(attackerWins) / rows : 0.0;
}

double BattleSimulationSummary::attackerWilsonLowerBound(double z) const
{
	if(!hasSamples())
		return 0.0;

	const double sampleCount = static_cast<double>(rows);
	const double probability = attackerWinRate();
	const double zSquared = z * z;
	const double denominator = 1.0 + zSquared / sampleCount;
	const double center = probability + zSquared / (2.0 * sampleCount);
	const double margin = z * std::sqrt((probability * (1.0 - probability) + zSquared / (4.0 * sampleCount)) / sampleCount);

	return std::clamp((center - margin) / denominator, 0.0, 1.0);
}

bool BattleSimulationSummary::attackerWonAllSamples() const
{
	return hasSamples() && attackerWins == rows;
}

bool BattleSimulationSummary::defenderWonAllSamples() const
{
	return hasSamples() && defenderWins == rows;
}

BattleSimulationEvaluation evaluateSummary(
	const BattleSimulationSummary & summary,
	const BattleSimulationDecisionThresholds & thresholds)
{
	BattleSimulationEvaluation result;
	result.sampleCount = summary.rows;
	result.attackerWinProbability = summary.attackerWinRate();
	result.attackerWilsonLowerBound = summary.attackerWilsonLowerBound(thresholds.wilsonZ);
	result.attackerLikelyWins = result.attackerWinProbability >= thresholds.likelyWinProbability;
	result.attackerProbabilitySafe = result.attackerWinProbability >= thresholds.safeWinProbability;
	result.attackerAllWinsSafe = summary.attackerWonAllSamples();
	result.attackerWilsonSafe = result.attackerWilsonLowerBound >= thresholds.wilsonSafeProbability;
	result.defenderWonAllSamples = summary.defenderWonAllSamples();
	return result;
}
}
