/*
 * BattleSimulationSeed.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationSeed.h"

namespace BattleSimulation
{
namespace
{
uint64_t mix(uint64_t value)
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

void combine(uint64_t & seed, uint64_t value)
{
	seed ^= mix(value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}
}

uint64_t deriveSampleSeed(const BattleSimulationSeedContext & context)
{
	uint64_t seed = 0xcbf29ce484222325ULL;
	combine(seed, static_cast<uint64_t>(context.gameSeed));
	combine(seed, static_cast<uint64_t>(context.player.getNum()));
	combine(seed, static_cast<uint64_t>(context.heroId.getNum()));
	combine(seed, static_cast<uint64_t>(context.targetObjectId.getNum()));
	combine(seed, static_cast<uint64_t>(context.battleType));
	combine(seed, static_cast<uint64_t>(context.turn));
	combine(seed, static_cast<uint64_t>(context.sampleIndex));
	combine(seed, static_cast<uint64_t>(context.evaluatorVersion));
	return seed;
}
}
