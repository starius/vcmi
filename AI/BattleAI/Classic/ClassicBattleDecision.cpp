/*
 * ClassicBattleDecision.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "../StdInc.h"
#include "ClassicBattleDecision.h"

#include "../../../lib/CStack.h"
#include "../../../lib/CPlayerState.h"
#include "../../../lib/battle/CBattleInfoCallback.h"
#include "../../../lib/callback/IGameInfoCallback.h"
#include "ClassicAttackEvaluator.h"
#include "ClassicBattleRng.h"
#include "ClassicCombatValue.h"
#include "ClassicDecisionTrace.h"
#include "ClassicRetreatEvaluator.h"
#include "ClassicSpellEvaluator.h"
#include <vcmi/Environment.h>

BattleAction ClassicBattleDecision::decide(
	const Environment * env,
	std::shared_ptr<CBattleInfoCallback> battle,
	BattleSide side,
	const CStack * stack,
	int32_t difficulty,
	const AutocombatPreferences & preferences,
	std::shared_ptr<IClassicBattleAIRng> randomGenerator,
	std::shared_ptr<ClassicDecisionTrace> trace,
	ClassicDecisionEntryPoint entryPoint,
	ClassicDecisionContext * context
)
{
	for(const CStack * candidate : battle->battleGetAllStacks(false))
	{
		const int32_t creatureID = candidate->creatureId().getNum();
		if(creatureID < 0 || creatureID > 149)
			throw std::domain_error("Classic BattleAI supports only original Heroes III SoD creatures");
	}
	if(trace)
		trace->clear();
	ClassicCombatValue valueModel(battle);
	const ClassicCombatParameters parameters = valueModel.buildParameters(side, difficulty);
	const PlayerColor decidingPlayer = battle->sideToPlayer(side);
	const PlayerState * decidingPlayerState = env && decidingPlayer.isValidPlayer()
		? env->game()->getPlayerState(decidingPlayer, false)
		: nullptr;
	const bool localHumanAutocombat = decidingPlayerState && decidingPlayerState->isHuman();
	if(trace)
	{
		trace->record("parameters", "friendly", parameters.friendlyCombatValue);
		trace->record("parameters", "enemy", parameters.enemyCombatValue);
		trace->record("parameters", "rounds", parameters.roundsLeft);
		trace->record("parameters", "killsOnly", parameters.killsOnly);
	}

	ClassicAttackEvaluator evaluator(
		battle,
		randomGenerator,
		trace,
		difficulty >= 2 || localHumanAutocombat,
		context ? context->secondPhase : stack->waited());
	ClassicRetreatEvaluator retreatEvaluator(battle, randomGenerator, trace, env);
	ClassicSpellEvaluator spellEvaluator(
		env, battle, randomGenerator, trace,
		context ? &context->selectedCreatureSpells : nullptr,
		context ? &context->targetRecords : nullptr,
		&evaluator);
	auto persistTargetRecords = [&](bool bothSides)
	{
		if(!context)
			return;
		auto persistSide = [&](BattleSide physicalSide)
		{
			const ClassicProjectedTargets projected = evaluator.projectSpellTargets(
				physicalSide, parameters, false);
			for(const auto & [actor, target] : projected)
			{
				ClassicAITargetRecord & record = context->targetRecords[actor];
				record.hasTarget = target.target != nullptr;
				record.targetUnitId = target.target
					? target.target->unitId()
					: std::numeric_limits<uint32_t>::max();
				record.targetValue = target.value;
				record.targetDistance = target.distance;
				record.possibleTargets = target.possibleTargets;
			}
		};
		persistSide(side);
		if(bothSides)
			persistSide(battle->otherSide(side));
	};
	if(entryPoint == ClassicDecisionEntryPoint::CHECK_RETREAT)
	{
		if(retreatEvaluator.shouldRetreat(side, difficulty, parameters))
			return BattleAction::makeRetreat(side);
		return BattleAction::makeDefend(stack);
	}
	if(entryPoint != ClassicDecisionEntryPoint::DO_COMP_AI
	   && preferences.enableSpellsUsage
	   && spellEvaluator.canChooseHeroSpell(side))
	{
		// DoSpellAI constructs its spell evaluator before AICheckRetreat.  The
		// original constructor runs one simulated action sequence for the casting
		// side; melee target selection inside that simulation
		// consumes RNG even when the later retreat artifact/experience gate exits
		// early.  Keep this pre-pass separate from shouldRetreat(), which can run a
		// second projection when its own gates allow it.
		if(trace)
			trace->record("spell.prepass", "begin", 1);
		const ClassicExpectedDamage spellProjection = evaluator.projectExpectedDamage(
			side, false, parameters);
		if(trace)
			trace->record("spell.prepass", "end", 1);
		const bool retreatingForSpell = retreatEvaluator.shouldRetreat(
			side, difficulty, parameters, &spellProjection);
		const ClassicScoredSpell spell = spellEvaluator.chooseHeroSpell(side, retreatingForSpell, parameters);
		// The native spell-AI model leaves find_AI_targets scratch records on
		// both physical armies. They remain input to later Slow/Haste callbacks.
		persistTargetRecords(true);
		if(spell.valid)
			return spell.action;
	}
	if(entryPoint == ClassicDecisionEntryPoint::DO_SPELL_AI)
		return BattleAction::makeDefend(stack);
	if(entryPoint != ClassicDecisionEntryPoint::DO_COMP_AI
	   && retreatEvaluator.shouldRetreat(side, difficulty, parameters))
		return BattleAction::makeRetreat(side);

	if(stack->isCatapult())
		return evaluator.chooseCatapultAction(stack);
	if(stack->isFirstAidTent())
		return evaluator.chooseHealingTentAction(stack);

	const bool tacticsActive = battle->battleTacticDist() > 0
		&& battle->battleGetTacticsSide() == side;
	if(tacticsActive && stack->isShooter())
	{
		return evaluator.chooseTacticsShooterPlacement(
			stack, difficulty > 0 || localHumanAutocombat);
	}

	const ClassicScoredAction ordinary = evaluator.chooseAction(stack, parameters);
	if(tacticsActive)
	{
		// During deployment the original reuses ordinary movement action 2,
		// but action 3 means "finish tactics", not combat DEFEND.  If the
		// evaluator cannot improve the placement, terminate the phase.
		if(ordinary.valid && ordinary.action.actionType == EActionType::WALK)
			return ordinary.action;
		return BattleAction::makeEndOFTacticPhase(side);
	}
	const int32_t creatureID = stack->creatureId().getNum();
	const bool supportedCaster = creatureID == 13 || creatureID == 37 || creatureID == 51
		|| creatureID == 91 || creatureID == 134;
	if(supportedCaster)
	{
		// The Genie/Ogre and Faerie evaluators each construct the same spell-AI
		// model used by DoSpellAI. Its constructor simulates the casting side before
		// scoring a target; Archangel/Pit Lord resurrection uses a different
		// corpse chooser and does not perform this pass.
		if(creatureID == 37 || creatureID == 91 || creatureID == 134)
			evaluator.projectExpectedDamage(side, false, parameters);
		const ClassicScoredSpell creatureSpell = spellEvaluator.chooseCreatureSpell(
			stack, ordinary.score, parameters);
		if(creatureID == 37 || creatureID == 91 || creatureID == 134)
			persistTargetRecords(false);
		if(creatureSpell.valid)
			return creatureSpell.action;
	}
	return ordinary.action;
}
