/*
 * ClassicSpellEvaluator.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "../StdInc.h"
#include "ClassicSpellEvaluator.h"

#include "ClassicRulesAdapter.h"
#include "ClassicAttackEvaluator.h"

#include "../../../lib/CCreatureHandler.h"
#include "../../../lib/CStack.h"
#include "../../../lib/battle/CBattleInfoCallback.h"
#include "../../../lib/mapObjects/CGHeroInstance.h"
#include "../../../lib/spells/CSpell.h"
#include "../../../lib/spells/ISpellMechanics.h"
#include "../StackWithBonuses.h"
#include "ClassicBattleRng.h"
#include "ClassicDecisionTrace.h"
#include <vcmi/Environment.h>

namespace
{
std::vector<spells::Target> enumerateTargets(const spells::Mechanics & mechanics)
{
	const auto aimTypes = mechanics.getTargetTypes();
	std::vector<spells::Target> partial(1);
	for(spells::AimType aim : aimTypes)
	{
		std::vector<battle::Destination> destinations;
		switch(aim)
		{
			case spells::AimType::NOTHING:
				break;
			case spells::AimType::CREATURE:
			{
				auto units = mechanics.battle()->battleGetAllUnits(false);
				std::stable_sort(
					units.begin(),
					units.end(),
					[](const battle::Unit * lhs, const battle::Unit * rhs)
					{
						return lhs->unitId() < rhs->unitId();
					}
				);
				for(const battle::Unit * unit : units)
					destinations.emplace_back(unit);
				break;
			}
			case spells::AimType::LOCATION:
				for(int32_t index = 0; index < GameConstants::BFIELD_SIZE; ++index)
					destinations.emplace_back(BattleHex(index));
				break;
			case spells::AimType::OBSTACLE:
				return {};
		}

		if(aim == spells::AimType::NOTHING)
			continue;
		std::vector<spells::Target> expanded;
		for(const auto & prefix : partial)
		{
			for(const auto & destination : destinations)
			{
				auto candidate = prefix;
				candidate.push_back(destination);
				expanded.push_back(std::move(candidate));
			}
		}
		partial = std::move(expanded);
	}

	std::erase_if(
		partial,
		[&](const spells::Target & target)
		{
			return !mechanics.canBeCastAt(target);
		}
	);
	return partial;
}

int64_t unitUtility(const battle::Unit * unit)
{
	if(!unit || !unit->alive() || !unit->unitType())
		return 0;
	const int64_t maxHealth = std::max<int64_t>(1, unit->getMaxHealth());
	const int64_t base = static_cast<int64_t>(unit->unitType()->getFightValue()) * unit->getAvailableHealth() / maxHealth;
	const int32_t combatStats =
		ClassicRulesAdapter::attack(unit, unit->isShooter()) + ClassicRulesAdapter::defense(unit);
	int64_t result = base + base * combatStats / 40;
	result += base * unit->getMovementRange() / 20;
	if(!unit->canMove())
		result /= 2;
	return result;
}

int64_t signedUtility(const battle::Unit * unit, BattleSide owner, BattleSide decidingSide)
{
	return owner == decidingSide ? unitUtility(unit) : -unitUtility(unit);
}
}

ClassicSpellEvaluator::ClassicSpellEvaluator(
	const Environment * env,
	std::shared_ptr<CBattleInfoCallback> battle,
	std::shared_ptr<IClassicBattleAIRng> randomGenerator,
	std::shared_ptr<ClassicDecisionTrace> trace,
	const std::map<uint32_t, SpellID> * selectedCreatureSpells,
	const ClassicAITargetRecords * targetRecords,
	const ClassicAttackEvaluator * attackEvaluator
)
	: env(env)
	, battle(std::move(battle))
	, randomGenerator(std::move(randomGenerator))
	, trace(std::move(trace))
	, selectedCreatureSpells(selectedCreatureSpells)
	, targetRecords(targetRecords)
	, attackEvaluator(attackEvaluator)
{
}

int64_t ClassicSpellEvaluator::evaluateStateChange(const HypotheticBattle & after, BattleSide side) const
{
	std::set<uint32_t> ids;
	for(const battle::Unit * unit : battle->battleGetAllUnits(false))
		ids.insert(unit->unitId());
	for(const battle::Unit * unit : after.battleGetAllUnits(false))
		ids.insert(unit->unitId());

	int64_t beforeValue = 0;
	int64_t afterValue = 0;
	for(uint32_t id : ids)
	{
		if(const battle::Unit * unit = battle->battleGetUnitByID(id))
		{
			const BattleSide owner = battle->playerToSide(battle->battleGetOwner(unit));
			beforeValue += signedUtility(unit, owner, side);
		}
		if(const battle::Unit * unit = after.battleGetUnitByID(id))
		{
			const BattleSide owner = after.playerToSide(after.battleGetOwner(unit));
			afterValue += signedUtility(unit, owner, side);
		}
	}
	return afterValue - beforeValue;
}

int64_t ClassicSpellEvaluator::applyManaConservation(int64_t value, int32_t castsAvailable)
{
	if(castsAvailable <= 0 || value <= 0)
		return 0;
	if(castsAvailable < 7)
		return static_cast<int64_t>(std::llround(value * std::sqrt(static_cast<double>(castsAvailable))));
	return 5 * value / 2;
}

int32_t ClassicSpellEvaluator::classicSlowSpeed(int32_t currentEffectiveSpeed, int32_t effectLevel)
{
	const int32_t slowPercent = effectLevel >= 2 ? 50 : 75;
	return std::max(1, currentEffectiveSpeed * slowPercent / 100);
}

int64_t ClassicSpellEvaluator::manaAdjustedValue(int64_t rawValue, int32_t currentMana, int32_t cost) const
{
	return cost > 0 ? applyManaConservation(rawValue, currentMana / cost) : applyManaConservation(rawValue, 7);
}

ClassicScoredSpell ClassicSpellEvaluator::chooseHeroSpell(
	BattleSide side,
	bool retreating,
	const ClassicCombatParameters & parameters) const
{
	ClassicScoredSpell best;
	const CGHeroInstance * hero = battle->battleGetFightingHero(side);
	if(!canChooseHeroSpell(side))
		return best;
	ClassicCombatValue combatValue(battle);
	const ClassicProjectedTargets hasteTargets = attackEvaluator
		? attackEvaluator->projectSpellTargets(side, parameters)
		: ClassicProjectedTargets{};
	const ClassicProjectedTargets slowTargets = attackEvaluator
		? attackEvaluator->projectSpellTargets(battle->otherSide(side), parameters)
		: ClassicProjectedTargets{};

	for(int32_t spellIndex = 0; spellIndex <= 69; ++spellIndex)
	{
		const CSpell * spell = SpellID(spellIndex).toSpell();
		if(!spell || !spell->isCombat() || (retreating && !spell->isDamage()))
			continue;
		if(!hero->canCastThisSpell(spell))
			continue;
		if(!spell->canBeCast(battle.get(), spells::Mode::HERO, hero))
			continue;
		const int32_t cost = spell->getCost(hero->getSpellSchoolLevel(spell));
		if(cost > hero->mana)
			continue;

		spells::BattleCast castInfo(battle.get(), hero, spells::Mode::HERO, spell);
		auto mechanics = spell->battleMechanics(&castInfo);
		ClassicScoredSpell spellBest;
		for(const spells::Target & target : enumerateTargets(*mechanics))
		{
			auto state = std::make_shared<HypotheticBattle>(env, battle);
			spells::BattleCast cast(state.get(), hero, spells::Mode::HERO, spell);
			cast.castEval(state->getServerCallback(), target);
			int64_t rawValue = evaluateStateChange(*state, side);
			if(spellIndex == SpellID::HASTE)
			{
				rawValue = 0;
				const int32_t horizon = std::max(1, parameters.roundsLeft);
				const int32_t duration = mechanics->getEffectDuration();
				for(const CStack * original : battle->battleGetAllStacks(false))
				{
					if(original->unitSide() != side
					   || vstd::contains(original->activeSpells(), SpellID(SpellID::HASTE)))
						continue;
					const auto planIt = hasteTargets.find(original->unitId());
					if(planIt == hasteTargets.end() || !planIt->second.target)
						continue;
					if(duration - (original->waited() ? 1 : 0) <= 0)
						continue;
					const battle::Unit * resulting = state->battleGetUnitByID(original->unitId());
					if(!resulting || !resulting->alive())
						continue;
					// Basic/advanced Haste has a single unit target. Expert Haste
					// is represented by an empty mass target.
					const bool affected = target.empty() || std::ranges::any_of(
						target,
						[&](const auto & destination)
						{
							return destination.unitValue
								&& destination.unitValue->unitId() == original->unitId();
						});
					if(!affected)
						continue;
					const int32_t oldSpeed = original->getMovementRange();
					const int32_t newSpeed = resulting->getMovementRange();
					if(oldSpeed <= 0 || newSpeed <= 0)
						continue;
					const int32_t distance = planIt->second.distance;
					const int32_t oldTime = std::max(1, (distance + oldSpeed - 1) / oldSpeed);
					const int32_t newTime = std::max(1, (distance + newSpeed - 1) / newSpeed);
					if(newTime > horizon)
						continue;

					int64_t value = 0;
					if(newTime == 1)
					{
						const int32_t targetSpeed = planIt->second.target->getMovementRange();
						if(targetSpeed >= oldSpeed && targetSpeed < newSpeed && attackEvaluator)
						{
							value = std::max<int64_t>(0, attackEvaluator->spellExchangeEffect(
								original, planIt->second.target, parameters));
						}
					}
					if(newTime < oldTime)
					{
						const int32_t cappedOldTime = std::min(oldTime, horizon + 1);
						const int64_t stackValue = combatValue.stackValue(original, parameters);
						value += (horizon - newTime + 1) * stackValue / horizon;
						value -= (horizon - cappedOldTime + 1) * stackValue / horizon;
					}
					rawValue += value;
					if(trace)
						trace->record("spell.haste.stack", std::to_string(original->unitId()), value);
				}
			}
			else if(spellIndex == SpellID::SLOW)
			{
				rawValue = 0;
				const int32_t horizon = std::max(1, parameters.roundsLeft);
				const int32_t duration = mechanics->getEffectDuration();
				for(const CStack * original : battle->battleGetAllStacks(false))
				{
					if(original->unitSide() == side
					   || vstd::contains(original->activeSpells(), SpellID(SpellID::SLOW)))
						continue;
					const battle::Unit * resulting = state->battleGetUnitByID(original->unitId());
					const bool affected = target.empty() || std::ranges::any_of(
						target,
						[&](const auto & destination)
						{
							return destination.unitValue
								&& destination.unitValue->unitId() == original->unitId();
						});
					if(!resulting || !resulting->alive() || !affected)
						continue;
					const auto planIt = slowTargets.find(original->unitId());
					if(planIt == slowTargets.end() || !planIt->second.target)
						continue;
					if(duration - (original->waited() ? 1 : 0) <= 0)
						continue;

					const int32_t oldSpeed = original->getMovementRange();
					// The SoD callback does not ask the rules engine for the
					// post-cast speed. It applies Slow's mastery percentage to
					// the stack's current effective speed, even when that speed
					// already includes Haste.
					const int32_t newSpeed = classicSlowSpeed(oldSpeed, mechanics->getEffectLevel());
					if(oldSpeed <= 0 || newSpeed <= 0)
						continue;
					const int32_t distance = planIt->second.distance;
					const int32_t oldTime = std::max(1, (distance + oldSpeed - 1) / oldSpeed);
					if(oldTime > horizon)
						continue;

					int64_t value = 0;
					if(oldTime == 1 && attackEvaluator)
					{
						// If Slow moves the victim behind a friendly attacker that
						// plans to hit it this turn, preserve the best newly reversed
						// exchange. The executable scans physical army order and keeps
						// a strict maximum starting at zero.
						for(const CStack * friendly : battle->battleGetAllStacks(false))
						{
							if(friendly->unitSide() != side || !friendly->alive()
							   || friendly->hasBonusOfType(BonusType::BIND_EFFECT)
							   || friendly->isFirstAidTent() || friendly->isAmmoCart()
							   || vstd::contains(friendly->activeSpells(), SpellID(SpellID::BLIND))
							   || vstd::contains(friendly->activeSpells(), SpellID(SpellID::STONE_GAZE))
							   || vstd::contains(friendly->activeSpells(), SpellID(SpellID::PARALYZE)))
								continue;
							const auto friendlyPlan = hasteTargets.find(friendly->unitId());
							if(friendlyPlan == hasteTargets.end()
							   || friendlyPlan->second.target != original)
								continue;
							const int32_t friendlySpeed = friendly->getMovementRange();
							if(friendlySpeed > oldSpeed || friendlySpeed <= newSpeed)
								continue;
							value = std::max<int64_t>(value, attackEvaluator->spellExchangeEffect(
								friendly, original, parameters));
						}
					}

					// Ranged stacks lose no modeled attack opportunities. For a
					// melee target, cap the delay by spell duration and the combat
					// horizon, then subtract the two integer action-share values.
					if(!battle->battleCanShoot(original))
					{
						const int32_t newTime = std::max(1, (distance + newSpeed - 1) / newSpeed);
						const int32_t delayedTime = std::min(
							std::min(newTime - oldTime, duration) + oldTime,
							horizon + 1);
						if(delayedTime > oldTime)
						{
							const int64_t stackValue = combatValue.stackValue(original, parameters);
							value += (horizon - oldTime + 1) * stackValue / horizon;
							value -= (horizon - delayedTime + 1) * stackValue / horizon;
						}
					}
					rawValue += value;
					if(trace)
						trace->record("spell.slow.stack", std::to_string(original->unitId()), value);
				}
			}
			if(spell->isDamage())
			{
				// The executable values every damaged stack with the tactical
				// combat-loss function.  A generic before/after utility delta is
				// observably wrong for partial hit points and for the group-wide
				// friendly-fire tests below.
				int64_t enemyDamageValue = 0;
				int64_t friendlyDamageValue = 0;
				int64_t enemyTotalValue = 0;
				int64_t friendlyTotalValue = 0;
				for(const CStack * originalTarget : battle->battleGetAllStacks(false))
				{
					const BattleSide targetSide = battle->playerToSide(
						battle->battleGetOwner(originalTarget));
					const int64_t totalValue = combatValue.stackValue(originalTarget, parameters);
					if(targetSide == side)
						friendlyTotalValue += totalValue;
					else
						enemyTotalValue += totalValue;

					const battle::Unit * resultingTarget = state->battleGetUnitByID(
						originalTarget->unitId());
					const int64_t beforeHealth = originalTarget->getAvailableHealth();
					const int64_t afterHealth = resultingTarget && resultingTarget->alive()
						? resultingTarget->getAvailableHealth()
						: 0;
					if(afterHealth >= beforeHealth)
						continue;
					int64_t targetValue = combatValue.lossValue(
						originalTarget, beforeHealth, afterHealth, parameters);
					if(targetValue > 0
					   && (!originalTarget->canMove()
						   || originalTarget->isFirstAidTent()
						   || originalTarget->isAmmoCart()))
					{
						targetValue = 2 * targetValue - totalValue;
					}
					if(targetSide == side)
						friendlyDamageValue += targetValue;
					else
						enemyDamageValue += targetValue;
				}

				rawValue = enemyDamageValue - friendlyDamageValue;
				if(spellIndex >= 24 && spellIndex <= 26)
				{
					// Death Ripple, Destroy Undead, and Armageddon are accepted
					// only when they hurt the enemy both absolutely and by a
					// greater share of its army.  Use products to preserve the
					// original integer comparison without division rounding.
					const int64_t friendlyLoss = std::max<int64_t>(0, friendlyDamageValue);
					const bool safeMassDamage = enemyDamageValue > 0
						&& enemyDamageValue > friendlyLoss
						&& friendlyLoss < friendlyTotalValue
						&& enemyTotalValue > 0
						&& friendlyTotalValue > 0
						&& enemyDamageValue * friendlyTotalValue
							> friendlyLoss * enemyTotalValue;
					if(!safeMassDamage)
						rawValue = 0;
				}
			}
			if(rawValue <= 0)
				continue;
			if(trace)
			{
				const std::string key = std::to_string(spellIndex) + ":"
					+ (target.empty() ? "-1" : std::to_string(target.front().hexValue.toInt()));
				trace->record("spell.raw", key, rawValue);
			}
			if(!spellBest.valid || rawValue > spellBest.rawValue)
			{
				spellBest.valid = true;
				spellBest.rawValue = rawValue;
				spellBest.action.actionType = EActionType::HERO_SPELL;
				spellBest.action.spell = spell->id;
				spellBest.action.setTarget(target);
				spellBest.action.side = side;
				spellBest.action.stackNumber = -1;
			}
		}
		if(!spellBest.valid)
			continue;
		const int64_t adjusted = manaAdjustedValue(spellBest.rawValue, hero->mana, cost);
		const int32_t randomPercent = randomGenerator->nextIntInclusive(75, 100);
		spellBest.score = adjusted * randomPercent / 100;
		if(trace)
		{
			const std::string key = std::to_string(spellIndex) + ":"
				+ (spellBest.action.target.empty()
					? "-1"
					: std::to_string(spellBest.action.target.front().hexValue.toInt()));
			trace->record("spell.random", key, randomPercent);
			trace->record("spell.score", key, spellBest.score);
		}
		if(!best.valid || spellBest.score > best.score)
			best = spellBest;
	}
	return best;
}

bool ClassicSpellEvaluator::canChooseHeroSpell(BattleSide side) const
{
	const CGHeroInstance * hero = battle->battleGetFightingHero(side);
	return hero && !battle->battleTacticDist()
		&& battle->battleCanCastSpell(hero, spells::Mode::HERO) == ESpellCastProblem::OK;
}

ClassicScoredSpell ClassicSpellEvaluator::chooseCreatureSpell(
	const CStack * caster,
	int64_t competingValue,
	const ClassicCombatParameters & parameters) const
{
	ClassicScoredSpell best;
	if(!caster->canCast())
		return best;
	const int32_t creatureID = caster->creatureId().getNum();
	if(competingValue != 0 && (creatureID == 37 || creatureID == 91))
	{
		const int32_t roll = randomGenerator->nextIntInclusive(1, 100);
		if(trace)
			trace->record("creatureSpell", "declineRoll", roll);
		if(roll <= 30)
			return best;
	}

	std::vector<SpellID> spells;
	const auto selected = selectedCreatureSpells
		? selectedCreatureSpells->find(caster->unitId())
		: std::map<uint32_t, SpellID>::const_iterator();
	if(selectedCreatureSpells && selected != selectedCreatureSpells->end())
	{
		// Heroes III selects a Faerie Dragon's weighted spell before DoCompAI
		// and stores it on the army. That selection is canonical pre-state, not
		// a tactical-AI RNG request.
		spells.push_back(selected->second);
	}
	else
	{
		for(const auto & bonus : *caster->getBonusesOfType(BonusType::SPELLCASTER))
		{
			if(!bonus->parameters && bonus->subtype.as<SpellID>().hasValue())
				spells.push_back(bonus->subtype.as<SpellID>());
		}
		ClassicVstdRngAdapter rng(*randomGenerator);
		const SpellID randomSpell = battle->getRandomCastedSpell(rng, caster);
		if(randomSpell.hasValue())
			spells.push_back(randomSpell);
	}
	std::stable_sort(
		spells.begin(),
		spells.end(),
		[](SpellID lhs, SpellID rhs)
		{
			return lhs.getNum() < rhs.getNum();
		}
	);
	spells.erase(std::unique(spells.begin(), spells.end()), spells.end());

	for(SpellID spellID : spells)
	{
		const CSpell * spell = spellID.toSpell();
		if(!spell || !spell->canBeCast(battle.get(), spells::Mode::CREATURE_ACTIVE, caster))
			continue;
		spells::BattleCast castInfo(battle.get(), caster, spells::Mode::CREATURE_ACTIVE, spell);
		auto mechanics = spell->battleMechanics(&castInfo);
		for(const spells::Target & target : enumerateTargets(*mechanics))
		{
			auto state = std::make_shared<HypotheticBattle>(env, battle);
			const battle::Unit * stateCaster = state->battleGetUnitByID(caster->unitId());
			spells::BattleCast cast(state.get(), stateCaster, spells::Mode::CREATURE_ACTIVE, spell);
			cast.castEval(state->getServerCallback(), target);
			int64_t score = evaluateStateChange(
				*state, battle->playerToSide(battle->battleGetOwner(caster)));
			const bool faerieDragon = creatureID == 134;
			const int32_t spellIndex = spellID.getNum();
			const bool directDamageSpell = spellIndex >= 15 && spellIndex <= 18;
			if(faerieDragon && directDamageSpell
			   && target.size() == 1 && target.front().unitValue)
			{
				const uint32_t targetId = target.front().unitValue->unitId();
				const CStack * originalTarget = battle->battleGetStackByID(targetId, false);
				const battle::Unit * resultingTarget = state->battleGetUnitByID(targetId);
				if(originalTarget)
				{
					const int64_t afterHealth = resultingTarget && resultingTarget->alive()
						? resultingTarget->getAvailableHealth()
						: 0;
					ClassicCombatValue combatValue(battle);
					score = combatValue.lossValue(
						originalTarget,
						originalTarget->getAvailableHealth(),
						afterHealth,
						parameters);
					const BattleSide casterSide = battle->playerToSide(battle->battleGetOwner(caster));
					const BattleSide targetSide = battle->playerToSide(
						battle->battleGetOwner(originalTarget));
					if(targetSide == casterSide)
						score = -score;
					if(score > 0
					   && (!originalTarget->canMove()
						   || originalTarget->isFirstAidTent()
						   || originalTarget->isAmmoCart()))
					{
						score = 2 * score
							- combatValue.stackValue(originalTarget, parameters);
					}
				}
			}
			if(score <= 0
			   || (!faerieDragon && score <= competingValue)
			   || (best.valid && score <= best.score))
				continue;
			best.valid = true;
			best.rawValue = score;
			best.score = score;
			best.action = BattleAction::makeCreatureSpellcast(caster, target, spellID);
		}
	}
	return best;
}
