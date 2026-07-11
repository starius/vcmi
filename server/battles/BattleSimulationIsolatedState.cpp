/*
 * BattleSimulationIsolatedState.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationIsolatedState.h"

#include "../../lib/gameState/CGameState.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"

namespace BattleSimulation
{
namespace
{
const CArmedInstance * remapArmy(const CGameState & gameState, const CArmedInstance * source)
{
	if(!source)
		return nullptr;

	const auto * object = gameState.getObjInstance(source->id);
	return dynamic_cast<const CArmedInstance *>(object);
}

const CGHeroInstance * remapHero(const CGameState & gameState, const CGHeroInstance * source)
{
	if(!source)
		return nullptr;

	return gameState.getHero(source->id);
}

const CGTownInstance * remapTown(const CGameState & gameState, const CGTownInstance * source)
{
	if(!source)
		return nullptr;

	return gameState.getTown(source->id);
}
}

std::shared_ptr<CGameState> cloneGameStateForSimulation(const CGameState & source)
{
	return source.cloneForSimulation();
}

std::optional<BattleStartInfo> remapBattleStartInfo(
	const CGameState & gameState,
	const BattleStartInfo & source)
{
	BattleStartInfo result = source;

	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		result.armies[side] = remapArmy(gameState, source.armies[side]);
		if(source.armies[side] && !result.armies[side])
			return std::nullopt;

		result.heroes[side] = remapHero(gameState, source.heroes[side]);
		if(source.heroes[side] && !result.heroes[side])
			return std::nullopt;
	}

	result.town = remapTown(gameState, source.town);
	if(source.town && !result.town)
		return std::nullopt;

	return result;
}
}
