/*
 * FailedRouteRecoveryTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "AI/Nullkiller2/AIGateway.h"
#include "AI/Nullkiller2/Behaviors/EscapeBehavior.h"
#include "AI/Nullkiller2/Engine/Nullkiller.h"
#include "AI/Nullkiller2/Goals/Composition.h"
#include "AI/Nullkiller2/Goals/ExecuteHeroChain.h"

#include "mock/TinyH3MBuilder.h"
#include "nullkiller2/NullkillerTest.h"

#include "lib/callback/CCallback.h"
#include "lib/callback/IClient.h"
#include "lib/gameState/CGameState.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/networkPacks/PacksForClient.h"
#include "lib/networkPacks/PacksForServer.h"

namespace
{
const PlayerColor PLAYER(0);
const PlayerColor ENEMY(1);

class MovementClient : public IClient
{
public:
	explicit MovementClient(CGameState & state, bool removeMovingHero = false)
		: gameState(state)
		, removeMovingHero(removeMovingHero)
	{
	}

	std::optional<BattleAction> makeSurrenderRetreatDecision(
		PlayerColor,
		const BattleID &,
		const BattleStateInfoForRetreat &) override
	{
		return std::nullopt;
	}

	int sendRequest(const CPackForServer & request, PlayerColor player, bool) override
	{
		if(removeMovingHero)
		{
			if(const auto * movement = dynamic_cast<const MoveHero *>(&request))
			{
				RemoveObject removeHero(movement->hid, player);
				gameState.apply(removeHero);
				removeMovingHero = false;
			}
		}
		return ++lastRequestID;
	}

	CGameState & gameState;
	bool removeMovingHero;
	int lastRequestID = 0;
};

const NK2AI::Goals::ExecuteHeroChain * findHeroChain(
	const NK2AI::Goals::TGoalVec & goals,
	const CGHeroInstance * hero)
{
	for(const auto & goal : goals)
	{
		const auto * chain = dynamic_cast<const NK2AI::Goals::ExecuteHeroChain *>(goal.get());
		if(chain && chain->getHero() == hero)
			return chain;
	}

	return nullptr;
}

NK2AI::AIPath singleHeroPath(const CGHeroInstance & hero, const int3 & destination)
{
	NK2AI::AIPath path;
	path.targetHero = &hero;
	path.heroArmy = &hero;
	path.exchangeCount = 1;
	path.chainMask = 1;

	NK2AI::AIPathNodeInfo node;
	node.coord = destination;
	node.layer = EPathfindingLayer::LAND;
	node.targetHero = &hero;
	node.parentIndex = -1;
	node.chainMask = 1;
	node.turns = 0;
	path.nodes.push_back(node);
	return path;
}

class FailedEscapeRouteTest : public NullkillerTest
{
protected:
	void startGame()
	{
		TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
		builder
			.size(36, false)
			.playerActive(PLAYER)
			.playerActive(ENEMY)
			.hero({5, 5, 0}, HeroTypeID(0), PLAYER)
			.heroGarrison({{CreatureID(0), 1}})
			.hero({7, 5, 0}, HeroTypeID(1), ENEMY)
			.heroGarrison({{CreatureID(13), 20}});

		startWithMap(std::move(builder));
	}
};
}

TEST_F(FailedEscapeRouteTest, replanningAvoidsFailedMultiNodeEscapeRoute)
{
	startGame();
	revealMap(PLAYER);

	auto * escapingHero = findHeroByOwner(PLAYER);
	auto * threateningHero = findHeroByOwner(ENEMY);
	ASSERT_NE(escapingHero, nullptr);
	ASSERT_NE(threateningHero, nullptr);
	escapingHero->setMovementPoints(2000);
	threateningHero->setMovementPoints(1000);

	MovementClient client(*gameState);
	auto gateway = makeGateway(PLAYER, &client);
	auto & ai = *gateway->nullkiller;
	NK2AI::NullkillerTestAccess::prepareState(ai);

	NK2AI::Goals::EscapeBehavior escape;
	const auto initialGoals = escape.decompose(&ai);
	const auto * initialChain = findHeroChain(initialGoals, escapingHero);
	ASSERT_NE(initialChain, nullptr);
	const int3 failedDestination = initialChain->getPath().targetTile();

	const auto failedTask = NK2AI::Goals::taskptr(*initialChain);
	ASSERT_FALSE(NK2AI::NullkillerTestAccess::executeTask(ai, failedTask));

	const auto replannedGoals = escape.decompose(&ai);
	bool foundAlternative = false;
	for(const auto & goal : replannedGoals)
	{
		const auto * replannedChain = dynamic_cast<const NK2AI::Goals::ExecuteHeroChain *>(goal.get());
		if(!replannedChain || replannedChain->getHero() != escapingHero)
			continue;

		foundAlternative = true;
		EXPECT_NE(replannedChain->getPath().targetTile(), failedDestination);
	}
	EXPECT_TRUE(foundAlternative) << "another safe escape destination should remain available";
}

TEST_F(FailedEscapeRouteTest, failedDestinationDoesNotRejectAPathUsingItAsWaypoint)
{
	startGame();
	revealMap(PLAYER);
	auto * hero = findHeroByOwner(PLAYER);
	ASSERT_NE(hero, nullptr);
	hero->setMovementPoints(2000);

	MovementClient client(*gameState);
	auto gateway = makeGateway(PLAYER, &client);
	auto & ai = *gateway->nullkiller;
	NK2AI::NullkillerTestAccess::prepareState(ai);

	const auto failedPath = singleHeroPath(*hero, hero->visitablePos() + int3(1, 0, 0));
	ASSERT_FALSE(NK2AI::NullkillerTestAccess::executeTask(
		ai,
		NK2AI::Goals::taskptr(NK2AI::Goals::ExecuteHeroChain(failedPath))));
	EXPECT_TRUE(ai.isPathRejected(failedPath));

	auto otherPath = singleHeroPath(*hero, hero->visitablePos() + int3(2, 0, 0));
	otherPath.nodes.push_back(failedPath.nodes.front());
	EXPECT_FALSE(ai.isPathRejected(otherPath));
}

TEST_F(FailedEscapeRouteTest, compositionRecordsFailureOfTheChainThatFailed)
{
	startGame();
	revealMap(PLAYER);
	auto * hero = findHeroByOwner(PLAYER);
	ASSERT_NE(hero, nullptr);
	hero->setMovementPoints(2000);

	MovementClient client(*gameState);
	auto gateway = makeGateway(PLAYER, &client);
	auto & ai = *gateway->nullkiller;
	NK2AI::NullkillerTestAccess::prepareState(ai);

	const auto completedPath = singleHeroPath(*hero, hero->visitablePos());
	const auto failedPath = singleHeroPath(*hero, hero->visitablePos() + int3(1, 0, 0));
	NK2AI::Goals::Composition composition;
	composition.addNextSequence({
		NK2AI::Goals::sptr(NK2AI::Goals::ExecuteHeroChain(completedPath)),
		NK2AI::Goals::sptr(NK2AI::Goals::ExecuteHeroChain(failedPath))});

	ASSERT_FALSE(NK2AI::NullkillerTestAccess::executeTask(
		ai,
		NK2AI::Goals::taskptr(composition)));
	EXPECT_FALSE(ai.isPathRejected(completedPath));
	EXPECT_TRUE(ai.isPathRejected(failedPath));
}

TEST_F(FailedEscapeRouteTest, losingHeroDuringMovementDoesNotCrashFailureHandling)
{
	startGame();
	revealMap(PLAYER);
	auto * hero = findHeroByOwner(PLAYER);
	ASSERT_NE(hero, nullptr);
	hero->setMovementPoints(2000);

	MovementClient client(*gameState, true);
	auto gateway = makeGateway(PLAYER, &client);
	auto & ai = *gateway->nullkiller;
	NK2AI::NullkillerTestAccess::prepareState(ai);
	const auto path = singleHeroPath(*hero, hero->visitablePos() + int3(1, 0, 0));
	bool completed = true;

	EXPECT_NO_THROW(completed = NK2AI::NullkillerTestAccess::executeTask(
		ai,
		NK2AI::Goals::taskptr(NK2AI::Goals::ExecuteHeroChain(path))));
	EXPECT_FALSE(completed);
}
