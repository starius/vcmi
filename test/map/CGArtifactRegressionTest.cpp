/*
 * CGArtifactRegressionTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "callback/EditorCallback.h"
#include "mapping/CMap.h"
#include "mapObjects/MiscObjects.h"

TEST(CGArtifactRegression, RandomArtifactInstanceLookupIsSafe)
{
	EditorCallback cb(nullptr);
	CMap map(&cb);
	cb.setMap(&map);
	CGArtifact randomArtifact(&cb);
	randomArtifact.ID = Obj::RANDOM_ART;

	const CArtifactInstance * instance = nullptr;
	EXPECT_NO_THROW(instance = randomArtifact.getArtifactInstance());
	EXPECT_EQ(instance, nullptr);
}

TEST(CGArtifactRegression, SpellScrollLookupWithoutInstanceIsSafe)
{
	EditorCallback cb(nullptr);
	CMap map(&cb);
	cb.setMap(&map);
	CGArtifact spellScroll(&cb);
	spellScroll.ID = Obj::SPELL_SCROLL;

	const CArtifactInstance * instance = nullptr;
	EXPECT_NO_THROW(instance = spellScroll.getArtifactInstance());
	EXPECT_EQ(instance, nullptr);
}
