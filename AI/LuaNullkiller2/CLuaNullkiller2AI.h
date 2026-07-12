/*
 * CLuaNullkiller2AI.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/callback/CAdventureAI.h"

namespace LuaNullkiller2AI
{

struct LuaCommand;

class CLuaNullkiller2AI final : public CAdventureAI
{
	std::shared_ptr<CCallback> cc;

	void answerQuery(QueryID queryID, int selection = 0) const;
	bool executeCommand(const LuaCommand & command) const;

public:
	std::string getBattleAIName() const override;

	void initGameInterface(std::shared_ptr<Environment> env, std::shared_ptr<CCallback> callback) override;
	void yourTurn(QueryID queryID) override;
	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	std::optional<BattleAction> makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState) override;
};

}
