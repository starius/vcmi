/*
 * GatherArmyBehaviorTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "AI/Nullkiller2/AIGateway.h"
#include "AI/Nullkiller2/Behaviors/GatherArmyBehavior.h"
#include "AI/Nullkiller2/Goals/Composition.h"
#include "AI/Nullkiller2/Pathfinding/AIPathfinder.h"

#include "mock/TinyH3MBuilder.h"
#include "mock/TestGameServer.h"
#include "nullkiller2/NullkillerTest.h"

#include "lib/CPlayerState.h"
#include "lib/callback/CCallback.h"
#include "lib/gameState/CGameState.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/mapObjects/CGTownInstance.h"
#include "lib/mapping/CMap.h"
#include "lib/networkPacks/PacksForClient.h"

namespace
{
const PlayerColor PLAYER = PlayerColor(0);
const PlayerColor ENEMY = PlayerColor(1);

TinyH3M::TinyH3MBuilder makeGarrisonUpgradeMap()
{
	TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
	builder
		.size(36, false)
		.name("NK2GarrisonUpgrade")
		.playerActive(PLAYER)
		.playerActive(ENEMY)
		.randomTown({9, 5, 0}, PLAYER)
		.hero({5, 5, 0}, HeroTypeID(0), PLAYER)
		.randomTown({30, 30, 0}, ENEMY);

	return builder;
}

struct UpgradeCreatures
{
	CreatureID base;
	CreatureID upgraded;
};

class Nullkiller2_Behaviors_GatherArmyBehavior : public NullkillerTest
{
public:
	void putHeroInGarrison(const CGHeroInstance & hero, const CGTownInstance & town)
	{
		ChangeObjPos moveHero;
		moveHero.objid = hero.id;
		moveHero.nPos = town.visitablePos();
		moveHero.initiator = PLAYER;
		gameState->apply(moveHero);

		SetHeroesInTown setHeroes;
		setHeroes.tid = town.id;
		setHeroes.visiting = ObjectInstanceID::NONE;
		setHeroes.garrison = hero.id;
		gameState->apply(setHeroes);
	}

	std::optional<UpgradeCreatures> addUpgradeableArmy(CGHeroInstance & hero, CGTownInstance & town)
	{
		const auto & townCreatures = town.getTown()->creatures;
		const auto level = std::find_if(
			townCreatures.begin(),
			townCreatures.end(),
			[](const std::vector<CreatureID> & creatures)
			{
				return creatures.size() > 1;
			});
		if(level == townCreatures.end())
			return std::nullopt;

		const auto levelIndex = std::distance(townCreatures.begin(), level);
		const CreatureID baseCreature = level->front();
		const CreatureID upgradedCreature = level->at(1);

		town.addBuilding(BuildingID::getDwellingFromLevel(levelIndex, 1));
		town.creatures.at(levelIndex).second.push_back(upgradedCreature);

		hero.clearSlots();
		if(!hero.setCreature(SlotID(0), baseCreature, 1000))
			return std::nullopt;
		gameState->players.at(PLAYER).resources += 1000000;
		return UpgradeCreatures{baseCreature, upgradedCreature};
	}

	std::unique_ptr<NK2AI::AIGateway> makePreparedGateway(IClient * client)
	{
		auto gateway = makeGateway(PLAYER, client);

		gateway->nullkiller->heroManager->update();
		NK2AI::PathfinderSettings pathfinderSettings;
		pathfinderSettings.useHeroChain = true;
		gateway->nullkiller->pathfinder->updatePaths(
			gateway->nullkiller->getHeroesForPathfinding(),
			pathfinderSettings);

		return gateway;
	}
};
}

TEST_F(Nullkiller2_Behaviors_GatherArmyBehavior, garrisonUpgradePlanChangesArmyAndTownState)
{
	startWithMap(makeGarrisonUpgradeMap());

	auto * hero = findHeroByOwner(PLAYER);
	auto * town = findFirst<CGTownInstance>();
	ASSERT_NE(hero, nullptr);
	ASSERT_NE(town, nullptr);

	putHeroInGarrison(*hero, *town);
	const auto creatures = addUpgradeableArmy(*hero, *town);
	ASSERT_TRUE(creatures.has_value()) << "test town must provide an upgraded creature";
	ASSERT_EQ(hero->getStackPtr(SlotID(0))->getCreatureID(), creatures->base);

	ServerApplyingClient client(gameState);
	const auto gateway = makePreparedGateway(&client);
	const auto tasks = NK2AI::Goals::GatherArmyBehavior().decompose(gateway->nullkiller.get());

	NK2AI::Goals::TSubgoal upgradePlan;
	for(const auto & task : tasks)
	{
		if(task->goalType != NK2AI::Goals::COMPOSITION)
			continue;

		const auto steps = task->decompose(gateway->nullkiller.get());
		if(std::ranges::any_of(
			steps,
			[](const NK2AI::Goals::TSubgoal & step)
			{
				return step->goalType == NK2AI::Goals::ARMY_UPGRADE;
			}))
		{
			upgradePlan = task;
			break;
		}
	}

	ASSERT_NE(upgradePlan, nullptr) << "garrison army should produce an upgrade plan";
	NK2AI::Goals::taskptr(*upgradePlan)->accept(gateway.get());
	ASSERT_EQ(town->getVisitingHero(), hero);
	ASSERT_EQ(town->getGarrisonHero(), nullptr);

	EXPECT_EQ(hero->getStackPtr(SlotID(0))->getCreatureID(), creatures->upgraded);
	EXPECT_EQ(client.getGarrisonSwapRequests(), 1);
	EXPECT_GT(client.getUpgradeRequests(), 0);
}
