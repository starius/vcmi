/*
 * BattleStartInfo.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleStartInfo.h"

#include "../../lib/battle/IBattleState.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"

BattleStartInfo BattleStartInfo::fromBattle(const IBattleInfo & battle)
{
	return BattleStartInfo{
		BattleSideArray<const CArmedInstance *>{
			battle.getSideArmy(BattleSide::ATTACKER),
			battle.getSideArmy(BattleSide::DEFENDER)
		},
		BattleSideArray<const CGHeroInstance *>{
			battle.getSideHero(BattleSide::ATTACKER),
			battle.getSideHero(BattleSide::DEFENDER)
		},
		battle.getLocation(),
		battle.getLayout(),
		battle.getDefendedTown()
	};
}

BattleStartArmySnapshot makeBattleStartArmySnapshot(const CArmedInstance * army)
{
	BattleStartArmySnapshot result;
	if(!army)
		return result;

	result.objectId = army->id;
	result.armyStrength = army->getArmyStrength();
	result.stacks.reserve(army->stacksCount());
	for(const auto & [slot, stack] : army->Slots())
	{
		if(!stack)
			continue;

		result.stacks.push_back(BattleStartStackSnapshot{
			slot,
			stack->getCreatureID(),
			stack->getCount(),
			stack->getPower(),
			stack->getTotalExperience()
		});
	}
	return result;
}

std::optional<BattleStartTownPreMergeSnapshot> makeBattleStartTownPreMergeSnapshot(
	const CGTownInstance * town,
	const CGHeroInstance * defendingHero)
{
	if(!town || !defendingHero)
		return std::nullopt;

	return BattleStartTownPreMergeSnapshot{
		town->id,
		defendingHero->id,
		makeBattleStartArmySnapshot(town),
		makeBattleStartArmySnapshot(defendingHero)
	};
}
