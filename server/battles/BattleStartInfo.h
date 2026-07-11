/*
 * BattleStartInfo.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/battle/BattleLayout.h"
#include "../../lib/int3.h"

class CArmedInstance;
class CGHeroInstance;
class CGTownInstance;
class IBattleInfo;

struct BattleStartInfo
{
	BattleSideArray<const CArmedInstance *> armies{nullptr, nullptr};
	BattleSideArray<const CGHeroInstance *> heroes{nullptr, nullptr};
	int3 tile;
	BattleLayout layout;
	const CGTownInstance * town = nullptr;

	static BattleStartInfo fromBattle(const IBattleInfo & battle);
};
