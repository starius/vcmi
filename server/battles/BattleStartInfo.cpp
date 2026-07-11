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
