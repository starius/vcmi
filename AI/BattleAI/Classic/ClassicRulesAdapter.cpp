/*
 * ClassicRulesAdapter.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "../StdInc.h"
#include "ClassicRulesAdapter.h"

#include "../../../lib/battle/Unit.h"
#include "../../../lib/CStack.h"
#include "../../../lib/battle/CBattleInfoCallback.h"
#include "../../../lib/mapObjects/CGTownInstance.h"
#include "ClassicBattleStateView.h"

namespace
{
bool isArrowTower(const battle::Unit * unit)
{
	return unit && unit->creatureId() == CreatureID(CreatureID::ARROW_TOWERS);
}
}

int32_t ClassicRulesAdapter::attack(const battle::Unit * unit, bool ranged)
{
	// VCMI deliberately forces tower attack to zero because its combat script
	// applies tower damage independently of attack/defence. The SoD tactical AI
	// reads the CRTRAITS-facing value instead.
	return isArrowTower(unit) ? 10 : unit->getAttack(ranged);
}

int32_t ClassicRulesAdapter::defense(const battle::Unit * unit)
{
	return isArrowTower(unit) ? 5 : unit->getDefense(false);
}

int32_t ClassicRulesAdapter::minDamage(const battle::Unit * unit, bool ranged)
{
	return isArrowTower(unit) ? 2 : unit->getMinDamage(ranged);
}

int32_t ClassicRulesAdapter::maxDamage(const battle::Unit * unit, bool ranged)
{
	return isArrowTower(unit) ? 4 : unit->getMaxDamage(ranged);
}

bool ClassicRulesAdapter::failedSiege(
	const std::shared_ptr<CBattleInfoCallback> & battle,
	BattleSide side)
{
	const CGTownInstance * town = battle->battleGetDefendedTown();
	if(side != BattleSide::ATTACKER
	   || !town
	   || town->fortLevel() != CGTownInstance::CASTLE)
		return false;

	constexpr std::array<EWallPart, 4> BREACH_PARTS = {
		EWallPart::BELOW_GATE,
		EWallPart::OVER_GATE,
		EWallPart::BOTTOM_WALL,
		EWallPart::UPPER_WALL
	};
	for(EWallPart part : BREACH_PARTS)
	{
		if(battle->battleGetWallState(part) <= EWallState::DESTROYED)
			return false;
	}

	ClassicBattleStateView view(battle);
	for(const CStack * stack : view.orderedStacks())
	{
		// CF_IMMOBILIZED (bit 21 at 0x41E61F/0x41E6AB) excludes
		// siege objects. Temporary Bind is unrelated to this creature flag.
		if(stack->hasBonusOfType(BonusType::SIEGE_WEAPON))
			continue;
		if(stack->unitSide() == BattleSide::ATTACKER)
		{
			if(stack->isShooter()
			   || stack->hasBonusOfType(BonusType::FLYING)
			   || battle->battleCanShoot(stack))
				return false;
		}
		else if(!battle->battleIsInsideWalls(stack->getPosition()))
			return false;
	}
	return true;
}
