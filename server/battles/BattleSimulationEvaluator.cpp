/*
 * BattleSimulationEvaluator.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationEvaluator.h"

namespace BattleSimulation
{
BattleSimulationResponse BattleSimulationEvaluator::evaluate(const BattleSimulationRequest & request) const
{
	if(!isValidRequest(request))
		return makeResponse({}, request.thresholds, BattleSimulationResponseStatus::INVALID_REQUEST);

	return makeResponse({}, request.thresholds, BattleSimulationResponseStatus::NOT_AVAILABLE);
}
}
