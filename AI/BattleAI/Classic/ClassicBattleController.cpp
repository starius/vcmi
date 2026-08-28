/*
 * ClassicBattleController.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "../StdInc.h"
#include "ClassicBattleController.h"

#include "../../../lib/CStack.h"
#include "../../../lib/StartInfo.h"
#include "../../../lib/battle/BattleAction.h"
#include "../../../lib/battle/CPlayerBattleCallback.h"
#include "../../../lib/callback/CBattleCallback.h"
#include "../../../lib/callback/IGameInfoCallback.h"
#include "ClassicAttackEvaluator.h"
#include "ClassicBattleDecision.h"
#include "ClassicBattleRng.h"
#include "ClassicBattleStateView.h"
#include "ClassicCombatValue.h"
#include "ClassicDecisionTrace.h"
#include "ClassicRetreatEvaluator.h"
#include "ClassicSpellEvaluator.h"
#include <vcmi/Environment.h>

ClassicBattleController::ClassicBattleController(
	std::shared_ptr<Environment> env,
	std::shared_ptr<CBattleCallback> cb,
	PlayerColor playerID,
	std::shared_ptr<IClassicBattleAIRng> randomGenerator,
	std::shared_ptr<ClassicDecisionTrace> trace
)
	: env(std::move(env)),
	cb(std::move(cb)),
	playerID(playerID),
	side(BattleSide::NONE),
	randomGenerator(std::move(randomGenerator)),
	trace(std::move(trace))
{
	if(!this->randomGenerator)
		this->randomGenerator = std::make_shared<ClassicBattleAIRng>();
}

void ClassicBattleController::battleStart(const BattleID & battleID, BattleSide side)
{
	this->side = side;
	currentBattleID = battleID;
	tacticsStackIDs.clear();
	decisionContext = {};
	tacticsCursor = 0;
	movingTacticsStack = std::numeric_limits<uint32_t>::max();
}

void ClassicBattleController::battleEnd(const BattleID & battleID)
{
	side = BattleSide::NONE;
	currentBattleID = BattleID::NONE;
	tacticsStackIDs.clear();
	decisionContext = {};
	tacticsCursor = 0;
	movingTacticsStack = std::numeric_limits<uint32_t>::max();
}

void ClassicBattleController::activeStack(
	const BattleID & battleID,
	const CStack * stack,
	const AutocombatPreferences & preferences)
{
	const BattleAction action = decideStackAction(battleID, stack, preferences);
	if(action.actionType == EActionType::HERO_SPELL)
		cb->battleMakeSpellAction(battleID, action);
	else
		cb->battleMakeUnitAction(battleID, action);
}

BattleAction ClassicBattleController::decideStackAction(
	const BattleID & battleID,
	const CStack * stack,
	const AutocombatPreferences & preferences)
{
	const auto battle = cb->getBattle(battleID);
	const int32_t difficulty = env->game()->getStartInfo()->difficulty;
	decisionContext.secondPhase = stack->waited();
	return ClassicBattleDecision::decide(
		env.get(), battle, side, stack, difficulty, preferences,
		randomGenerator, trace, ClassicDecisionEntryPoint::FULL_PIPELINE, &decisionContext);
}

void ClassicBattleController::yourTacticPhase(
	const BattleID & battleID,
	int distance,
	const AutocombatPreferences & preferences)
{
	currentBattleID = battleID;
	if(!preferences.enableTacticsUsage)
	{
		cb->battleMakeTacticAction(battleID, BattleAction::makeEndOFTacticPhase(side));
		return;
	}

	ClassicBattleStateView view(cb->getBattle(battleID));
	for(const CStack * stack : view.orderedStacks())
	{
		if(stack->unitSide() == side && stack->canMove())
			tacticsStackIDs.push_back(stack->unitId());
	}
	tacticsCursor = 0;
	advanceTactics();
}

void ClassicBattleController::actionFinished(const BattleID & battleID, const BattleAction & action)
{
	if(battleID != currentBattleID || movingTacticsStack == std::numeric_limits<uint32_t>::max())
		return;
	if(action.actionType != EActionType::WALK || action.stackNumber != movingTacticsStack)
		return;
	movingTacticsStack = std::numeric_limits<uint32_t>::max();
	advanceTactics();
}

std::optional<BattleHex> ClassicBattleController::chooseTacticsHex(const CStack * stack) const
{
	const auto battle = cb->getBattle(currentBattleID);
	const ReachabilityInfo reachability = battle->getReachability(stack);
	const BattleHexArray available = battle->battleGetAvailableHexes(reachability, stack, true);
	BattleHex best = stack->getPosition();
	int32_t bestScore = std::numeric_limits<int32_t>::max();

	auto scoreHex = [&](const BattleHex & candidate)
	{
		int32_t score = 0;
		for(const BattleHex & adjacent : stack->getSurroundingHexes(candidate))
		{
			if(!adjacent.isAvailable())
			{
				++score;
				continue;
			}
			const auto * neighbour = battle->battleGetUnitByPos(adjacent, true);
			if(!neighbour)
				continue;
			if(battle->battleMatchOwner(stack, neighbour))
				score += neighbour->isShooter() ? 1000 : 10;
			else
				++score;
		}
		return score;
	};

	bestScore = scoreHex(best);
	for(const BattleHex & candidate : available)
	{
		if(!candidate.isAvailable() || !battle->isInTacticRange(candidate))
			continue;
		const int32_t score = scoreHex(candidate);
		if(score < bestScore)
		{
			best = candidate;
			bestScore = score;
		}
	}
	if(best != stack->getPosition())
		return best;
	return std::nullopt;
}

void ClassicBattleController::advanceTactics()
{
	const auto battle = cb->getBattle(currentBattleID);
	while(tacticsCursor < tacticsStackIDs.size())
	{
		const uint32_t stackID = tacticsStackIDs[tacticsCursor++];
		const auto * stack = battle->battleGetStackByID(stackID, true);
		if(!stack)
			continue;
		const auto destination = chooseTacticsHex(stack);
		if(!destination)
			continue;
		movingTacticsStack = stackID;
		cb->battleMakeUnitAction(currentBattleID, BattleAction::makeMove(stack, *destination));
		return;
	}
	cb->battleMakeTacticAction(currentBattleID, BattleAction::makeEndOFTacticPhase(side));
}
