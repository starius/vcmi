/*
 * CLuaNullkiller2AI.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "CLuaNullkiller2AI.h"

#include "LuaNullkiller2Runner.h"

#include "../../lib/battle/BattleAction.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/StartInfo.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/IObjectInterface.h"

namespace LuaNullkiller2AI
{
namespace
{

std::optional<int> commandInteger(const LuaCommand & command, const std::string & key)
{
	const auto found = command.integers.find(key);
	if(found == command.integers.end())
		return std::nullopt;

	return found->second;
}

}

std::string CLuaNullkiller2AI::getBattleAIName() const
{
	return "BattleAI";
}

void CLuaNullkiller2AI::initGameInterface(std::shared_ptr<Environment> env, std::shared_ptr<CCallback> callback)
{
	this->env = env;
	cc = callback;
	cbc = callback;
	human = false;
	playerID = *cc->getPlayerID();
}

void CLuaNullkiller2AI::answerQuery(QueryID queryID, int selection) const
{
	cc->selectionMade(selection, queryID);
}

bool CLuaNullkiller2AI::executeCommand(const LuaCommand & command) const
{
	if(command.name == "lockResources")
		return true;

	if(command.name == "recruitHero")
	{
		const auto townID = commandInteger(command, "town");
		const auto heroID = commandInteger(command, "hero");
		if(!townID || !heroID)
			return false;

		const auto * townOrTavern = cc->getObj(ObjectInstanceID(*townID), false);
		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		if(!townOrTavern || !hero)
			return false;

		cc->recruitHero(townOrTavern, hero);
		return true;
	}

	if(command.name == "buildBuilding")
	{
		const auto townID = commandInteger(command, "town");
		const auto buildingID = commandInteger(command, "bid");
		if(!townID || !buildingID)
			return false;

		const auto * town = cc->getTown(ObjectInstanceID(*townID));
		return town && cc->buildBuilding(town, BuildingID(*buildingID));
	}

	if(command.name == "buildBoat")
	{
		const auto shipyardID = commandInteger(command, "shipyard");
		if(!shipyardID)
			return false;

		const auto * object = cc->getObj(ObjectInstanceID(*shipyardID), false);
		const auto * shipyard = dynamic_cast<const IShipyard *>(object);
		if(!shipyard)
			return false;

		cc->buildBoat(shipyard);
		return true;
	}

	if(command.name == "dismissHero")
	{
		const auto heroID = commandInteger(command, "hero");
		if(!heroID)
			return false;

		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		return hero && cc->dismissHero(hero);
	}

	if(command.name == "swapGarrisonHero")
	{
		const auto townID = commandInteger(command, "town");
		if(!townID)
			return false;

		const auto * town = cc->getTown(ObjectInstanceID(*townID));
		if(!town)
			return false;

		cc->swapGarrisonHero(town);
		return true;
	}

	if(command.name == "castSpell")
	{
		const auto heroID = commandInteger(command, "hero");
		const auto spellID = commandInteger(command, "spell");
		if(!heroID || !spellID)
			return false;

		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		if(!hero)
			return false;

		const int3 target(
			commandInteger(command, "x").value_or(-1),
			commandInteger(command, "y").value_or(-1),
			commandInteger(command, "z").value_or(-1));
		cc->castSpell(hero, SpellID(*spellID), target);
		return true;
	}

	if(command.name == "executeHeroChain")
	{
		const auto heroID = commandInteger(command, "hero");
		const auto x = commandInteger(command, "x");
		const auto y = commandInteger(command, "y");
		const auto z = commandInteger(command, "z");
		if(!heroID || !x || !y || !z)
			return false;

		const auto * hero = cc->getHero(ObjectInstanceID(*heroID));
		if(!hero)
			return false;

		const int3 visitableDestination(*x, *y, *z);
		cc->moveHero(hero, hero->convertFromVisitablePos(visitableDestination), false);
		return true;
	}

	logAi->warn("LuaNullkiller2 unsupported command: %s", command.name.c_str());
	return false;
}

void CLuaNullkiller2AI::yourTurn(QueryID queryID)
{
	answerQuery(queryID);

	LuaNullkiller2Runner runner;
	LuaRunInput input;
	input.difficultyLevel = cc->getStartInfo()->difficulty;
	input.commandHandler = [this](const LuaCommand & command)
	{
		return executeCommand(command);
	};

	const LuaTurnResult result = runner.runDay([this]()
	{
		cc->endTurn();
	}, input);

	if(!result.ok)
		logAi->error("LuaNullkiller2 runDay failed: %s", result.error);

	if(!result.requestedEndTurn)
		cc->endTurn();
}

void CLuaNullkiller2AI::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID)
{
	answerQuery(queryID);
}

void CLuaNullkiller2AI::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID)
{
	answerQuery(queryID);
}

void CLuaNullkiller2AI::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	answerQuery(askID);
}

void CLuaNullkiller2AI::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	answerQuery(queryID);
}

void CLuaNullkiller2AI::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	answerQuery(askID);
}

void CLuaNullkiller2AI::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	answerQuery(askID);
}

std::optional<BattleAction> CLuaNullkiller2AI::makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState)
{
	return std::nullopt;
}

}
