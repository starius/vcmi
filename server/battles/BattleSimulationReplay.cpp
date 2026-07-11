/*
 * BattleSimulationReplay.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationReplay.h"

#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"

namespace BattleSimulation
{
void BattleSimulationReplaySession::reset(int64_t sampleLimit)
{
	sampleLimitValue = sampleLimit;
	summary = {};
	initialHeroMana.clear();
}

void BattleSimulationReplaySession::setSampleLimit(int64_t sampleLimit)
{
	sampleLimitValue = sampleLimit;
}

int64_t BattleSimulationReplaySession::sampleLimit() const
{
	return sampleLimitValue;
}

int64_t BattleSimulationReplaySession::recordedSamples() const
{
	return summary.rows;
}

bool BattleSimulationReplaySession::hasRecordedSamples() const
{
	return summary.hasSamples();
}

BattleSimulationSummary BattleSimulationReplaySession::getSummary() const
{
	return summary;
}

int32_t BattleSimulationReplaySession::rememberInitialMana(const CGHeroInstance * hero, int32_t fallback)
{
	if(!hero)
		return fallback;

	const auto result = initialHeroMana.try_emplace(hero->id, fallback);
	return result.first->second;
}

int32_t BattleSimulationReplaySession::getReplayInitialMana(const CGHeroInstance * hero, int32_t fallback) const
{
	if(!hero)
		return fallback;

	const auto iter = initialHeroMana.find(hero->id);
	if(iter == initialHeroMana.end())
		return fallback;

	return iter->second;
}

BattleSimulationRecordResult BattleSimulationReplaySession::recordResult(const BattleResult & result)
{
	summary.recordWinner(result.winner);
	return BattleSimulationRecordResult{
		sampleLimitValue > 0 && summary.rows < sampleLimitValue,
		summary
	};
}
}
