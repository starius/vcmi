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

#include "../../lib/CStack.h"
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
		battle.getDefendedTown(),
		std::nullopt
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

BattleStartStateSnapshot makeBattleStartStateSnapshot(const IBattleInfo & battle)
{
	BattleStartStateSnapshot result;
	const auto stacks = battle.getStacksIf([](const CStack * stack)
	{
		return stack && !stack->isGhost();
	});

	result.stacks.reserve(stacks.size());
	for(const auto * stack : stacks)
	{
		result.stacks.push_back(BattleStartStackStateSnapshot{
			stack->unitId(),
			stack->unitSide(),
			stack->unitSlot(),
			stack->creatureId(),
			stack->getCount(),
			stack->unitBaseAmount(),
			stack->getPosition().toInt(),
			stack->initialPosition.toInt(),
			stack->getAvailableHealth(),
			stack->getTotalHealth(),
			static_cast<int32_t>(stack->getMaxHealth()),
			stack->getFirstHPleft(),
			stack->getAttack(false),
			stack->getAttack(true),
			stack->getDefense(false),
			stack->getDefense(true),
			stack->getMinDamage(false),
			stack->getMaxDamage(false),
			stack->getMinDamage(true),
			stack->getMaxDamage(true),
			stack->getInitiative(0),
			static_cast<int32_t>(stack->getMovementRange(0)),
			stack->moraleVal(),
			stack->luckVal(),
			stack->shots.available(),
			stack->shots.total(),
			stack->casts.available(),
			stack->casts.total(),
			stack->counterAttacks.available(),
			stack->counterAttacks.total(),
			stack->alive(),
			stack->isValidTarget(false),
			stack->doubleWide(),
			stack->isShooter(),
			stack->canShoot(),
			stack->isCaster(),
			stack->canCast(),
			stack->isTurret(),
			stack->isCatapult(),
			stack->isBallista(),
			stack->isFirstAidTent(),
			stack->isAmmoCart(),
			stack->summoned
		});
	}

	return result;
}
