/*
 * EscapeBehaviorTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "AI/Nullkiller2/Behaviors/EscapeBehavior.h"
#include "AI/Nullkiller2/Engine/PriorityEvaluator.h"
#include "AI/Nullkiller2/Goals/ExecuteHeroChain.h"

#include "mock/TinyH3MBuilder.h"
#include "nullkiller2/NullkillerTest.h"

#include "lib/gameState/CGameState.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/mapObjects/CGTownInstance.h"
#include "lib/networkPacks/PacksForClient.h"

namespace
{
NK2AI::EscapePathCandidate makeAcceptedCandidate()
{
	NK2AI::EscapePathCandidate candidate;
	candidate.currentTileThreatensHero = true;
	candidate.sameDay = true;
	candidate.singleHeroPath = true;
	candidate.destinationSafe = true;
	candidate.destinationIsSafer = true;
	candidate.threatReduction = 100.0f;
	candidate.movementCost = 0.5f;
	return candidate;
}

bool containsHeroChain(
	const NK2AI::Goals::TGoalVec & goals,
	const CGHeroInstance * hero)
{
	return std::ranges::any_of(goals, [hero](const NK2AI::Goals::TSubgoal & goal)
	{
		const auto * chain = dynamic_cast<const NK2AI::Goals::ExecuteHeroChain *>(goal.get());
		return chain && chain->getHero() == hero;
	});
}

const PlayerColor PLAYER(0);
const PlayerColor ENEMY(1);

class LockedDefenderEscapeTest : public NullkillerTest
{
protected:
	void startGame(uint16_t defenderCount, uint16_t attackerCount)
	{
		TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
		builder
			.size(36, false)
			.playerActive(PLAYER)
			.playerActive(ENEMY)
			.randomTown({9, 5, 0}, PLAYER)
			.hero({5, 5, 0}, HeroTypeID(0), PLAYER)
			.heroGarrison({{CreatureID(0), defenderCount}})
			.hero({20, 20, 0}, HeroTypeID(1), ENEMY)
			.heroGarrison({{CreatureID(13), attackerCount}});

		startWithMap(std::move(builder));
	}

	void placeHero(const CGHeroInstance & hero, const int3 & position)
	{
		ChangeObjPos moveHero;
		moveHero.objid = hero.id;
		moveHero.nPos = hero.convertFromVisitablePos(position);
		moveHero.initiator = hero.getOwner();
		gameState->apply(moveHero);
	}

	std::unique_ptr<NK2AI::AIGateway> prepareAI()
	{
		auto * town = findFirst<CGTownInstance>();
		auto * defender = findHeroByOwner(PLAYER);
		auto * attacker = findHeroByOwner(ENEMY);
		EXPECT_NE(town, nullptr);
		EXPECT_NE(defender, nullptr);
		EXPECT_NE(attacker, nullptr);
		if(!town || !defender || !attacker)
			return nullptr;

		placeHero(*defender, town->visitablePos());
		placeHero(*attacker, town->visitablePos() + int3(2, 0, 0));
		town->setVisitingHero(defender);
		defender->setMovementPoints(2000);
		attacker->setMovementPoints(2000);
		revealMap(PLAYER);

		auto gateway = makeGateway(PLAYER);
		NK2AI::NullkillerTestAccess::prepareState(*gateway->nullkiller);
		gateway->nullkiller->lockHero(defender, NK2AI::HeroLockedReason::DEFENCE);
		return gateway;
	}
};
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, acceptsSafeSameDayThreatReduction)
{
	const auto evaluation = NK2AI::evaluateEscapePathCandidate(makeAcceptedCandidate());

	EXPECT_TRUE(evaluation.accepted);
	EXPECT_FLOAT_EQ(evaluation.score, 200.0f);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, rejectsWhenCurrentTileIsNotThreatened)
{
	auto candidate = makeAcceptedCandidate();
	candidate.currentTileThreatensHero = false;

	EXPECT_FALSE(NK2AI::evaluateEscapePathCandidate(candidate).accepted);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, rejectsUnsafeDestination)
{
	auto candidate = makeAcceptedCandidate();
	candidate.destinationSafe = false;

	EXPECT_FALSE(NK2AI::evaluateEscapePathCandidate(candidate).accepted);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, rejectsDestinationWithNoThreatReduction)
{
	auto candidate = makeAcceptedCandidate();
	candidate.destinationIsSafer = false;

	EXPECT_FALSE(NK2AI::evaluateEscapePathCandidate(candidate).accepted);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, acceptsDimensionDoorPathWhenItIsOtherwiseValid)
{
	auto candidate = makeAcceptedCandidate();
	candidate.usesDimensionDoor = true;

	EXPECT_TRUE(NK2AI::evaluateEscapePathCandidate(candidate).accepted);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, doesNotUseDimensionDoorForNextDayEscape)
{
	auto candidate = makeAcceptedCandidate();
	candidate.usesDimensionDoor = true;
	candidate.sameDay = false;

	EXPECT_FALSE(NK2AI::evaluateEscapePathCandidate(candidate).accepted);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, doesNotUseDimensionDoorWhenLandingIsUnsafe)
{
	auto candidate = makeAcceptedCandidate();
	candidate.usesDimensionDoor = true;
	candidate.destinationSafe = false;

	EXPECT_FALSE(NK2AI::evaluateEscapePathCandidate(candidate).accepted);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, doesNotUseDimensionDoorWhenItDoesNotReduceThreat)
{
	auto candidate = makeAcceptedCandidate();
	candidate.usesDimensionDoor = true;
	candidate.destinationIsSafer = false;

	EXPECT_FALSE(NK2AI::evaluateEscapePathCandidate(candidate).accepted);
}

TEST(Nullkiller2_Behaviors_EscapeBehavior, escapePriorityRunsBeforeExploration)
{
	EXPECT_LT(
		NK2AI::PriorityEvaluator::PriorityTier::KILL,
		NK2AI::PriorityEvaluator::PriorityTier::ESCAPE);
	EXPECT_LT(
		NK2AI::PriorityEvaluator::PriorityTier::ESCAPE,
		NK2AI::PriorityEvaluator::PriorityTier::EXPLORE_AND_GATHER);
}

TEST_F(LockedDefenderEscapeTest, releasesIneffectiveDefenderToEscapeImmediateThreat)
{
	startGame(1, 20);
	const auto gateway = prepareAI();
	ASSERT_NE(gateway, nullptr);
	const auto * defender = findHeroByOwner(PLAYER);
	ASSERT_NE(defender, nullptr);

	const auto goals = NK2AI::Goals::EscapeBehavior().decompose(gateway->nullkiller.get());
	EXPECT_TRUE(containsHeroChain(goals, defender));
}

TEST_F(LockedDefenderEscapeTest, keepsDefenderWhenTownIsStable)
{
	startGame(1000, 1);
	const auto gateway = prepareAI();
	ASSERT_NE(gateway, nullptr);
	const auto * defender = findHeroByOwner(PLAYER);
	ASSERT_NE(defender, nullptr);

	const auto goals = NK2AI::Goals::EscapeBehavior().decompose(gateway->nullkiller.get());
	EXPECT_FALSE(containsHeroChain(goals, defender));
}
