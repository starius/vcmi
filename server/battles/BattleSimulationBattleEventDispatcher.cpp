/*
 * BattleSimulationBattleEventDispatcher.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationBattleEventDispatcher.h"

#include "../../lib/CStack.h"
#include "../../lib/battle/BattleInfo.h"
#include "../../lib/callback/CBattleCallback.h"
#include "../../lib/callback/CBattleGameInterface.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/networkPacks/SetStackEffect.h"

#include <set>

namespace BattleSimulation
{
namespace
{
template<typename Interfaces, typename Func>
void callRegisteredInterface(Interfaces & interfaces, PlayerColor player, Func && func)
{
	auto interface = interfaces.find(player);
	if(interface != interfaces.end())
		func(*interface->second);
}

template<typename Interfaces, typename Func>
void callBattleInterfacesForBothSides(const CGameState & gameState, Interfaces & interfaces, const BattleID & battleID, Func && func)
{
	const auto * battle = gameState.getBattle(battleID);
	if(!battle)
		return;

	std::set<PlayerColor> called;
	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		const auto player = battle->getSide(side).color;
		if(!called.insert(player).second)
			continue;

		callRegisteredInterface(interfaces, player, func);
	}
}

template<typename Interfaces, typename Func>
void callBattleStartBefore(const BattleStart & pack, Interfaces & interfaces, Func && func)
{
	std::set<PlayerColor> called;
	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		const auto player = pack.info->getSide(side).color;
		if(!called.insert(player).second)
			continue;

		callRegisteredInterface(interfaces, player, func);
	}
}

void callBattleCallbackStarted(
	const BattleInfo & battle,
	const std::map<PlayerColor, std::shared_ptr<CBattleCallback>> & callbacks)
{
	const auto leftColor = battle.getSide(BattleSide::LEFT_SIDE).color;
	const auto rightColor = battle.getSide(BattleSide::RIGHT_SIDE).color;

	for(const auto & [player, callback] : callbacks)
	{
		if(!player.isValidPlayer() || player == leftColor || player == rightColor)
			callback->onBattleStarted(&battle);
	}
}

void callBattleCallbackEnded(
	const BattleInfo & battle,
	const std::map<PlayerColor, std::shared_ptr<CBattleCallback>> & callbacks)
{
	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		const auto player = battle.getSide(side).color;
		auto callback = callbacks.find(player);
		if(callback != callbacks.end())
			callback->second->onBattleEnded(battle.getBattleID());
	}
}

void callBattleInterfaceStarted(
	const BattleInfo & battle,
	const std::map<PlayerColor, std::shared_ptr<CBattleGameInterface>> & interfaces)
{
	const auto & leftSide = battle.getSide(BattleSide::LEFT_SIDE);
	const auto & rightSide = battle.getSide(BattleSide::RIGHT_SIDE);

	auto callBattleStart = [&](PlayerColor color, BattleSide side)
	{
		callRegisteredInterface(interfaces, color, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleStart(
				battle.getBattleID(),
				leftSide.getArmy(),
				rightSide.getArmy(),
				battle.tile,
				leftSide.getHero(),
				rightSide.getHero(),
				side,
				battle.replayAllowed);
		});
	};

	callBattleStart(leftSide.color, BattleSide::LEFT_SIDE);
	callBattleStart(rightSide.color, BattleSide::RIGHT_SIDE);
	callBattleStart(PlayerColor::UNFLAGGABLE, BattleSide::RIGHT_SIDE);
	callBattleStart(PlayerColor::SPECTATOR, BattleSide::RIGHT_SIDE);
}

void callTacticsInterface(
	const BattleInfo & battle,
	const std::map<PlayerColor, std::shared_ptr<CBattleGameInterface>> & interfaces)
{
	if(!battle.tacticDistance)
		return;

	const auto tacticianColor = battle.getSide(battle.tacticsSide).color;
	callRegisteredInterface(interfaces, tacticianColor, [&](CBattleGameInterface & battleInterface)
	{
		battleInterface.yourTacticPhase(battle.getBattleID(), battle.tacticDistance);
	});
}
}

BattleSimulationBattleEventDispatcher::BattleSimulationBattleEventDispatcher(const CGameState & gameState)
	: gameState(gameState)
{
}

void BattleSimulationBattleEventDispatcher::registerBattleCallback(PlayerColor player, std::shared_ptr<CBattleCallback> callback)
{
	battleCallbacks[player] = std::move(callback);
}

void BattleSimulationBattleEventDispatcher::registerBattleInterface(PlayerColor player, std::shared_ptr<CBattleGameInterface> battleInterface)
{
	battleInterfaces[player] = std::move(battleInterface);
}

void BattleSimulationBattleEventDispatcher::beforeApply(CPackForClient & pack)
{
	if(auto * battleStart = dynamic_cast<BattleStart *>(&pack))
	{
		callBattleStartBefore(*battleStart, battleInterfaces, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleStartBefore(
				battleStart->battleID,
				battleStart->info->getSideArmy(BattleSide::ATTACKER),
				battleStart->info->getSideArmy(BattleSide::DEFENDER),
				battleStart->info->tile,
				battleStart->info->getSideHero(BattleSide::ATTACKER),
				battleStart->info->getSideHero(BattleSide::DEFENDER));
		});
		return;
	}

	if(auto * nextRound = dynamic_cast<BattleNextRound *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, nextRound->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleNewRoundFirst(nextRound->battleID);
		});
		return;
	}

	if(auto * gate = dynamic_cast<BattleUpdateGateState *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, gate->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleGateStateChanged(gate->battleID, gate->state);
		});
		return;
	}

	if(auto * result = dynamic_cast<BattleResult *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, result->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleEnd(result->battleID, result, result->queryID);
		});

		if(const auto * battle = gameState.getBattle(result->battleID))
			callBattleCallbackEnded(*battle, battleCallbacks);
		return;
	}

	if(auto * moved = dynamic_cast<BattleStackMoved *>(&pack))
	{
		const auto * battle = gameState.getBattle(moved->battleID);
		const auto * movedStack = battle ? battle->battleGetStackByID(moved->stack) : nullptr;
		callBattleInterfacesForBothSides(gameState, battleInterfaces, moved->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleStackMoved(moved->battleID, movedStack, moved->tilesToMove, moved->distance, moved->teleporting);
		});
		return;
	}

	if(auto * attack = dynamic_cast<BattleAttack *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, attack->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleAttack(attack->battleID, attack);
			battleInterface.battleStacksAttacked(attack->battleID, attack->bsa, attack->shot());
		});
		return;
	}

	if(auto * action = dynamic_cast<StartAction *>(&pack))
	{
		currentBattleAction = action->ba;
		callBattleInterfacesForBothSides(gameState, battleInterfaces, action->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.actionStarted(action->battleID, action->ba);
		});
		return;
	}
}

void BattleSimulationBattleEventDispatcher::afterApply(CPackForClient & pack)
{
	if(auto * battleStart = dynamic_cast<BattleStart *>(&pack))
	{
		const auto * battle = gameState.getBattle(battleStart->battleID);
		if(!battle)
			return;

		callBattleCallbackStarted(*battle, battleCallbacks);
		callBattleInterfaceStarted(*battle, battleInterfaces);
		callTacticsInterface(*battle, battleInterfaces);
		return;
	}

	if(auto * nextRound = dynamic_cast<BattleNextRound *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, nextRound->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleNewRound(nextRound->battleID);
		});
		return;
	}

	if(auto * activeStack = dynamic_cast<BattleSetActiveStack *>(&pack))
	{
		if(activeStack->reason == BattleUnitTurnReason::AUTOMATIC_ACTION)
			return;

		const auto * battle = gameState.getBattle(activeStack->battleID);
		if(!battle)
			return;

		const auto * activated = battle->battleGetStackByID(activeStack->stack, false);
		if(!activated)
			return;

		PlayerColor playerToCall;
		if(activated->isHypnotized())
		{
			playerToCall = battle->getSide(BattleSide::ATTACKER).color == activated->unitOwner()
				? battle->getSide(BattleSide::DEFENDER).color
				: battle->getSide(BattleSide::ATTACKER).color;
		}
		else
		{
			playerToCall = activated->unitOwner();
		}

		callRegisteredInterface(battleInterfaces, playerToCall, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.activeStack(activeStack->battleID, activated);
		});
		return;
	}

	if(auto * logMessage = dynamic_cast<BattleLogMessage *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, logMessage->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleLogMessage(logMessage->battleID, logMessage->lines);
		});
		return;
	}

	if(auto * trigger = dynamic_cast<BattleTriggerEffect *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, trigger->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleTriggerEffect(trigger->battleID, *trigger);
		});
		return;
	}

	if(auto * spellCast = dynamic_cast<BattleSpellCast *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, spellCast->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleSpellCast(spellCast->battleID, spellCast);
		});
		return;
	}

	if(auto * effect = dynamic_cast<SetStackEffect *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, effect->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleStacksEffectsSet(effect->battleID, *effect);
		});
		return;
	}

	if(auto * injured = dynamic_cast<StacksInjured *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, injured->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleStacksAttacked(injured->battleID, injured->stacks, false);
		});
		return;
	}

	if(auto * units = dynamic_cast<BattleUnitsChanged *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, units->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleUnitsChanged(units->battleID, units->changedStacks);
		});
		return;
	}

	if(auto * obstacles = dynamic_cast<BattleObstaclesChanged *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, obstacles->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleObstaclesChanged(obstacles->battleID, obstacles->changes);
		});
		return;
	}

	if(auto * catapult = dynamic_cast<CatapultAttack *>(&pack))
	{
		callBattleInterfacesForBothSides(gameState, battleInterfaces, catapult->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.battleCatapultAttacked(catapult->battleID, *catapult);
		});
		return;
	}

	if(auto * actionEnd = dynamic_cast<EndAction *>(&pack))
	{
		if(!currentBattleAction)
			return;

		callBattleInterfacesForBothSides(gameState, battleInterfaces, actionEnd->battleID, [&](CBattleGameInterface & battleInterface)
		{
			battleInterface.actionFinished(actionEnd->battleID, *currentBattleAction);
		});
		currentBattleAction.reset();
		return;
	}
}
}
