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
#include "../../lib/GameConstants.h"
#include "../../lib/int3.h"

#include <cstdint>
#include <optional>
#include <vector>

class CArmedInstance;
class CGHeroInstance;
class CGTownInstance;
class IBattleInfo;

struct BattleStartStackSnapshot
{
	SlotID slot;
	CreatureID creature;
	TQuantity count = 0;
	uint64_t power = 0;
	int64_t experience = 0;
};

struct BattleStartArmySnapshot
{
	ObjectInstanceID objectId;
	uint64_t armyStrength = 0;
	std::vector<BattleStartStackSnapshot> stacks;
};

struct BattleStartTownPreMergeSnapshot
{
	ObjectInstanceID townId;
	ObjectInstanceID defendingHeroId;
	BattleStartArmySnapshot townArmy;
	BattleStartArmySnapshot defendingHeroArmy;
};

struct BattleStartInfo
{
	BattleSideArray<const CArmedInstance *> armies{nullptr, nullptr};
	BattleSideArray<const CGHeroInstance *> heroes{nullptr, nullptr};
	int3 tile;
	BattleLayout layout;
	const CGTownInstance * town = nullptr;
	std::optional<BattleStartTownPreMergeSnapshot> townPreMerge;

	static BattleStartInfo fromBattle(const IBattleInfo & battle);
};

BattleStartArmySnapshot makeBattleStartArmySnapshot(const CArmedInstance * army);
std::optional<BattleStartTownPreMergeSnapshot> makeBattleStartTownPreMergeSnapshot(
	const CGTownInstance * town,
	const CGHeroInstance * defendingHero);
