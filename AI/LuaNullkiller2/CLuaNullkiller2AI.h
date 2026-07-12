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
#include "../../lib/json/JsonNode.h"

class CGObjectInstance;
class CGDwelling;
class IMarket;

namespace LuaNullkiller2AI
{

struct LuaCommand;
struct LuaRunInput;
struct LuaTurnResult;

class CLuaNullkiller2AI final : public CAdventureAI
{
	std::shared_ptr<CCallback> cc;
	JsonNode memory;
	int activeHeroID = -1;
	int targetObjectID = 0;
	int targetX = -1;
	int targetY = -1;
	int targetZ = -1;
	bool pathfinderInvalidated = false;

	void answerQuery(QueryID queryID, int selection = 0) const;
	bool executeCommand(const LuaCommand & command);
	LuaRunInput makeRunInput() const;
	void updateMemory(const LuaTurnResult & result);
	void runEventCallback(const std::string & functionName, JsonNode snapshot);
	void runQueryCallback(const std::string & functionName, QueryID queryID);

public:
	std::string getBattleAIName() const override;

	void initGameInterface(std::shared_ptr<Environment> env, std::shared_ptr<CCallback> callback) override;
	void yourTurn(QueryID queryID) override;
	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID) override;
	void showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QueryID queryID) override;
	void showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID) override;
	void showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QueryID queryID) override;
	void heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query) override;
	void playerBlocked(int reason, bool start) override;
	void availableCreaturesChanged(const CGDwelling * town) override;
	void heroInGarrisonChange(const CGTownInstance * town) override;
	void artifactMoved(const ArtifactLocation & src, const ArtifactLocation & dst) override;
	void artifactAssembled(const ArtifactLocation & al) override;
	void artifactPut(const ArtifactLocation & al) override;
	void artifactRemoved(const ArtifactLocation & al) override;
	void artifactDisassembled(const ArtifactLocation & al) override;
	void availableArtifactsChanged(const CGBlackMarket * bm = nullptr) override;
	void heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town) override;
	void heroExperienceChanged(const CGHeroInstance * hero, si64 val) override;
	void heroPrimarySkillChanged(const CGHeroInstance * hero, PrimarySkill which, si64 val) override;
	void heroMovePointsChanged(const CGHeroInstance * hero) override;
	void garrisonsChanged(ObjectInstanceID id1, ObjectInstanceID id2) override;
	void playerBonusChanged(const Bonus & bonus, bool gain) override;
	void advmapSpellCast(const CGHeroInstance * caster, SpellID spellID) override;
	void requestRealized(PackageApplied * pa) override;
	void receivedResource() override;
	void heroManaPointsChanged(const CGHeroInstance * hero) override;
	void heroSecondarySkillChanged(const CGHeroInstance * hero, int which, int val) override;
	void beforeObjectPropertyChanged(const SetObjectProperty * sop) override;
	void buildChanged(const CGTownInstance * town, BuildingID buildingID, int what) override;
	void heroBonusChanged(const CGHeroInstance * hero, const Bonus & bonus, bool gain) override;
	void heroCreated(const CGHeroInstance * hero) override;
	void heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start) override;
	void heroMoved(const TryMoveHero & details, bool verbose = true) override;
	void newObject(const CGObjectInstance * obj) override;
	void objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator) override;
	void objectPropertyChanged(const SetObjectProperty * sop) override;
	void tileHidden(const FowTilesType & pos) override;
	void tileRevealed(const FowTilesType & pos) override;
	void battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed) override;
	void battleEnd(const BattleID & battleID, const BattleResult * battleResult, QueryID queryID) override;
	void battleResultsApplied() override;
	void battleEnded() override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	std::optional<BattleAction> makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState) override;
};

}
