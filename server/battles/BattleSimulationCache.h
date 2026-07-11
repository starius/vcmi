/*
 * BattleSimulationCache.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationRequest.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace BattleSimulation
{
struct BattleSimulationCacheKey
{
	uint64_t stateFingerprint = 0;
	int64_t gameSeed = 0;
	int32_t player = 0;
	int32_t heroId = 0;
	int32_t targetObjectId = 0;
	int32_t battleType = 0;
	int32_t turn = 0;
	int32_t sampleIndex = 0;
	int32_t sampleCount = 0;
	int32_t evaluatorVersion = 0;

	bool operator==(const BattleSimulationCacheKey & other) const = default;
};

struct BattleSimulationCacheKeyHash
{
	size_t operator()(const BattleSimulationCacheKey & key) const;
};

BattleSimulationCacheKey makeCacheKey(const BattleSimulationRequest & request);

class BattleSimulationCache
{
public:
	std::optional<BattleSimulationSummary> find(const BattleSimulationCacheKey & key) const;
	void store(const BattleSimulationCacheKey & key, const BattleSimulationSummary & summary);
	void clear();
	size_t size() const;

private:
	std::unordered_map<BattleSimulationCacheKey, BattleSimulationSummary, BattleSimulationCacheKeyHash> entries;
};
}
