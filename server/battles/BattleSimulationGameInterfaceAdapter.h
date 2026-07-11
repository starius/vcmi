/*
 * BattleSimulationGameInterfaceAdapter.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationRunner.h"

#include "../../lib/battle/AutocombatPreferences.h"

#include <functional>

class CBattleGameInterface;

namespace BattleSimulation
{
class BattleSimulationGameInterfaceActionProviderFactory final : public IBattleSimulationActionProviderFactory
{
public:
	using BattleInterfaceFactory = std::function<std::shared_ptr<CBattleGameInterface>(PlayerColor)>;

	explicit BattleSimulationGameInterfaceActionProviderFactory(
		BattleInterfaceFactory battleInterfaceFactory,
		AutocombatPreferences autocombatPreferences = {});

	std::unique_ptr<IBattleSimulationActionProvider> create(
		CGameHandler & gameHandler,
		const BattleSimulationRequest & request,
		const BattleID & battleID) override;

private:
	BattleInterfaceFactory battleInterfaceFactory;
	AutocombatPreferences autocombatPreferences;
};
}
