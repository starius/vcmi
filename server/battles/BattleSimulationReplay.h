/*
 * BattleSimulationReplay.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationResult.h"

#include "../../lib/constants/EntityIdentifiers.h"

#include <cstdint>
#include <map>

class CGHeroInstance;
struct BattleResult;

namespace BattleSimulation
{
class BattleSimulationReplaySession
{
	int64_t sampleLimitValue = 0;
	BattleSimulationSummary summary;
	std::map<ObjectInstanceID, int32_t> initialHeroMana;

public:
	void reset(int64_t sampleLimit = 0);
	void setSampleLimit(int64_t sampleLimit);
	int64_t sampleLimit() const;
	int64_t recordedSamples() const;
	bool hasRecordedSamples() const;
	BattleSimulationSummary getSummary() const;

	int32_t rememberInitialMana(const CGHeroInstance * hero, int32_t fallback);
	int32_t getReplayInitialMana(const CGHeroInstance * hero, int32_t fallback) const;

	BattleSimulationRecordResult recordResult(const BattleResult & result);
};
}
