/*
 * BattleSimulationSetup.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationSetup.h"

#include "BattleSimulationFingerprint.h"

#include "../../lib/battle/BattleLayout.h"
#include "../../lib/callback/IGameInfoCallback.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"

namespace BattleSimulation
{
namespace
{
bool isEnemy(const IGameInfoCallback & gameInfo, PlayerColor left, PlayerColor right)
{
	return left.isValidPlayer()
		&& right.isValidPlayer()
		&& gameInfo.getPlayerRelations(left, right) == PlayerRelations::ENEMIES;
}

bool isPotentialEnemyArmy(const IGameInfoCallback & gameInfo, const CGHeroInstance * attacker, const CArmedInstance * defender)
{
	if(!attacker || !defender || defender->stacksCount() <= 0)
		return false;

	if(!defender->tempOwner.isValidPlayer())
		return true;

	return isEnemy(gameInfo, defender->tempOwner, attacker->tempOwner);
}

std::optional<BattleStartInfo> makeTownBattleStartInfo(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CGTownInstance * town)
{
	if(!attacker || !town || !isEnemy(gameInfo, town->getOwner(), attacker->getOwner()))
		return std::nullopt;

	if(!town->armedGarrison() && !town->getVisitingHero())
		return std::nullopt;

	const CGHeroInstance * defendingHero = town->getVisitingHero() ? town->getVisitingHero() : town->getGarrisonHero();
	const auto * defendingArmy = defendingHero ? static_cast<const CArmedInstance *>(defendingHero) : static_cast<const CArmedInstance *>(town);
	const bool isBattleOutside = town->isBattleOutsideTown(defendingHero);
	auto preMerge = std::optional<BattleStartTownPreMergeSnapshot>{};

	if(!isBattleOutside && defendingHero == town->getVisitingHero() && town->stacksCount() > 0)
		preMerge = makeBattleStartTownPreMergeSnapshot(town, defendingHero);

	return BattleStartInfo{
		BattleSideArray<const CArmedInstance *>{attacker, defendingArmy},
		BattleSideArray<const CGHeroInstance *>{attacker, defendingHero},
		town->getSightCenter(),
		BattleLayout::createDefaultLayout(gameInfo, attacker, defendingArmy),
		isBattleOutside ? nullptr : town,
		preMerge
	};
}

std::optional<BattleStartInfo> makeFieldBattleStartInfo(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CArmedInstance * defender)
{
	if(!isPotentialEnemyArmy(gameInfo, attacker, defender))
		return std::nullopt;

	return BattleStartInfo{
		BattleSideArray<const CArmedInstance *>{attacker, defender},
		BattleSideArray<const CGHeroInstance *>{
			attacker,
			defender->ID == Obj::HERO ? dynamic_cast<const CGHeroInstance *>(defender) : nullptr
		},
		defender->visitablePos(),
		BattleLayout::createDefaultLayout(gameInfo, attacker, defender),
		nullptr,
		std::nullopt
	};
}

BattleSimulationBattleType battleTypeForSetup(const BattleStartInfo & setup, const CGObjectInstance * target)
{
	if(setup.town)
		return BattleSimulationBattleType::TOWN;

	if(target && target->ID == Obj::TOWN)
		return BattleSimulationBattleType::TOWN_OUTSIDE;

	const auto * defenderHero = setup.heroes[BattleSide::DEFENDER];
	if(defenderHero && defenderHero->getVisitedTown())
		return BattleSimulationBattleType::TOWN_OUTSIDE;

	return BattleSimulationBattleType::FIELD;
}
}

std::optional<BattleStartInfo> makeBattleStartInfoForVisit(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CGObjectInstance * target)
{
	if(!attacker || !target || attacker == target)
		return std::nullopt;

	if(const auto * defenderHero = dynamic_cast<const CGHeroInstance *>(target))
	{
		if(!isEnemy(gameInfo, defenderHero->tempOwner, attacker->tempOwner))
			return std::nullopt;

		if(const auto * visitedTown = defenderHero->getVisitedTown())
			return makeTownBattleStartInfo(gameInfo, attacker, visitedTown);

		return makeFieldBattleStartInfo(gameInfo, attacker, defenderHero);
	}

	if(const auto * town = dynamic_cast<const CGTownInstance *>(target))
		return makeTownBattleStartInfo(gameInfo, attacker, town);

	if(const auto * defender = dynamic_cast<const CArmedInstance *>(target))
		return makeFieldBattleStartInfo(gameInfo, attacker, defender);

	return std::nullopt;
}

BattleSimulationSeedContext makeSeedContextForVisit(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CGObjectInstance * target,
	int64_t gameSeed,
	BattleSimulationBattleType battleType,
	int32_t sampleIndex,
	int32_t evaluatorVersion)
{
	BattleSimulationSeedContext seed;
	seed.gameSeed = gameSeed;
	seed.player = attacker ? attacker->getOwner() : PlayerColor::CANNOT_DETERMINE;
	seed.heroId = attacker ? attacker->id : ObjectInstanceID::NONE;
	seed.targetObjectId = target ? target->id : ObjectInstanceID::NONE;
	seed.battleType = static_cast<int32_t>(battleType);
	seed.turn = gameInfo.getCalendar().getCurrentDay();
	seed.sampleIndex = sampleIndex;
	seed.evaluatorVersion = evaluatorVersion;
	return seed;
}

std::optional<BattleSimulationRequest> makeBattleSimulationRequestForVisit(
	const IGameInfoCallback & gameInfo,
	const CGHeroInstance * attacker,
	const CGObjectInstance * target,
	int64_t gameSeed,
	int32_t sampleCount,
	const BattleSimulationDecisionThresholds & thresholds,
	int32_t sampleIndex,
	int32_t evaluatorVersion)
{
	auto setup = makeBattleStartInfoForVisit(gameInfo, attacker, target);
	if(!setup)
		return std::nullopt;

	BattleSimulationRequest request;
	request.setup = *setup;
	request.seed = makeSeedContextForVisit(
		gameInfo,
		attacker,
		target,
		gameSeed,
		battleTypeForSetup(*setup, target),
		sampleIndex,
		evaluatorVersion);
	request.thresholds = thresholds;
	request.stateFingerprint = fingerprintBattleStartInfo(*setup);
	request.sampleCount = sampleCount;
	return request;
}
}
