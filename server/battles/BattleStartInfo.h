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
#include "../../lib/constants/EntityIdentifiers.h"
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

struct BattleStartStackStateSnapshot
{
	uint32_t unitId = 0;
	BattleSide side = BattleSide::NONE;
	SlotID slot;
	CreatureID creature;
	TQuantity count = 0;
	int32_t baseAmount = 0;
	int32_t position = BattleHex::INVALID;
	int32_t initialPosition = BattleHex::INVALID;
	int64_t availableHealth = 0;
	int64_t totalHealth = 0;
	int32_t maxHealth = 0;
	int32_t firstHPLeft = 0;
	int32_t meleeAttack = 0;
	int32_t rangedAttack = 0;
	int32_t meleeDefense = 0;
	int32_t rangedDefense = 0;
	int32_t meleeDamageMin = 0;
	int32_t meleeDamageMax = 0;
	int32_t rangedDamageMin = 0;
	int32_t rangedDamageMax = 0;
	int32_t speed = 0;
	int32_t movementRange = 0;
	int32_t morale = 0;
	int32_t luck = 0;
	int32_t shotsAvailable = 0;
	int32_t shotsTotal = 0;
	int32_t castsAvailable = 0;
	int32_t castsTotal = 0;
	int32_t retaliationsAvailable = 0;
	int32_t retaliationsTotal = 0;
	bool alive = false;
	bool validTarget = false;
	bool doubleWide = false;
	bool shooter = false;
	bool canShoot = false;
	bool caster = false;
	bool canCast = false;
	bool turret = false;
	bool catapult = false;
	bool ballista = false;
	bool firstAidTent = false;
	bool ammoCart = false;
	bool summoned = false;
};

struct BattleStartObstacleSnapshot
{
	int32_t uniqueId = -1;
	int32_t id = -1;
	int32_t type = 0;
	int32_t position = BattleHex::INVALID;
	int32_t trigger = -1;
	int32_t turnsRemaining = -1;
	int32_t spellLevel = -1;
	int32_t casterSide = static_cast<int32_t>(BattleSide::NONE);
	bool blocksTiles = false;
	bool stopsMovement = false;
	bool triggersEffects = false;
	bool hidden = false;
	bool passable = false;
	bool trap = false;
	bool removeOnTrigger = false;
	bool revealed = false;
	std::vector<int32_t> affectedTiles;
};

struct BattleStartStateSnapshot
{
	std::vector<BattleStartStackStateSnapshot> stacks;
	std::vector<BattleStartObstacleSnapshot> obstacles;
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
BattleStartStateSnapshot makeBattleStartStateSnapshot(const IBattleInfo & battle);
