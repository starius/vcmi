/*
 * BattleSimulationGameInterfaceAdapter.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationGameInterfaceAdapter.h"

#include "BattleSimulationActionCapture.h"
#include "BattleSimulationBattleEventDispatcher.h"

#include "../CGameHandler.h"

#include "../../lib/callback/CBattleCallback.h"
#include "../../lib/callback/CBattleGameInterface.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"

#include <set>

namespace BattleSimulation
{
namespace
{
class BattleSimulationGameInterfaceActionProvider final : public IBattleSimulationActionProvider
{
public:
	BattleSimulationGameInterfaceActionProvider(
		CGameHandler & gameHandler,
		const BattleSimulationRequest & request,
		const BattleID & battleID,
		BattleSimulationGameInterfaceActionProviderFactory::BattleInterfaceFactory battleInterfaceFactory,
		AutocombatPreferences autocombatPreferences)
		: captureClient(std::make_shared<BattleSimulationActionCaptureClient>())
		, dispatcher(gameHandler.gameState())
	{
		const auto environment = std::shared_ptr<Environment>(&gameHandler, [](Environment *){});
		std::set<PlayerColor> installedPlayers;

		for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		{
			const auto * army = request.setup.armies[side];
			if(!army)
				continue;

			const auto player = army->getOwner();
			if(!installedPlayers.insert(player).second)
				continue;

			auto battleInterface = battleInterfaceFactory(player);
			if(!battleInterface)
				continue;

			auto callback = std::make_shared<CBattleCallback>(player, captureClient.get());
			dispatcher.registerBattleCallback(player, callback);
			dispatcher.registerBattleInterface(player, battleInterface);
			battleInterface->initBattleInterface(environment, callback, autocombatPreferences);
		}
	}

	std::optional<BattleAction> makeAction(CGameHandler &, const CBattleInfoCallback &) override
	{
		return captureClient->takeAction();
	}

	IBattleSimulationPackListener * packListener() override
	{
		return &dispatcher;
	}

private:
	std::shared_ptr<BattleSimulationActionCaptureClient> captureClient;
	BattleSimulationBattleEventDispatcher dispatcher;
};
}

BattleSimulationGameInterfaceActionProviderFactory::BattleSimulationGameInterfaceActionProviderFactory(
	BattleInterfaceFactory battleInterfaceFactory,
	AutocombatPreferences autocombatPreferences)
	: battleInterfaceFactory(std::move(battleInterfaceFactory))
	, autocombatPreferences(autocombatPreferences)
{
}

std::unique_ptr<IBattleSimulationActionProvider> BattleSimulationGameInterfaceActionProviderFactory::create(
	CGameHandler & gameHandler,
	const BattleSimulationRequest & request,
	const BattleID & battleID)
{
	if(!battleInterfaceFactory)
		return nullptr;

	return std::make_unique<BattleSimulationGameInterfaceActionProvider>(
		gameHandler,
		request,
		battleID,
		battleInterfaceFactory,
		autocombatPreferences);
}
}
