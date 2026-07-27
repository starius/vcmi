/*
 * HeroManagerTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "AI/Nullkiller2/Analyzers/HeroManager.h"

TEST(Nullkiller2_Analyzers_HeroManager, strongestArmyIsAlwaysMeaningful)
{
	EXPECT_TRUE(NK2AI::isMeaningfulArmyCarrierStrength(1, 1, true));
}

TEST(Nullkiller2_Analyzers_HeroManager, reinforcementMustExceedRelativeThreshold)
{
	constexpr uint64_t strongestArmy = 10000;

	EXPECT_FALSE(NK2AI::isMeaningfulArmyCarrierStrength(1000, strongestArmy, false));
	EXPECT_TRUE(NK2AI::isMeaningfulArmyCarrierStrength(1001, strongestArmy, false));
}

TEST(Nullkiller2_Analyzers_HeroManager, reinforcementMustExceedAbsoluteThreshold)
{
	constexpr uint64_t strongestArmy = 4000;

	EXPECT_FALSE(NK2AI::isMeaningfulArmyCarrierStrength(500, strongestArmy, false));
	EXPECT_TRUE(NK2AI::isMeaningfulArmyCarrierStrength(501, strongestArmy, false));
}
