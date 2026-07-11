/*
 * BattleSimulationRunner.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationRequest.h"

#include <optional>

namespace BattleSimulation
{
class IBattleSimulationRunner
{
public:
	virtual ~IBattleSimulationRunner() = default;

	virtual std::optional<BattleSimulationSummary> run(const BattleSimulationRequest & request) = 0;
};
}
