/*
 * BattleSimulationRunner.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "BattleSimulationRequest.h"

#include "../../lib/battle/BattleAction.h"

#include <optional>
#include <memory>

class CGameHandler;
class CGameState;
class CBattleInfoCallback;
class BattleID;

namespace BattleSimulation
{
class IBattleSimulationPackListener;

class IBattleSimulationRunner
{
public:
	virtual ~IBattleSimulationRunner() = default;

	virtual std::optional<BattleSimulationSummary> run(const BattleSimulationRequest & request) = 0;
};

class IBattleSimulationActionProvider
{
public:
	virtual ~IBattleSimulationActionProvider() = default;

	virtual std::optional<BattleAction> makeAction(CGameHandler & gameHandler, const CBattleInfoCallback & battle) = 0;
	virtual IBattleSimulationPackListener * packListener() { return nullptr; }
};

class IBattleSimulationActionProviderFactory
{
public:
	virtual ~IBattleSimulationActionProviderFactory() = default;

	virtual std::unique_ptr<IBattleSimulationActionProvider> create(
		CGameHandler & gameHandler,
		const BattleSimulationRequest & request,
		const BattleID & battleID) = 0;
};

struct BattleSimulationRunnerOptions
{
	int32_t maxActionsPerSample = 10000;
};

class IsolatedBattleSimulationRunner final : public IBattleSimulationRunner
{
public:
	IsolatedBattleSimulationRunner(
		const CGameState & sourceState,
		std::shared_ptr<IBattleSimulationActionProviderFactory> actionProviderFactory,
		BattleSimulationRunnerOptions options = {});

	std::optional<BattleSimulationSummary> run(const BattleSimulationRequest & request) override;

private:
	const CGameState & sourceState;
	std::shared_ptr<IBattleSimulationActionProviderFactory> actionProviderFactory;
	BattleSimulationRunnerOptions options;
};
}
