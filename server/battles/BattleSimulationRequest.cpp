/*
 * BattleSimulationRequest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationRequest.h"
#include "BattleSimulationFingerprint.h"

namespace BattleSimulation
{
bool isValidRequest(const BattleSimulationRequest & request)
{
	return request.sampleCount > 0
		&& isValidStateFingerprint(request.stateFingerprint)
		&& request.setup.armies[BattleSide::ATTACKER] != nullptr
		&& request.setup.armies[BattleSide::DEFENDER] != nullptr
		&& request.seed.sampleIndex >= 0
		&& request.seed.evaluatorVersion > 0
		&& request.thresholds.likelyWinProbability >= 0.0
		&& request.thresholds.likelyWinProbability <= 1.0
		&& request.thresholds.safeWinProbability >= 0.0
		&& request.thresholds.safeWinProbability <= 1.0
		&& request.thresholds.wilsonZ >= 0.0
		&& request.thresholds.wilsonSafeProbability >= 0.0
		&& request.thresholds.wilsonSafeProbability <= 1.0;
}

bool isCompleteResponse(const BattleSimulationRequest & request, const BattleSimulationResponse & response)
{
	return isValidRequest(request)
		&& response.status == BattleSimulationResponseStatus::COMPLETE
		&& response.summary.rows >= request.sampleCount;
}

BattleSimulationResponse makeResponse(
	const BattleSimulationSummary & summary,
	const BattleSimulationDecisionThresholds & thresholds,
	BattleSimulationResponseStatus status)
{
	return BattleSimulationResponse{
		status,
		summary,
		evaluateSummary(summary, thresholds)
	};
}
}
