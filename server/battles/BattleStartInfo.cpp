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
#include "../../lib/battle/CObstacleInstance.h"
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

	const auto obstacles = battle.getAllObstacles();
	result.obstacles.reserve(obstacles.size());
	for(const auto & obstacle : obstacles)
	{
		if(!obstacle)
			continue;

		BattleStartObstacleSnapshot snapshot;
		snapshot.uniqueId = obstacle->uniqueID;
		snapshot.id = obstacle->ID;
		snapshot.type = static_cast<int32_t>(obstacle->obstacleType);
		snapshot.position = obstacle->pos.toInt();
		snapshot.trigger = obstacle->getTrigger().getNum();
		snapshot.blocksTiles = obstacle->blocksTiles();
		snapshot.stopsMovement = obstacle->stopsMovement();
		snapshot.triggersEffects = obstacle->triggersEffects();

		const auto * spellObstacle = dynamic_cast<const SpellCreatedObstacle *>(obstacle.get());
		if(spellObstacle)
		{
			snapshot.turnsRemaining = spellObstacle->turnsRemaining;
			snapshot.spellLevel = spellObstacle->spellLevel;
			snapshot.casterSide = static_cast<int32_t>(spellObstacle->casterSide);
			snapshot.hidden = spellObstacle->hidden;
			snapshot.passable = spellObstacle->passable;
			snapshot.trap = spellObstacle->trap;
			snapshot.removeOnTrigger = spellObstacle->removeOnTrigger;
			snapshot.revealed = spellObstacle->revealed;
		}

		if(spellObstacle || obstacle->obstacleType == CObstacleInstance::USUAL || obstacle->obstacleType == CObstacleInstance::ABSOLUTE_OBSTACLE)
		{
			for(const auto & tile : obstacle->getAffectedTiles())
				snapshot.affectedTiles.push_back(tile.toInt());
		}

		result.obstacles.push_back(std::move(snapshot));
	}

	return result;
}
