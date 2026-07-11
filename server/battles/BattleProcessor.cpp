/*
 * BattleProcessor.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleProcessor.h"

#include "BattleActionProcessor.h"
#include "BattleFlowProcessor.h"
#include "BattleResultProcessor.h"
#include "BattleSimulationBatch.h"
#include "BattleSimulationEvaluator.h"

#include "../CGameHandler.h"
#include "../queries/QueriesProcessor.h"
#include "../queries/BattleQueries.h"

#include "../../lib/CPlayerState.h"
#include "../../lib/TerrainHandler.h"
#include "../../lib/battle/CBattleInfoCallback.h"
#include "../../lib/battle/CObstacleInstance.h"
#include "../../lib/battle/BattleInfo.h"
#include "../../lib/battle/BattleLayout.h"
#include "../../lib/entities/building/TownFortifications.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/mapping/CMap.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/modding/IdentifierStorage.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/CPlayerState.h"
#include <vstd/RNG.h>

namespace
{
std::optional<BattleStartTownPreMergeSnapshot> takeMatchingTownPreMergeSnapshot(
	std::optional<BattleStartTownPreMergeSnapshot> & pending,
	const BattleStartInfo & setup)
{
	if(!pending)
		return std::nullopt;

	const auto * defenderHero = setup.heroes[BattleSide::DEFENDER];
	if(!setup.town || !defenderHero)
		return std::nullopt;
	if(pending->townId != setup.town->id || pending->defendingHeroId != defenderHero->id)
		return std::nullopt;

	auto result = std::move(pending);
	pending.reset();
	return result;
}
}

BattleProcessor::BattleProcessor(CGameHandler * gameHandler)
	: gameHandler(gameHandler)
	, actionsProcessor(std::make_unique<BattleActionProcessor>(this, gameHandler))
	, flowProcessor(std::make_unique<BattleFlowProcessor>(this, gameHandler))
	, resultProcessor(std::make_unique<BattleResultProcessor>(gameHandler))
	, simulationEvaluator(std::make_unique<BattleSimulation::BattleSimulationEvaluator>())
{
}

BattleProcessor::~BattleProcessor() = default;

void BattleProcessor::engageIntoBattle(PlayerColor player)
{
	//notify interfaces
	PlayerBlocked pb;
	pb.player = player;
	pb.reason = PlayerBlocked::UPCOMING_BATTLE;
	pb.startOrEnd = PlayerBlocked::BLOCKADE_STARTED;
	gameHandler->sendAndApply(pb);
}

void BattleProcessor::restartBattle(const BattleID & battleID, const CArmedInstance *army1, const CArmedInstance *army2, int3 tile,
								const CGHeroInstance *hero1, const CGHeroInstance *hero2, const BattleLayout & layout, const CGTownInstance *town)
{
	restartBattle(
		battleID,
		BattleStartInfo{
			BattleSideArray<const CArmedInstance *>{army1, army2},
			BattleSideArray<const CGHeroInstance *>{hero1, hero2},
			tile,
			layout,
			town
		}
	);
}

void BattleProcessor::restartBattle(const BattleID & battleID, const BattleStartInfo & setup)
{
	BattleStartInfo replaySetup = setup;
	auto battle = gameHandler->gameState().getBattle(battleID);
	if(!replaySetup.townPreMerge)
	{
		auto preMerge = townPreMergeSnapshots.find(battleID);
		if(preMerge != townPreMergeSnapshots.end())
			replaySetup.townPreMerge = preMerge->second;
	}

	auto attackerQuery = gameHandler->queries->topQuery(battle->getSide(BattleSide::ATTACKER).color);
	auto * lastBattleQuery = gameHandler->queries->queryAs<CBattleQuery>(attackerQuery);
	if(!lastBattleQuery)
	{
		auto defenderPlayer = battle->getSide(BattleSide::DEFENDER).color;
		if(defenderPlayer.isValidPlayer())
		{
			auto defenderQuery = gameHandler->queries->topQuery(defenderPlayer);
			lastBattleQuery = gameHandler->queries->queryAs<CBattleQuery>(defenderQuery);
		}
	}

	assert(lastBattleQuery);

	//existing battle query for retying auto-combat
	BattleSideArray<int32_t> manaToRestore{
		battle->getSide(BattleSide::ATTACKER).initialMana,
		battle->getSide(BattleSide::DEFENDER).initialMana
	};
	if(lastBattleQuery)
	{
		for(auto i : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		{
			if(replaySetup.heroes[i])
				manaToRestore[i] = BattleSimulationBatch::getReplayInitialMana(replaySetup.heroes[i], battle->getSide(i).initialMana);
		}

		lastBattleQuery->result = std::nullopt;

		assert(lastBattleQuery->belligerents[BattleSide::ATTACKER] == replaySetup.armies[BattleSide::ATTACKER]);
		assert(lastBattleQuery->belligerents[BattleSide::DEFENDER] == replaySetup.armies[BattleSide::DEFENDER]);
	}

	BattleCancelled bc;
	bc.battleID = battleID;
	gameHandler->sendAndApply(bc);
	resultProcessor->discardBattleResult(battleID);
	townPreMergeSnapshots.erase(battleID);

	for(auto i : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		if(replaySetup.heroes[i])
		{
			SetMana restoreInitialMana;
			restoreInitialMana.val = manaToRestore[i];
			restoreInitialMana.hid = replaySetup.heroes[i]->id;
			restoreInitialMana.mode = ChangeValueMode::ABSOLUTE;
			gameHandler->sendAndApply(restoreInitialMana);
		}
	}

	startBattle(replaySetup);
}

void BattleProcessor::restartBattle(const IBattleInfo & battle)
{
	auto setup = BattleStartInfo::fromBattle(battle);
	auto preMerge = townPreMergeSnapshots.find(battle.getBattleID());
	if(preMerge != townPreMergeSnapshots.end())
		setup.townPreMerge = preMerge->second;
	restartBattle(battle.getBattleID(), setup);
}

BattleSimulation::BattleSimulationResponse BattleProcessor::evaluateBattleSimulation(const BattleSimulation::BattleSimulationRequest & request) const
{
	return simulationEvaluator->evaluate(request);
}

void BattleProcessor::setBattleSimulationRunner(std::shared_ptr<BattleSimulation::IBattleSimulationRunner> runner)
{
	simulationEvaluator->setRunner(std::move(runner));
}

void BattleProcessor::storeBattleSimulationSummary(const BattleSimulation::BattleSimulationRequest & request, const BattleSimulation::BattleSimulationSummary & summary)
{
	simulationEvaluator->storeCachedSummary(request, summary);
}

void BattleProcessor::startBattle(const CArmedInstance *army1, const CArmedInstance *army2, int3 tile,
								const CGHeroInstance *hero1, const CGHeroInstance *hero2, const BattleLayout & layout, const CGTownInstance *town)
{
	startBattle(
		BattleStartInfo{
			BattleSideArray<const CArmedInstance *>{army1, army2},
			BattleSideArray<const CGHeroInstance *>{hero1, hero2},
			tile,
			layout,
			town
		}
	);
}

void BattleProcessor::startBattle(const BattleStartInfo & setup)
{
	BattleStartInfo battleSetup = setup;
	if(!battleSetup.townPreMerge)
		battleSetup.townPreMerge = takeMatchingTownPreMergeSnapshot(nextTownPreMergeSnapshot, setup);

	assert(gameHandler->gameState().getBattle(battleSetup.armies[BattleSide::ATTACKER]->getOwner()) == nullptr);
	assert(gameHandler->gameState().getBattle(battleSetup.armies[BattleSide::DEFENDER]->getOwner()) == nullptr);

	auto battleID = setupBattle(battleSetup.tile, battleSetup.armies, battleSetup.heroes, battleSetup.layout, battleSetup.town); //initializes stacks, places creatures on battlefield, blocks and informs player interfaces
	if(battleSetup.townPreMerge)
		townPreMergeSnapshots[battleID] = *battleSetup.townPreMerge;

	const auto * battle = gameHandler->gameState().getBattle(battleID);
	assert(battle);

	//add battle bonuses based from player state only when attacks neutral creatures
	const auto * attackerInfo = gameHandler->gameInfo().getPlayerState(battleSetup.armies[BattleSide::ATTACKER]->getOwner(), false);
	if(attackerInfo && !battleSetup.armies[BattleSide::DEFENDER]->getOwner().isValidPlayer())
	{
		for(const auto & bonus : attackerInfo->battleBonuses)
		{
			GiveBonus giveBonus(GiveBonus::ETarget::OBJECT);
			giveBonus.id = battleSetup.heroes[BattleSide::ATTACKER]->id;
			giveBonus.bonus = bonus;
			gameHandler->sendAndApply(giveBonus);
		}
	}

	auto attackerQuery = gameHandler->queries->topQuery(battle->getSide(BattleSide::ATTACKER).color);
	auto * topBattleQuery = gameHandler->queries->queryAs<CBattleQuery>(attackerQuery);
	if(!topBattleQuery && battleSetup.armies[BattleSide::DEFENDER]->getOwner().isValidPlayer())
	{
		auto defenderQuery = gameHandler->queries->topQuery(battle->getSide(BattleSide::DEFENDER).color);
		topBattleQuery = gameHandler->queries->queryAs<CBattleQuery>(defenderQuery);
	}
	if (topBattleQuery)
	{
		topBattleQuery->battleID = battleID;
	}
	else
	{
		auto newBattleQuery = std::make_shared<CBattleQuery>(gameHandler, battle);
		gameHandler->queries->addQuery(newBattleQuery);
	}

	flowProcessor->onBattleStarted(*battle);
}

void BattleProcessor::startBattle(const CArmedInstance *army1, const CArmedInstance *army2)
{
	startBattle(army1, army2, army2->visitablePos(),
		army1->ID == Obj::HERO ? dynamic_cast<const CGHeroInstance*>(army1) : nullptr,
		army2->ID == Obj::HERO ? dynamic_cast<const CGHeroInstance*>(army2) : nullptr,
		BattleLayout::createDefaultLayout(gameHandler->gameInfo(), army1, army2),
		nullptr);
}

void BattleProcessor::setNextBattleTownPreMergeState(const CGTownInstance * town, const CGHeroInstance * defendingHero)
{
	if(!town || !defendingHero)
	{
		nextTownPreMergeSnapshot.reset();
		return;
	}

	nextTownPreMergeSnapshot = makeBattleStartTownPreMergeSnapshot(town, defendingHero);
}

const BattleStartTownPreMergeSnapshot * BattleProcessor::getTownPreMergeSnapshot(const BattleID & battleID) const
{
	auto found = townPreMergeSnapshots.find(battleID);
	if(found == townPreMergeSnapshots.end())
		return nullptr;

	return &found->second;
}

BattleID BattleProcessor::setupBattle(int3 tile, BattleSideArray<const CArmedInstance *> armies, BattleSideArray<const CGHeroInstance *> heroes, const BattleLayout & layout, const CGTownInstance *town)
{
	const auto & t = *gameHandler->gameInfo().getTile(tile);
	TerrainId terrain = t.getTerrainID();

	BattleField battlefieldType = gameHandler->gameState().battleGetBattlefieldType(tile, gameHandler->getRandomGenerator());

	// The battle may take place on a terrain dictated by an object rather than the map tile:
	// a town siege uses the town's native terrain, and objects such as an abandoned mine can
	// force e.g. subterranean terrain. In that case the battlefield is the object's fixed one if it
	// defines one, otherwise it is selected from that terrain - keeping terrain, battlefield and
	// obstacles consistent.
	// A town's battle terrain is dictated only through the explicit 'town' parameter; a null town
	// means an outside/field battle that uses the map tile terrain, so the town object sitting on
	// the battle tile must be ignored here.
	const CGObjectInstance * topObject = nullptr;
	if (!town && !t.visitableObjects.empty())
	{
		const auto * tileObject = gameHandler->gameInfo().getObjInstance(t.visitableObjects.front());
		if (tileObject && tileObject->ID != Obj::TOWN)
			topObject = tileObject;
	}

	TerrainId forcedTerrain = town ? town->getBattleTerrain() : (topObject ? topObject->getBattleTerrain() : TerrainId::NONE);

	if (forcedTerrain != TerrainId::NONE)
	{
		terrain = forcedTerrain;
		BattleField forcedBattlefield = topObject ? topObject->getBattlefield() : BattleField::NONE;
		if (forcedBattlefield != BattleField::NONE)
			battlefieldType = forcedBattlefield; // object defines a fixed battlefield explicitly
		else
		{
			const TerrainType * terrainData = LIBRARY->terrainTypeHandler->getById(terrain);
			battlefieldType = BattleField(*RandomGeneratorUtil::nextItem(terrainData->battleFields, gameHandler->getRandomGenerator()));
		}
	}
	else if (gameHandler->gameState().getMap().isCoastalTile(tile)) //coastal tile is always ground
		terrain = ETerrainId::SAND;
	else if (heroes[BattleSide::ATTACKER] && heroes[BattleSide::ATTACKER]->inBoat() && heroes[BattleSide::DEFENDER] && heroes[BattleSide::DEFENDER]->inBoat())
		battlefieldType = BattleField(*LIBRARY->identifiers()->getIdentifier("core", "battlefield.ship_to_ship"));

	//send info about battles
	BattleStart bs;
	bs.info = BattleInfo::setupBattle(&gameHandler->gameInfo(), tile, terrain, battlefieldType, armies, heroes, layout, town);
	bs.battleID = gameHandler->gameState().nextBattleID;

	engageIntoBattle(bs.info->getSide(BattleSide::ATTACKER).color);
	engageIntoBattle(bs.info->getSide(BattleSide::DEFENDER).color);

	auto attackerQuery = gameHandler->queries->topQuery(bs.info->getSide(BattleSide::ATTACKER).color);
	auto * topBattleQuery = gameHandler->queries->queryAs<CBattleQuery>(attackerQuery);
	bool isDefenderHuman = bs.info->getSide(BattleSide::DEFENDER).color.isValidPlayer() && gameHandler->gameInfo().getPlayerState(bs.info->getSide(BattleSide::DEFENDER).color)->isHuman();
	if(!topBattleQuery && isDefenderHuman)
	{
		auto defenderQuery = gameHandler->queries->topQuery(bs.info->getSide(BattleSide::DEFENDER).color);
		topBattleQuery = gameHandler->queries->queryAs<CBattleQuery>(defenderQuery);
	}

	bool isAttackerHuman = gameHandler->gameInfo().getPlayerState(bs.info->getSide(BattleSide::ATTACKER).color)->isHuman();

	bool onlyOnePlayerHuman = isDefenderHuman != isAttackerHuman;
	bs.info->replayAllowed = topBattleQuery == nullptr && onlyOnePlayerHuman;

	gameHandler->sendAndApply(bs);

	return bs.battleID;
}

bool BattleProcessor::checkBattleStateChanges(const CBattleInfoCallback & battle)
{
	//check if drawbridge state need to be changes
	if (battle.battleGetFortifications().wallsHealth > 0)
		updateGateState(battle);

	if (resultProcessor->battleIsEnding(battle))
		return true;

	//check if battle ended
	if (auto result = battle.battleIsFinished())
	{
		setBattleResult(battle, EBattleResult::NORMAL, *result);
		return true;
	}

	return false;
}

void BattleProcessor::updateGateState(const CBattleInfoCallback & battle)
{
	// GATE_BRIDGE - leftmost tile, located over moat
	// GATE_OUTER - central tile, mostly covered by gate image
	// GATE_INNER - rightmost tile, inside the walls

	// GATE_OUTER or GATE_INNER:
	// - if defender moves unit on these tiles, bridge will open
	// - if there is a creature (dead or alive) on these tiles, bridge will always remain open
	// - blocked to attacker if bridge is closed

	// GATE_BRIDGE
	// - if there is a unit or corpse here, bridge can't open (and can't close in fortress)
	// - if Force Field is cast here, bridge can't open (but can close, in any town)
	// - deals moat damage to attacker if bridge is closed (fortress only)

	bool hasForceFieldOnBridge = !battle.battleGetAllObstaclesOnPos(BattleHex(BattleHex::GATE_BRIDGE), true).empty();
	bool hasStackAtGateInner   = battle.battleGetUnitByPos(BattleHex(BattleHex::GATE_INNER), false) != nullptr;
	bool hasStackAtGateOuter   = battle.battleGetUnitByPos(BattleHex(BattleHex::GATE_OUTER), false) != nullptr;
	bool hasStackAtGateBridge  = battle.battleGetUnitByPos(BattleHex(BattleHex::GATE_BRIDGE), false) != nullptr;
	bool hasWideMoat           = vstd::contains_if(battle.battleGetAllObstaclesOnPos(BattleHex(BattleHex::GATE_BRIDGE), false), [](const std::shared_ptr<const CObstacleInstance> & obst)
	{
		return obst->obstacleType == CObstacleInstance::MOAT;
	});

	BattleUpdateGateState db;
	db.state = battle.battleGetGateState();
	db.battleID = battle.getBattle()->getBattleID();

	if (battle.battleGetWallState(EWallPart::GATE) == EWallState::DESTROYED)
	{
		db.state = EGateState::DESTROYED;
	}
	else if (db.state == EGateState::OPENED)
	{
		bool hasStackOnLongBridge = hasStackAtGateBridge && hasWideMoat;
		bool gateCanClose = !hasStackAtGateInner && !hasStackAtGateOuter && !hasStackOnLongBridge;

		if (gateCanClose)
			db.state = EGateState::CLOSED;
		else
			db.state = EGateState::OPENED;
	}
	else // CLOSED or BLOCKED
	{
		bool gateBlocked = hasForceFieldOnBridge || hasStackAtGateBridge;

		if (gateBlocked)
			db.state = EGateState::BLOCKED;
		else
			db.state = EGateState::CLOSED;
	}

	if (db.state != battle.battleGetGateState())
		gameHandler->sendAndApply(db);
}

bool BattleProcessor::makePlayerBattleAction(const BattleID & battleID, PlayerColor player, const BattleAction &ba)
{
	const auto * battle = gameHandler->gameState().getBattle(battleID);

	if (!battle)
		return false;

	bool result = actionsProcessor->makePlayerBattleAction(*battle, player, ba);
	if (gameHandler->gameState().getBattle(battleID) != nullptr && !resultProcessor->battleIsEnding(*battle))
		flowProcessor->onActionMade(*battle, ba);
	return result;
}

void BattleProcessor::setBattleResult(const CBattleInfoCallback & battle, EBattleResult resultType, BattleSide victoriusSide)
{
	resultProcessor->setBattleResult(battle, resultType, victoriusSide);
	resultProcessor->endBattle(battle);
}

bool BattleProcessor::makeAutomaticBattleAction(const CBattleInfoCallback & battle, const BattleAction &ba)
{
	return actionsProcessor->makeAutomaticBattleAction(battle, ba);
}

void BattleProcessor::endBattleConfirm(const BattleID & battleID)
{
	auto battle = gameHandler->gameState().getBattle(battleID);
	assert(battle);

	if (!battle)
		return;

	resultProcessor->endBattleConfirm(*battle);
}

void BattleProcessor::battleFinalize(const BattleID & battleID, const BattleResult &result)
{
	resultProcessor->battleFinalize(battleID, result);
}
