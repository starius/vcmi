/*
 * AIFactory.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "AIFactory.h"

#include "CGlobalAI.h"

#ifdef ENABLE_NULLKILLER2_AI
#  include "../../AI/Nullkiller2/AIGateway.h"
#endif
#ifdef ENABLE_BATTLE_AI
#  include "../../AI/BattleAI/BattleAI.h"
#endif
#ifdef ENABLE_STUPID_AI
#  include "../../AI/StupidAI/StupidAI.h"
#endif
#ifdef ENABLE_MMAI
#  include "../../AI/MMAI/MMAI.h"
#endif
#include "../../AI/EmptyAI/CEmptyAI.h"

#ifdef ENABLE_NULLKILLER2_AI
namespace
{
constexpr int NULLKILLER2_V3_SIMULATION_SAMPLES = 15;
constexpr float NULLKILLER2_V3_SIMULATION_PLANNING_SAFE_ATTACK_RATIO = 1.0f;

std::shared_ptr<CGlobalAI> createNullkiller2Gateway(
	const std::string & name,
	NK2AI::BattlePredictionSettingsOverride settingsOverride = {})
{
	auto ret = std::make_shared<NK2AI::AIGateway>(std::move(settingsOverride));
	ret->dllName = name;
	return ret;
}
}
#endif

std::shared_ptr<CGlobalAI> AIFactory::createAdventureAI(const std::string & name)
{
	logGlobal->info("Creating adventure AI %s", name);

	if(name == "Nullkiller2")
	{
#ifdef ENABLE_NULLKILLER2_AI
		return createNullkiller2Gateway(name);
#else
		throw std::runtime_error("Nullkiller2 is not available in this build!");
#endif
	}

	if(name == "Nullkiller2Ratio")
	{
#ifdef ENABLE_NULLKILLER2_AI
		NK2AI::BattlePredictionSettingsOverride settingsOverride;
		settingsOverride.model = NK2AI::BattlePredictionModel::RATIO;
		return createNullkiller2Gateway(name, settingsOverride);
#else
		throw std::runtime_error("Nullkiller2 is not available in this build!");
#endif
	}

	if(name == "Nullkiller2V2")
	{
#ifdef ENABLE_NULLKILLER2_AI
		NK2AI::BattlePredictionSettingsOverride settingsOverride;
		settingsOverride.model = NK2AI::BattlePredictionModel::V2;
		return createNullkiller2Gateway(name, settingsOverride);
#else
		throw std::runtime_error("Nullkiller2 is not available in this build!");
#endif
	}

	if(name == "Nullkiller2V3")
	{
#ifdef ENABLE_NULLKILLER2_AI
		NK2AI::BattlePredictionSettingsOverride settingsOverride;
		settingsOverride.model = NK2AI::BattlePredictionModel::V3;
		return createNullkiller2Gateway(name, settingsOverride);
#else
		throw std::runtime_error("Nullkiller2 is not available in this build!");
#endif
	}

	if(name == "Nullkiller2V3Simulation")
	{
#ifdef ENABLE_NULLKILLER2_AI
		NK2AI::BattlePredictionSettingsOverride settingsOverride;
		settingsOverride.model = NK2AI::BattlePredictionModel::V3;
		settingsOverride.simulationSamples = NULLKILLER2_V3_SIMULATION_SAMPLES;
		settingsOverride.simulationPlanningSafeAttackRatio = NULLKILLER2_V3_SIMULATION_PLANNING_SAFE_ATTACK_RATIO;
		return createNullkiller2Gateway(name, settingsOverride);
#else
		throw std::runtime_error("Nullkiller2 is not available in this build!");
#endif
	}

	auto ret = std::make_shared<CEmptyAI>();
	ret->dllName = name;
	return ret;
}

std::shared_ptr<CBattleGameInterface> AIFactory::createBattleAI(const std::string & name)
{
	logGlobal->info("Creating battle AI %s", name);

	if(name == "BattleAI")
#ifdef ENABLE_BATTLE_AI
		return std::make_shared<CBattleAI>();
#else
		throw std::runtime_error("BattleAI is not available in this build!");
#endif

	if(name == "StupidAI")
#ifdef ENABLE_STUPID_AI
		return std::make_shared<CStupidAI>();
#else
		throw std::runtime_error("StupidAI is not available in this build!");
#endif

	if(name == "MMAI")
#ifdef ENABLE_MMAI
		return std::make_shared<MMAI::BAI::Router>();
#else
		throw std::runtime_error("MMAI is not available in this build!");
#endif

	return std::make_shared<CEmptyAI>();
}

bool AIFactory::isAvailableAdventureAI(const std::string & name)
{
	if(name == "EmptyAI")
		return true;
#ifdef ENABLE_NULLKILLER2_AI
	if(name == "Nullkiller2")
		return true;
	if(name == "Nullkiller2Ratio")
		return true;
	if(name == "Nullkiller2V2")
		return true;
	if(name == "Nullkiller2V3")
		return true;
	if(name == "Nullkiller2V3Simulation")
		return true;
#endif
	return false;
}
