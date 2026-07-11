/*
 * BattleSimulationSeed.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/constants/EntityIdentifiers.h"

#include <cstdint>

namespace BattleSimulation
{
struct BattleSimulationSeedContext
{
	int64_t gameSeed = 0;
	PlayerColor player = PlayerColor::CANNOT_DETERMINE;
	ObjectInstanceID heroId = ObjectInstanceID::NONE;
	ObjectInstanceID targetObjectId = ObjectInstanceID::NONE;
	int32_t battleType = 0;
	int32_t turn = 0;
	int32_t sampleIndex = 0;
	int32_t evaluatorVersion = 1;
};

uint64_t deriveSampleSeed(const BattleSimulationSeedContext & context);
}
