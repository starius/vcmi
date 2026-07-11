/*
 * BattleSimulationCache.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationCache.h"

namespace BattleSimulation
{
namespace
{
void combine(size_t & seed, uint64_t value)
{
	seed ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}
}

size_t BattleSimulationCacheKeyHash::operator()(const BattleSimulationCacheKey & key) const
{
	size_t seed = 0;
	combine(seed, key.stateFingerprint);
	combine(seed, static_cast<uint64_t>(key.gameSeed));
	combine(seed, static_cast<uint64_t>(key.player));
	combine(seed, static_cast<uint64_t>(key.heroId));
	combine(seed, static_cast<uint64_t>(key.targetObjectId));
	combine(seed, static_cast<uint64_t>(key.battleType));
	combine(seed, static_cast<uint64_t>(key.turn));
	combine(seed, static_cast<uint64_t>(key.sampleIndex));
	combine(seed, static_cast<uint64_t>(key.sampleCount));
	combine(seed, static_cast<uint64_t>(key.evaluatorVersion));
	return seed;
}

BattleSimulationCacheKey makeCacheKey(const BattleSimulationRequest & request)
{
	return BattleSimulationCacheKey{
		request.stateFingerprint,
		request.seed.gameSeed,
		request.seed.player.getNum(),
		request.seed.heroId.getNum(),
		request.seed.targetObjectId.getNum(),
		request.seed.battleType,
		request.seed.turn,
		request.seed.sampleIndex,
		request.sampleCount,
		request.seed.evaluatorVersion
	};
}

std::optional<BattleSimulationSummary> BattleSimulationCache::find(const BattleSimulationCacheKey & key) const
{
	const auto iter = entries.find(key);
	if(iter == entries.end())
		return std::nullopt;

	return iter->second;
}

void BattleSimulationCache::store(const BattleSimulationCacheKey & key, const BattleSimulationSummary & summary)
{
	entries[key] = summary;
}

void BattleSimulationCache::clear()
{
	entries.clear();
}

size_t BattleSimulationCache::size() const
{
	return entries.size();
}
}
