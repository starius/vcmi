/*
 * BattleSimulationBatch.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleSimulationBatch.h"

#include "../CGameHandler.h"

#include "../../lib/CConfigHandler.h"
#include "../../lib/battle/CBattleInfoCallback.h"
#include "../../lib/battle/IBattleState.h"
#include "../../lib/constants/Enumerations.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/army/CCreatureSet.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"

#include <fstream>
#include <sstream>

namespace BattleSimulationBatch
{
namespace
{
struct Config
{
	bool enabled = false;
	std::string outputPath;
	int64_t maxBattles = 0;
	int shardIndex = 0;
	int shardCount = 1;
	int64_t globalSeed = 0;
	int64_t shardSeed = 0;
};

struct State
{
	bool initialized = false;
	Config config;
	std::ofstream output;
	int64_t rowsWritten = 0;
};

State state;

std::string quote(const std::string & value)
{
	std::ostringstream out;
	out << '"';
	for(const char ch : value)
	{
		switch(ch)
		{
			case '\\':
				out << "\\\\";
				break;
			case '"':
				out << "\\\"";
				break;
			case '\n':
				out << "\\n";
				break;
			case '\r':
				out << "\\r";
				break;
			case '\t':
				out << "\\t";
				break;
			default:
				out << ch;
				break;
		}
	}
	out << '"';
	return out.str();
}

std::string battleSideToString(BattleSide side)
{
	switch(side)
	{
		case BattleSide::ATTACKER:
			return "attacker";
		case BattleSide::DEFENDER:
			return "defender";
		case BattleSide::NONE:
			return "none";
		default:
			return "draw";
	}
}

std::string battleResultToString(EBattleResult result)
{
	switch(result)
	{
		case EBattleResult::NORMAL:
			return "normal";
		case EBattleResult::ESCAPE:
			return "escape";
		case EBattleResult::SURRENDER:
			return "surrender";
		default:
			return "unknown";
	}
}

void appendArmy(std::ostream & out, const CCreatureSet * army)
{
	out << '[';
	bool first = true;
	if(army)
	{
		for(const auto & [slot, stack] : army->Slots())
		{
			if(!first)
				out << ',';
			first = false;

			out << "{";
			out << "\"slot\":" << slot.getNum();
			out << ",\"creature\":" << stack->getCreatureID().getNum();
			out << ",\"count\":" << stack->getCount();
			out << ",\"power\":" << stack->getPower();
			out << ",\"experience\":" << stack->getTotalExperience();
			out << "}";
		}
	}
	out << ']';
}

void appendCasualties(std::ostream & out, const std::map<CreatureID, si32> & casualties)
{
	out << '[';
	bool first = true;
	for(const auto & [creature, killed] : casualties)
	{
		if(!first)
			out << ',';
		first = false;

		out << "{\"creature\":" << creature.getNum() << ",\"killed\":" << killed << '}';
	}
	out << ']';
}

void appendHero(std::ostream & out, const CGHeroInstance * hero)
{
	if(!hero)
	{
		out << "null";
		return;
	}

	out << "{";
	out << "\"objectId\":" << hero->id.getNum();
	out << ",\"type\":" << hero->getHeroTypeID().getNum();
	out << ",\"name\":" << quote(hero->getNameTranslated());
	out << ",\"level\":" << hero->level;
	out << ",\"mana\":" << hero->mana;
	out << ",\"primary\":[";
	for(size_t i = 0; i < GameConstants::PRIMARY_SKILLS; ++i)
	{
		if(i)
			out << ',';
		out << hero->getPrimSkillLevel(PrimarySkill(static_cast<int32_t>(i)));
	}
	out << "]}";
}

Config readConfig()
{
	Config config;
	const auto batchSettings = settings["server"]["battleSimulation"];
	config.enabled = batchSettings["enabled"].Bool();
	if(!config.enabled)
		return config;

	config.outputPath = batchSettings["output"].String();
	config.maxBattles = batchSettings["maxBattles"].Integer();
	config.shardIndex = batchSettings["shardIndex"].Integer();
	config.shardCount = batchSettings["shardCount"].Integer();
	config.globalSeed = batchSettings["globalSeed"].Integer();
	config.shardSeed = batchSettings["shardSeed"].Integer();

	if(config.shardCount <= 0)
		config.shardCount = 1;
	if(config.shardIndex < 0)
		config.shardIndex = 0;

	return config;
}

void initialize()
{
	if(state.initialized)
		return;

	state.initialized = true;
	state.config = readConfig();

	if(!state.config.enabled)
		return;

	if(state.config.maxBattles <= 0)
		throw std::runtime_error("Battle simulation max battle count must be positive");

	if(state.config.outputPath.empty())
		throw std::runtime_error("Battle simulation output path must not be empty");

	state.output.open(state.config.outputPath, std::ios::out | std::ios::trunc);
	if(!state.output)
		throw std::runtime_error("Unable to open battle simulation output: " + state.config.outputPath);
}

void appendResultRow(const CBattleInfoCallback & battle, const BattleResult & result)
{
	const auto * info = battle.getBattle();
	const int64_t rowIndex = state.rowsWritten;

	state.output << "{";
	state.output << "\"schema\":1";
	state.output << ",\"row\":" << rowIndex;
	state.output << ",\"shardIndex\":" << state.config.shardIndex;
	state.output << ",\"shardCount\":" << state.config.shardCount;
	state.output << ",\"globalSeed\":" << state.config.globalSeed;
	state.output << ",\"shardSeed\":" << state.config.shardSeed;
	state.output << ",\"battleId\":" << info->getBattleID().getNum();
	state.output << ",\"round\":" << info->getRound();
	state.output << ",\"result\":" << quote(battleResultToString(result.result));
	state.output << ",\"winner\":" << quote(battleSideToString(result.winner));
	state.output << ",\"terrain\":" << info->getTerrainType().getNum();
	state.output << ",\"battlefield\":" << info->getBattlefieldType().getNum();
	state.output << ",\"attackerPlayer\":" << info->getSidePlayer(BattleSide::ATTACKER).getNum();
	state.output << ",\"defenderPlayer\":" << info->getSidePlayer(BattleSide::DEFENDER).getNum();
	state.output << ",\"combatEnemyAI\":" << quote(settings["ai"]["combatEnemyAI"].String());
	state.output << ",\"combatNeutralAI\":" << quote(settings["ai"]["combatNeutralAI"].String());
	state.output << ",\"attackerHero\":";
	appendHero(state.output, info->getSideHero(BattleSide::ATTACKER));
	state.output << ",\"defenderHero\":";
	appendHero(state.output, info->getSideHero(BattleSide::DEFENDER));
	state.output << ",\"attackerArmyStrength\":" << info->getSideArmy(BattleSide::ATTACKER)->getArmyStrength();
	state.output << ",\"defenderArmyStrength\":" << info->getSideArmy(BattleSide::DEFENDER)->getArmyStrength();
	state.output << ",\"attackerArmy\":";
	appendArmy(state.output, info->getSideArmy(BattleSide::ATTACKER));
	state.output << ",\"defenderArmy\":";
	appendArmy(state.output, info->getSideArmy(BattleSide::DEFENDER));
	state.output << ",\"attackerCasualties\":";
	appendCasualties(state.output, result.casualties[BattleSide::ATTACKER]);
	state.output << ",\"defenderCasualties\":";
	appendCasualties(state.output, result.casualties[BattleSide::DEFENDER]);
	state.output << "}\n";

	if(!state.output)
		throw std::runtime_error("Failed to write battle simulation result");

	++state.rowsWritten;
	state.output.flush();

	if(state.rowsWritten % 1000 == 0)
	{
		logGlobal->info("Battle simulation wrote %lld/%lld rows to %s",
			static_cast<long long>(state.rowsWritten),
			static_cast<long long>(state.config.maxBattles),
			state.config.outputPath);
	}
}
}

bool isEnabled()
{
	initialize();
	return state.config.enabled;
}

bool recordResultAndShouldReplay(CGameHandler &, const CBattleInfoCallback & battle, const BattleResult & result)
{
	initialize();
	if(!state.config.enabled)
		return false;

	appendResultRow(battle, result);
	return state.rowsWritten < state.config.maxBattles;
}
}
