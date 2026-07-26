/*
 * TownHeroAssignmentTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "mock/TinyH3MBuilder.h"
#include "mock/TinyMapGameTest.h"

#include "lib/gameState/CGameState.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/mapObjects/CGTownInstance.h"
#include "lib/mapping/CMap.h"

namespace
{
const PlayerColor PLAYER = PlayerColor(0);

class TownHeroAssignmentTest : public TinyMapGameTest
{
public:
	void startGame(int townCount = 1, int heroCount = 1)
	{
		const std::array townPositions = {
			int3(9, 5, 0),
			int3(20, 5, 0),
			int3(30, 5, 0)
		};
		const std::array heroPositions = {
			int3(5, 5, 0),
			int3(6, 5, 0)
		};

		TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
		builder
			.size(36, false)
			.playerActive(PLAYER);

		for(int index = 0; index < townCount; ++index)
			builder.randomTown(townPositions.at(index), PLAYER);

		for(int index = 0; index < heroCount; ++index)
			builder.hero(heroPositions.at(index), HeroTypeID(index), PLAYER);

		startWithMap(std::move(builder));
		towns = findAll<CGTownInstance>();
		heroes = findAll<CGHeroInstance>();
	}

	CGTownInstance * town(size_t index = 0)
	{
		return towns.at(index);
	}

	CGHeroInstance * hero(size_t index = 0)
	{
		return heroes.at(index);
	}

private:
	std::vector<CGTownInstance *> towns;
	std::vector<CGHeroInstance *> heroes;
};
}

TEST_F(TownHeroAssignmentTest, replacingVisitorDetachesDisplacedHero)
{
	startGame(1, 2);

	town()->setVisitingHero(hero(0));
	town()->setVisitingHero(hero(1));

	EXPECT_EQ(town()->getVisitingHero(), hero(1));
	EXPECT_EQ(hero(0)->getVisitedTown(), nullptr);
	EXPECT_EQ(hero(1)->getVisitedTown(), town());
}

TEST_F(TownHeroAssignmentTest, assigningVisitorToSecondTownIsRejected)
{
	startGame(2);

	town(0)->setVisitingHero(hero(0));

	EXPECT_THROW(town(1)->setVisitingHero(hero(0)), std::runtime_error);
	EXPECT_EQ(town(0)->getVisitingHero(), hero(0));
	EXPECT_EQ(town(1)->getVisitingHero(), nullptr);
	EXPECT_EQ(hero(0)->getVisitedTown(), town(0));
}

TEST_F(TownHeroAssignmentTest, assigningGarrisonToSecondTownIsRejected)
{
	startGame(2);

	town(0)->setGarrisonedHero(hero(0));

	EXPECT_THROW(town(1)->setGarrisonedHero(hero(0)), std::runtime_error);
	EXPECT_EQ(town(0)->getGarrisonHero(), hero(0));
	EXPECT_EQ(town(1)->getGarrisonHero(), nullptr);
	EXPECT_EQ(hero(0)->getVisitedTown(), town(0));
	EXPECT_TRUE(hero(0)->isGarrisoned());
}
