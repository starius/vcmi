/*
 * QuestActionTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "AI/Nullkiller2/Pathfinding/AIPathfinder.h"
#include "AI/Nullkiller2/Pathfinding/Actions/QuestAction.h"

#include "mock/TinyH3MBuilder.h"
#include "nullkiller2/NullkillerTest.h"

#include "lib/callback/CCallback.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/networkPacks/PacksForClient.h"

namespace
{
const PlayerColor PLAYER = PlayerColor(0);
const int3 HERO_POS(4, 5, 0);
const int3 GATE_POS(4, 6, 0);
const int3 TARGET_POS(4, 8, 0);

TinyH3M::TinyH3MBuilder makeGateMap()
{
	TinyH3M::TinyH3MBuilder builder(EMapFormat::HOTA);
	builder
		.hotaVersion(3)
		.size(36, false)
		.name("NK2QuestGate")
		.playerActive(PLAYER)
		.hero(HERO_POS + int3(1, 0, 0), HeroTypeID(0), PLAYER)
		.heroGarrison({{CreatureID(27), 1}})
		.questGate(GATE_POS + int3(1, 0, 0), TinyH3M::TinyH3MBuilder::missionLevel(1));

	return builder;
}

TinyH3M::TinyH3MBuilder makeQuestGuardMap()
{
	TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
	builder
		.size(36, false)
		.playerActive(PLAYER)
		.hero(HERO_POS + int3(1, 0, 0), HeroTypeID(0), PLAYER)
		.heroGarrison({{CreatureID(27), 1}})
		.questGuard(GATE_POS + int3(1, 0, 0), TinyH3M::TinyH3MBuilder::missionLevel(1));
	return builder;
}

class Nullkiller2_Pathfinding_QuestAction : public NullkillerTest
{
public:
	void revealMapAndEncloseHero()
	{
		revealMap(PLAYER);

		for(int dx = -1; dx <= 1; ++dx)
		{
			for(int dy = -1; dy <= 1; ++dy)
			{
				const int3 tile = HERO_POS + int3(dx, dy, 0);
				if(tile != HERO_POS && tile != GATE_POS)
					map->getTile(tile).terrainType = TerrainId(ETerrainId::ROCK);
			}
		}
	}

	std::vector<NK2AI::AIPath> updateAndGetPaths(const int3 & destination)
	{
		auto * hero = findHeroByOwner(PLAYER);
		EXPECT_NE(hero, nullptr);
		if(!hero)
			return {};

		gateway = makeGateway(PLAYER);
		NK2AI::HeroMap<NK2AI::HeroRole> heroes;
		heroes[hero] = NK2AI::HeroRole::MAIN;
		NK2AI::PathfinderSettings settings;
		settings.allowBypassObjects = true;
		gateway->nullkiller->pathfinder->updatePaths(heroes, settings);
		return gateway->nullkiller->pathfinder->getPathInfo(destination);
	}

	std::unique_ptr<NK2AI::AIGateway> gateway;
};
}

TEST_F(Nullkiller2_Pathfinding_QuestAction, addsInitialVisitBeforeCrossingUnopenedGate)
{
	startWithMap(makeGateMap());
	revealMapAndEncloseHero();

	auto * hero = findHeroByOwner(PLAYER);
	ASSERT_NE(hero, nullptr);
	ASSERT_EQ(hero->visitablePos(), HERO_POS);
	const auto gateObject = std::find_if(
		map->objects.begin(),
		map->objects.end(),
		[](const std::shared_ptr<CGObjectInstance> & object)
		{
			return object && object->visitablePos() == GATE_POS;
		});
	ASSERT_NE(gateObject, map->objects.end());
	ASSERT_TRUE((*gateObject)->passableFor(hero));
	const auto gatePaths = updateAndGetPaths(GATE_POS);
	ASSERT_FALSE(gatePaths.empty()) << "the hero must be able to reach the gate";
	const auto paths = gateway->nullkiller->pathfinder->getPathInfo(TARGET_POS);
	ASSERT_FALSE(paths.empty()) << "the satisfiable gate should not block planning";

	const auto & path = paths.front();
	const auto actionNode = std::find_if(
		path.nodes.begin(),
		path.nodes.end(),
		[](const NK2AI::AIPathNodeInfo & node)
		{
			return dynamic_cast<const NK2AI::AIPathfinding::QuestAction *>(node.specialAction.get());
		});

	ASSERT_NE(actionNode, path.nodes.end())
		<< "an unopened gate must be visited before planning movement through it";
	EXPECT_FALSE(actionNode->actionIsBlocked);
}

TEST_F(Nullkiller2_Pathfinding_QuestAction, rejectsActionAfterQuestObjectWasRemoved)
{
	startWithMap(makeGateMap());

	auto * hero = findHeroByOwner(PLAYER);
	ASSERT_NE(hero, nullptr);
	const auto gateObject = std::find_if(
		map->objects.begin(),
		map->objects.end(),
		[](const std::shared_ptr<CGObjectInstance> & object)
		{
			return object && object->visitablePos() == GATE_POS;
		});
	ASSERT_NE(gateObject, map->objects.end());

	NK2AI::AIPathfinding::QuestAction questAction(QuestInfo((*gateObject)->id));
	const auto gateway = makeGateway(PLAYER);
	const ObjectInstanceID gateID = (*gateObject)->id;
	RemoveObject removeObject(gateID, PLAYER);
	gameState->apply(removeObject);
	ASSERT_EQ(gameState->getObjInstance(gateID), nullptr);

	EXPECT_THROW(
		questAction.execute(gateway.get(), hero),
		NK2AI::cannotFulfillGoalException);
	EXPECT_FALSE(questAction.needsInitialVisit(gateway->nullkiller.get(), hero));
}

TEST_F(Nullkiller2_Pathfinding_QuestAction, satisfiedQuestGuardDoesNotUseGateInitialVisitAction)
{
	startWithMap(makeQuestGuardMap());
	revealMapAndEncloseHero();

	const auto paths = updateAndGetPaths(GATE_POS);
	ASSERT_FALSE(paths.empty());
	for(const auto & path : paths)
	{
		EXPECT_FALSE(std::ranges::any_of(path.nodes, [](const NK2AI::AIPathNodeInfo & node)
		{
			return dynamic_cast<const NK2AI::AIPathfinding::QuestAction *>(node.specialAction.get());
		}));
	}
}
