/*
 * BattleSimulationEvaluator.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationCache.h"
#include "BattleSimulationRequest.h"

#include <cstddef>

namespace BattleSimulation
{
class BattleSimulationEvaluator
{
public:
	BattleSimulationResponse evaluate(const BattleSimulationRequest & request) const;

	void storeCachedSummary(const BattleSimulationRequest & request, const BattleSimulationSummary & summary);
	void clearCache();
	size_t cacheSize() const;

private:
	mutable BattleSimulationCache cache;
};
}
