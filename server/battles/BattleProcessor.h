/*
 * BattleProcessor.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/constants/EntityIdentifiers.h"
#include "../../lib/constants/Enumerations.h"
#include "../../lib/battle/BattleSide.h"
#include "BattleSimulationRequest.h"
#include "BattleStartInfo.h"

#include <map>
#include <optional>

class CGHeroInstance;
class CGTownInstance;
class CArmedInstance;
class IBattleInfo;
class BattleAction;
class int3;
class CBattleInfoCallback;
struct BattleResult;
struct BattleLayout;
class BattleID;

class CGameHandler;
class CBattleQuery;
class BattleActionProcessor;
class BattleFlowProcessor;
class BattleResultProcessor;
namespace BattleSimulation
{
	class BattleSimulationEvaluator;
	class IBattleSimulationRunner;
}

/// Main class for battle handling. Contains all public interface for battles that is accessible from outside, e.g. for CGameHandler
class BattleProcessor : boost::noncopyable
{
	friend class BattleActionProcessor;
	friend class BattleFlowProcessor;
	friend class BattleResultProcessor;

	CGameHandler * gameHandler;
	std::unique_ptr<BattleActionProcessor> actionsProcessor;
	std::unique_ptr<BattleFlowProcessor> flowProcessor;
	std::unique_ptr<BattleResultProcessor> resultProcessor;
	std::unique_ptr<BattleSimulation::BattleSimulationEvaluator> simulationEvaluator;
	std::optional<BattleStartTownPreMergeSnapshot> nextTownPreMergeSnapshot;
	std::map<BattleID, BattleStartTownPreMergeSnapshot> townPreMergeSnapshots;
	std::map<BattleID, BattleStartStateSnapshot> battleStartSnapshots;

	void updateGateState(const CBattleInfoCallback & battle);
	void engageIntoBattle(PlayerColor player);
	void updateBattleStartSnapshot(const CBattleInfoCallback & battle);

	bool checkBattleStateChanges(const CBattleInfoCallback & battle);
	BattleID setupBattle(int3 tile, BattleSideArray<const CArmedInstance *> armies, BattleSideArray<const CGHeroInstance *> heroes, const BattleLayout & layout, const CGTownInstance *town);

	bool makeAutomaticBattleAction(const CBattleInfoCallback & battle, const BattleAction & ba);

	void setBattleResult(const CBattleInfoCallback & battle, EBattleResult resultType, BattleSide victoriusSide);

public:
	explicit BattleProcessor(CGameHandler * gameHandler);
	~BattleProcessor();

	/// Starts battle with specified parameters
	void startBattle(const CArmedInstance *army1, const CArmedInstance *army2, int3 tile, const CGHeroInstance *hero1, const CGHeroInstance *hero2, const BattleLayout & layout, const CGTownInstance *town);
	/// Starts battle with specified setup
	void startBattle(const BattleStartInfo & setup);
	/// Starts battle between two armies (which can also be heroes) at position of 2nd object
	void startBattle(const CArmedInstance *army1, const CArmedInstance *army2);
	/// Stores pre-merge town/visiting hero army state for the next matching siege battle
	void setNextBattleTownPreMergeState(const CGTownInstance * town, const CGHeroInstance * defendingHero);
	const BattleStartTownPreMergeSnapshot * getTownPreMergeSnapshot(const BattleID & battleID) const;
	const BattleStartStateSnapshot * getBattleStartSnapshot(const BattleID & battleID) const;
	void discardBattleSnapshots(const BattleID & battleID);
	/// Restart ongoing battle and end previous battle
	void restartBattle(const BattleID & battleID, const CArmedInstance *army1, const CArmedInstance *army2, int3 tile, const CGHeroInstance *hero1, const CGHeroInstance *hero2, const BattleLayout & layout, const CGTownInstance *town);
	/// Restart ongoing battle and end previous battle using specified setup
	void restartBattle(const BattleID & battleID, const BattleStartInfo & setup);
	/// Restart an ongoing battle using its current setup
	void restartBattle(const IBattleInfo & battle);
	/// Evaluate a battle setup through the runtime simulation service boundary
	BattleSimulation::BattleSimulationResponse evaluateBattleSimulation(const BattleSimulation::BattleSimulationRequest & request) const;
	/// Install a runtime simulation runner used when cache-only evaluation can not answer
	void setBattleSimulationRunner(std::shared_ptr<BattleSimulation::IBattleSimulationRunner> runner);
	/// Store a runtime simulation summary for later cached evaluation
	void storeBattleSimulationSummary(const BattleSimulation::BattleSimulationRequest & request, const BattleSimulation::BattleSimulationSummary & summary);

	/// Processing of incoming battle action netpack
	bool makePlayerBattleAction(const BattleID & battleID, PlayerColor player, const BattleAction & ba);

	/// Applies results of a battle once player agrees to them
	void endBattleConfirm(const BattleID & battleID);
	/// Applies results of a battle after potential levelup
	void battleFinalize(const BattleID & battleID, const BattleResult & result);

	template <typename Handler> void serialize(Handler &h)
	{

	}
};
