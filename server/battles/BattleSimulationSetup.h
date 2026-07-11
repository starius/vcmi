/*
 * BattleSimulationSetup.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationRequest.h"
#include "BattleStartInfo.h"

#include <optional>

class CGHeroInstance;
class CGObjectInstance;
class IGameInfoCallback;

namespace BattleSimulation
{
enum class BattleSimulationBattleType : int32_t
{
	FIELD = 1,
	TOWN = 2,
	TOWN_OUTSIDE = 3
};

std::optional<BattleStartInfo> makeBattleStartInfoForVisit(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CGObjectInstance * target);

BattleSimulationSeedContext makeSeedContextForVisit(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CGObjectInstance * target,
	int64_t gameSeed,
	BattleSimulationBattleType battleType,
	int32_t sampleIndex = 0,
	int32_t evaluatorVersion = 1);

std::optional<BattleSimulationRequest> makeBattleSimulationRequestForVisit(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CGObjectInstance * target,
	int64_t gameSeed,
	int32_t sampleCount,
	const BattleSimulationDecisionThresholds & thresholds = {},
	int32_t sampleIndex = 0,
	int32_t evaluatorVersion = 1);
}
