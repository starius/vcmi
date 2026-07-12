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

#include "../../lib/battle/BattleAction.h"
#include "../../lib/callback/CCallback.h"

namespace LuaNullkiller2AI
{

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

void CLuaNullkiller2AI::yourTurn(QueryID queryID)
{
	answerQuery(queryID);
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
