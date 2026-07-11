/*
 * BattleSimulationRequest.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationResult.h"
#include "BattleSimulationSeed.h"
#include "BattleStartInfo.h"

#include <cstdint>

namespace BattleSimulation
{
struct BattleSimulationRequest
{
	BattleStartInfo setup;
	BattleSimulationSeedContext seed;
	BattleSimulationDecisionThresholds thresholds;
	uint64_t stateFingerprint = 0;
	int32_t sampleCount = 0;
};

enum class BattleSimulationResponseStatus
{
	INVALID_REQUEST,
	NOT_AVAILABLE,
	COMPLETE
};

struct BattleSimulationResponse
{
	BattleSimulationResponseStatus status = BattleSimulationResponseStatus::NOT_AVAILABLE;
	BattleSimulationSummary summary;
	BattleSimulationEvaluation evaluation;
};

bool isValidRequest(const BattleSimulationRequest & request);
bool isCompleteResponse(const BattleSimulationRequest & request, const BattleSimulationResponse & response);

BattleSimulationResponse makeResponse(
	const BattleSimulationSummary & summary,
	const BattleSimulationDecisionThresholds & thresholds = {},
	BattleSimulationResponseStatus status = BattleSimulationResponseStatus::COMPLETE);
}
