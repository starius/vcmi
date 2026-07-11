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

#include "BattleSimulationRunner.h"

namespace BattleSimulation
{
namespace
{
void addSummary(BattleSimulationSummary & target, const BattleSimulationSummary & source)
{
	target.rows += source.rows;
	target.attackerWins += source.attackerWins;
	target.defenderWins += source.defenderWins;
	target.noWinner += source.noWinner;
	target.otherWinner += source.otherWinner;
}
}

BattleSimulationResponse BattleSimulationEvaluator::evaluate(const BattleSimulationRequest & request) const
{
	if(!isValidRequest(request))
		return makeResponse({}, request.thresholds, BattleSimulationResponseStatus::INVALID_REQUEST);

	const auto cached = cache.find(makeCacheKey(request));
	if(cached)
		return makeResponse(*cached, request.thresholds, BattleSimulationResponseStatus::COMPLETE);

	if(request.sampleCount > 1)
	{
		BattleSimulationSummary summary;
		auto sampleRequest = request;
		sampleRequest.sampleCount = 1;
		bool allSamplesCached = true;

		for(int32_t index = 0; index < request.sampleCount; ++index)
		{
			sampleRequest.seed = sampleSeedContext(request.seed, request.seed.sampleIndex + index);
			const auto sample = cache.find(makeCacheKey(sampleRequest));
			if(!sample || !sample->hasSamples())
			{
				allSamplesCached = false;
				break;
			}

			addSummary(summary, *sample);
		}

		if(allSamplesCached)
		{
			cache.store(makeCacheKey(request), summary);
			return makeResponse(summary, request.thresholds, BattleSimulationResponseStatus::COMPLETE);
		}
	}

	if(runner)
	{
		const auto summary = runner->run(request);
		if(summary && summary->rows >= request.sampleCount)
		{
			cache.store(makeCacheKey(request), *summary);
			return makeResponse(*summary, request.thresholds, BattleSimulationResponseStatus::COMPLETE);
		}
	}

	return makeResponse({}, request.thresholds, BattleSimulationResponseStatus::NOT_AVAILABLE);
}

void BattleSimulationEvaluator::setRunner(std::shared_ptr<IBattleSimulationRunner> newRunner)
{
	runner = std::move(newRunner);
}

void BattleSimulationEvaluator::storeCachedSummary(const BattleSimulationRequest & request, const BattleSimulationSummary & summary)
{
	if(isValidRequest(request) && summary.rows >= request.sampleCount)
		cache.store(makeCacheKey(request), summary);
}

void BattleSimulationEvaluator::clearCache()
{
	cache.clear();
}

size_t BattleSimulationEvaluator::cacheSize() const
{
	return cache.size();
}
}
