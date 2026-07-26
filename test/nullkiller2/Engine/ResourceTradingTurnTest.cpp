/*
 * ResourceTradingTurnTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "mock/TinyH3MBuilder.h"
#include "mock/TestGameServer.h"
#include "nullkiller2/NullkillerTest.h"

#include "lib/CPlayerState.h"
#include "lib/constants/NumericConstants.h"
#include "lib/gameState/CGameState.h"
#include "lib/gameState/TavernHeroesPool.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/mapObjects/CGTownInstance.h"
#include "lib/mapObjects/army/CSimpleArmy.h"
#include "lib/mapping/CMap.h"
#include "lib/networkPacks/PacksForClient.h"

namespace
{
const PlayerColor PLAYER = PlayerColor(0);
const PlayerColor ENEMY = PlayerColor(1);

class ResourceTradingTurnTest : public NullkillerTest
{
public:
	void startGame()
	{
		TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
		builder
			.size(36, false)
			.playerActive(PLAYER)
			.playerActive(ENEMY)
			.randomTown({9, 5, 0}, PLAYER)
			.hero({25, 25, 0}, HeroTypeID(0), PLAYER)
			.randomTown({30, 30, 0}, ENEMY)
			.hero({30, 25, 0}, HeroTypeID(1), ENEMY);

		startWithMap(std::move(builder), EMapDifficulty::IMPOSSIBLE);
	}

	void prepareTradingCycle(CGTownInstance & town)
	{
		NewStructures structures;
		structures.tid = town.id;
		for(const auto & building : town.getTown()->buildings)
			structures.bid.insert(building.first);
		gameState->apply(structures);

		for(auto & creatureLevel : town.creatures)
		{
			if(!creatureLevel.second.empty())
				creatureLevel.first = 100;
		}

		auto & resources = gameState->players.at(PLAYER).resources;
		for(int resource = 0; resource < GameConstants::RESOURCE_QUANTITY; ++resource)
			resources[resource] = resource == GameResID::GOLD ? 0 : 1000000;

		revealMap(PLAYER);

		CSimpleArmy emptyArmy;
		gameState->heroesPool->setHeroForPlayer(
			PLAYER,
			TavernHeroSlot::NATIVE,
			HeroTypeID::NONE,
			emptyArmy,
			TavernSlotRole::NONE,
			false);
		gameState->heroesPool->setHeroForPlayer(
			PLAYER,
			TavernHeroSlot::RANDOM,
			HeroTypeID::NONE,
			emptyArmy,
			TavernSlotRole::NONE,
			false);
	}
};
}

TEST_F(ResourceTradingTurnTest, tradesForArmyOnlyOncePerTurn)
{
	startGame();

	auto * town = findFirst<CGTownInstance>();
	ASSERT_NE(town, nullptr);
	auto * hero = findHeroByOwner(PLAYER);
	ASSERT_NE(hero, nullptr);
	prepareTradingCycle(*town);
	ChangeObjPos moveHero;
	moveHero.objid = hero->id;
	moveHero.nPos = hero->convertFromVisitablePos(town->visitablePos());
	moveHero.initiator = PLAYER;
	gameState->apply(moveHero);
	town->setVisitingHero(hero);
	SetMovePoints stopHero(hero->id, 0);
	gameState->apply(stopHero);
	ServerApplyingClient client(gameState);

	auto gateway = makeGateway(PLAYER, &client);

	gateway->nullkiller->makeTurn();

	EXPECT_GT(client.getRecruitmentRequests(), 0)
		<< "the fixture must consume traded gold by buying army";
	EXPECT_GT(client.getTradeRequests(), 0)
		<< "the fixture must fund army purchases through a marketplace";
	EXPECT_EQ(client.getTradePhases(), 1)
		<< "army purchases must not reopen the turn's resource-trading budget";
}
