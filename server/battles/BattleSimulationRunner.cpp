/*
 * BattleSimulationRunner.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationRunner.h"

#include "BattleProcessor.h"
#include "BattleSimulationIsolatedState.h"
#include "BattleSimulationLocalGameServer.h"
#include "BattleSimulationSeed.h"

#include "../CGameHandler.h"

#include "../../lib/battle/BattleInfo.h"
#include "../../lib/callback/GameRandomizer.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"

namespace BattleSimulation
{
namespace
{
int sampleSeedToInt(const BattleSimulationSeedContext & context)
{
	const auto seed = deriveSampleSeed(context);
	const auto mixed = static_cast<uint32_t>(seed ^ (seed >> 32));
	const auto result = static_cast<int>(mixed & 0x7fffffffu);
	return result == 0 ? 1 : result;
}

const BattleInfo * getOnlyCurrentBattle(const CGameState & gameState)
{
	if(gameState.currentBattles.size() != 1)
		return nullptr;

	return gameState.currentBattles.front().get();
}

std::optional<BattleResult> runSingleSample(
	const CGameState & sourceState,
	const BattleSimulationRequest & request,
	IBattleSimulationActionProviderFactory & actionProviderFactory,
	const BattleSimulationRunnerOptions & options,
	int32_t sampleIndex)
{
	auto sampleRequest = request;
	sampleRequest.sampleCount = 1;
	sampleRequest.seed = sampleSeedContext(request.seed, request.seed.sampleIndex + sampleIndex);

	auto clonedState = cloneGameStateForSimulation(sourceState);
	if(!clonedState)
		return std::nullopt;

	BattleSimulationLocalGameServer localServer(*clonedState);
	CGameHandler gameHandler(localServer, clonedState);
	gameHandler.randomizer->setSeed(sampleSeedToInt(sampleRequest.seed));

	auto setup = remapBattleStartInfo(*clonedState, sampleRequest.setup);
	if(!setup)
		return std::nullopt;

	localServer.clearBattleResults();
	gameHandler.battles->startBattle(*setup);

	if(auto result = localServer.lastBattleResult())
		return result;

	const auto * initialBattle = getOnlyCurrentBattle(*clonedState);
	if(!initialBattle)
		return std::nullopt;

	const auto battleID = initialBattle->getBattleID();
	auto actionProvider = actionProviderFactory.create(gameHandler, sampleRequest, battleID);
	if(!actionProvider)
		return std::nullopt;

	for(int32_t actionIndex = 0; actionIndex < options.maxActionsPerSample; ++actionIndex)
	{
		if(auto result = localServer.lastBattleResult())
			return result;

		const auto * battle = clonedState->getBattle(battleID);
		if(!battle)
			return std::nullopt;

		auto action = actionProvider->makeAction(gameHandler, *battle);
		if(!action)
			return std::nullopt;

		if(action->side == BattleSide::NONE)
			return std::nullopt;

		const auto player = battle->sideToPlayer(action->side);
		if(!gameHandler.battles->makePlayerBattleAction(battleID, player, *action))
			return std::nullopt;
	}

	return std::nullopt;
}
}

IsolatedBattleSimulationRunner::IsolatedBattleSimulationRunner(
	const CGameState & sourceState,
	std::shared_ptr<IBattleSimulationActionProviderFactory> actionProviderFactory,
	BattleSimulationRunnerOptions options)
	: sourceState(sourceState)
	, actionProviderFactory(std::move(actionProviderFactory))
	, options(options)
{
}

std::optional<BattleSimulationSummary> IsolatedBattleSimulationRunner::run(const BattleSimulationRequest & request)
{
	if(!isValidRequest(request))
		return std::nullopt;

	if(!actionProviderFactory)
		return std::nullopt;

	if(options.maxActionsPerSample <= 0)
		return std::nullopt;

	BattleSimulationSummary summary;
	for(int32_t sampleIndex = 0; sampleIndex < request.sampleCount; ++sampleIndex)
	{
		const auto result = runSingleSample(sourceState, request, *actionProviderFactory, options, sampleIndex);
		if(!result)
			return summary;

		summary.recordWinner(result->winner);
	}

	return summary;
}
}
