/*
 * AIGateway.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "../../lib/AsyncRunner.h"
#include "../../lib/UnlockGuard.h"
#include "../../lib/StartInfo.h"
#include "../../lib/battle/CPlayerBattleCallback.h"
#include "../../lib/entities/artifact/ArtifactUtils.h"
#include "../../lib/entities/artifact/CArtifact.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/mapObjects/MapObjects.h"
#include "../../lib/mapObjects/ObjectTemplate.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapping/TerrainTile.h"
#include "../../lib/CConfigHandler.h"
#include "../../lib/IGameSettings.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/gameState/UpgradeInfo.h"
#include "../../lib/serializer/CTypeList.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../lib/networkPacks/StackLocation.h"
#include "../../lib/battle/BattleStateInfoForRetreat.h"
#include "../../lib/battle/BattleInfo.h"
#include "../../lib/CPlayerState.h"

#include "AIGateway.h"
#include "Goals/Goals.h"
#include "Engine/NativeTrace.h"

namespace NK2AI
{

namespace
{
JsonNode objectIDsSnapshot(const std::vector<ObjectInstanceID> & objects)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & object : objects)
		result.Vector().push_back(JsonNode(static_cast<int64_t>(object.getNum())));
	return result;
}

std::string componentTypeName(ComponentType type)
{
	switch(type)
	{
	case ComponentType::NONE:
		return "NONE";
	case ComponentType::PRIM_SKILL:
		return "PRIM_SKILL";
	case ComponentType::SEC_SKILL:
		return "SEC_SKILL";
	case ComponentType::RESOURCE:
		return "RESOURCE";
	case ComponentType::RESOURCE_PER_DAY:
		return "RESOURCE_PER_DAY";
	case ComponentType::CREATURE:
		return "CREATURE";
	case ComponentType::ARTIFACT:
		return "ARTIFACT";
	case ComponentType::SPELL_SCROLL:
		return "SPELL_SCROLL";
	case ComponentType::MANA:
		return "MANA";
	case ComponentType::EXPERIENCE:
		return "EXPERIENCE";
	case ComponentType::LEVEL:
		return "LEVEL";
	case ComponentType::SPELL:
		return "SPELL";
	case ComponentType::MORALE:
		return "MORALE";
	case ComponentType::LUCK:
		return "LUCK";
	case ComponentType::BUILDING:
		return "BUILDING";
	case ComponentType::HERO_PORTRAIT:
		return "HERO_PORTRAIT";
	case ComponentType::FLAG:
		return "FLAG";
	}
	return "UNKNOWN";
}

std::string objectTypeName(Obj type)
{
	switch(type)
	{
	case Obj::ARTIFACT:
		return "ARTIFACT";
	case Obj::BORDERGUARD:
		return "BORDERGUARD";
	case Obj::QUEST_GUARD:
		return "QUEST_GUARD";
	case Obj::RESOURCE:
		return "RESOURCE";
	default:
		return "";
	}
}

JsonNode tileSnapshot(const int3 & tile)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["x"].Integer() = tile.x;
	result["y"].Integer() = tile.y;
	result["z"].Integer() = tile.z;
	result["valid"].Bool() = tile.isValid();
	return result;
}

JsonNode componentSnapshot(const Component & component)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["type"].String() = componentTypeName(component.type);
	result["typeID"].Integer() = static_cast<int>(component.type);
	if(component.subType.hasValue())
		result["subType"].Integer() = component.subType.getNum();
	if(component.value)
		result["value"].Integer() = *component.value;
	return result;
}

JsonNode componentsSnapshot(const std::vector<Component> & components)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & component : components)
		result.Vector().push_back(componentSnapshot(component));
	return result;
}

JsonNode blockingHeroSnapshot(const HeroPtr & heroPtr, std::optional<HeroRole> role = std::nullopt)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["verified"].Bool() = heroPtr.isVerified(false);
	if(!heroPtr.isVerified(false))
		return result;

	const auto * hero = heroPtr.get();
	result["id"].Integer() = hero->id.getNum();
	result["totalStrength"].Integer() = static_cast<int64_t>(hero->getTotalStrength());
	if(role)
		result["role"].Integer() = static_cast<int>(*role);
	return result;
}

JsonNode blockingObjectSnapshot(const CGObjectInstance * object, int64_t danger)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["id"].Integer() = object->id.getNum();
	result["typeID"].Integer() = static_cast<int>(object->ID);
	const std::string typeName = objectTypeName(object->ID);
	if(!typeName.empty())
		result["ID"].String() = typeName;
	result["danger"].Integer() = danger;
	return result;
}

JsonNode blockingObjectsSnapshot(const std::vector<const CGObjectInstance *> & objects, const std::unique_ptr<FuzzyHelper> & dangerEvaluator)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * object : objects)
		result.Vector().push_back(blockingObjectSnapshot(object, dangerEvaluator->evaluateDanger(object)));
	return result;
}

JsonNode answerQueryCommandJournal(QueryID queryID, int selection)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);

	JsonNode command;
	command.setType(JsonNode::JsonType::DATA_STRUCT);
	command["name"].String() = "answerQuery";
	command["payload"].setType(JsonNode::JsonType::DATA_STRUCT);
	command["payload"]["query"].Integer() = queryID.getNum();
	command["payload"]["selection"].Integer() = selection;
	result.Vector().push_back(std::move(command));

	return result;
}

JsonNode answerQueryInput(QueryID queryID)
{
	JsonNode input;
	input.setType(JsonNode::JsonType::DATA_STRUCT);
	input["queryID"].Integer() = queryID.getNum();
	return input;
}

JsonNode answerQueryNativeOutput(QueryID queryID, int selection)
{
	JsonNode output;
	output.setType(JsonNode::JsonType::DATA_STRUCT);
	output["status"].String() = "answered";
	output["selection"].Integer() = selection;
	output["commandJournal"] = answerQueryCommandJournal(queryID, selection);
	return output;
}

void recordAnswerQueryNativeTrace(NativeTrace * nativeTrace, const std::string & functionName, QueryID queryID, int selection)
{
	if(!nativeTrace)
		return;

	nativeTrace->recordDecision(
		functionName + "." + std::to_string(queryID.getNum()),
		functionName,
		answerQueryInput(queryID),
		answerQueryNativeOutput(queryID, selection),
		{ "status", "selection", "commandJournal" });
}

JsonNode emptyInput()
{
	JsonNode input;
	input.setType(JsonNode::JsonType::DATA_STRUCT);
	return input;
}

JsonNode objectReferenceSnapshot(const CGObjectInstance * object)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!object)
		return result;

	result["id"].Integer() = object->id.getNum();
	result["typeID"].Integer() = static_cast<int>(object->ID);
	return result;
}

JsonNode heroReferenceSnapshot(const CGHeroInstance * hero)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	if(!hero)
		return result;

	result["id"].Integer() = hero->id.getNum();
	result["level"].Integer() = hero->level;
	return result;
}

JsonNode artifactLocationReferenceSnapshot(const ArtifactLocation & location)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["holder"].Integer() = location.artHolder.getNum();
	result["slot"].Integer() = location.slot.getNum();
	return result;
}

JsonNode statusNativeOutput(const std::string & status)
{
	JsonNode output;
	output.setType(JsonNode::JsonType::DATA_STRUCT);
	output["status"].String() = status;
	return output;
}

void recordStatusNativeTrace(NativeTrace * nativeTrace, const std::string & id, const std::string & functionName, JsonNode input, const std::string & status)
{
	if(!nativeTrace)
		return;

	nativeTrace->recordDecision(
		id,
		functionName,
		std::move(input),
		statusNativeOutput(status),
		{ "status" });
}

JsonNode mapObjectSelectInput(QueryID queryID, int selection, const std::vector<ObjectInstanceID> & objects)
{
	JsonNode input;
	input.setType(JsonNode::JsonType::DATA_STRUCT);
	input["queryID"].Integer() = queryID.getNum();
	input["selectedObject"].Integer() = selection;
	input["objects"] = objectIDsSnapshot(objects);
	return input;
}

JsonNode mapObjectSelectNativeOutput(QueryID queryID, int selection)
{
	return answerQueryNativeOutput(queryID, selection);
}

JsonNode blockingDialogInput(
	QueryID queryID,
	const std::string & text,
	const std::vector<Component> & components,
	int soundID,
	bool selection,
	bool cancel,
	bool safeToAutoaccept)
{
	JsonNode input;
	input.setType(JsonNode::JsonType::DATA_STRUCT);
	input["queryID"].Integer() = queryID.getNum();
	input["text"].String() = text;
	input["soundID"].Integer() = soundID;
	input["selection"].Bool() = selection;
	input["cancel"].Bool() = cancel;
	input["safeToAutoaccept"].Bool() = safeToAutoaccept;
	input["components"] = componentsSnapshot(components);
	return input;
}

JsonNode blockingDialogNativeOutput(QueryID queryID, int selection)
{
	return answerQueryNativeOutput(queryID, selection);
}

JsonNode teleportExitsSnapshot(const TTeleportExitsList & exits, const std::shared_ptr<CCallback> & callback)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & exit : exits)
	{
		JsonNode item;
		item.setType(JsonNode::JsonType::DATA_STRUCT);
		item["id"].Integer() = exit.first.getNum();
		item["pos"] = tileSnapshot(exit.second);
		item["visible"].Bool() = callback->getObj(exit.first, false) != nullptr;
		result.Vector().push_back(std::move(item));
	}
	return result;
}

JsonNode teleportDialogInput(
	QueryID queryID,
	TeleportChannelID channel,
	const TTeleportExitsList & exits,
	bool impassable,
	ObjectInstanceID destinationTeleport,
	const int3 & destinationTeleportPos,
	bool channelProbing,
	const std::vector<ObjectInstanceID> & probingList,
	const std::shared_ptr<CCallback> & callback)
{
	JsonNode input;
	input.setType(JsonNode::JsonType::DATA_STRUCT);
	input["queryID"].Integer() = queryID.getNum();
	input["channel"].Integer() = channel.getNum();
	input["impassable"].Bool() = impassable;
	input["exits"] = teleportExitsSnapshot(exits, callback);
	if(destinationTeleport != ObjectInstanceID())
		input["destinationTeleport"].Integer() = destinationTeleport.getNum();
	if(destinationTeleportPos.isValid())
		input["destinationTeleportPos"] = tileSnapshot(destinationTeleportPos);
	input["channelProbing"].Bool() = channelProbing;
	input["teleportChannelProbingList"] = objectIDsSnapshot(probingList);
	return input;
}

JsonNode heroSecondarySkillsSnapshot(const std::vector<std::pair<SecondarySkill, ui8>> & skills)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & skill : skills)
	{
		JsonNode entry;
		entry.setType(JsonNode::JsonType::DATA_STRUCT);
		entry["skill"].Integer() = skill.first.getNum();
		entry["level"].Integer() = skill.second;
		result.Vector().push_back(std::move(entry));
	}
	return result;
}

JsonNode secondarySkillIDsSnapshot(const std::vector<SecondarySkill> & skills)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & skill : skills)
		result.Vector().push_back(JsonNode(static_cast<int64_t>(skill.getNum())));
	return result;
}

JsonNode primarySkillsSnapshot(const CGHeroInstance * hero)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["attack"].Integer() = hero->getBasePrimarySkillValue(PrimarySkill::ATTACK);
	result["defense"].Integer() = hero->getBasePrimarySkillValue(PrimarySkill::DEFENSE);
	result["spellPower"].Integer() = hero->getBasePrimarySkillValue(PrimarySkill::SPELL_POWER);
	result["knowledge"].Integer() = hero->getBasePrimarySkillValue(PrimarySkill::KNOWLEDGE);
	return result;
}

JsonNode heroLevelHeroSnapshot(const CGHeroInstance * hero, const HeroManager & heroManager, std::optional<HeroRole> role = std::nullopt)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["verified"].Bool() = true;
	result["id"].Integer() = hero->id.getNum();
	result["level"].Integer() = hero->level;
	result["evaluateHeroScore"].Float() = heroManager.evaluateHero(hero);
	result["totalStrength"].Integer() = static_cast<int64_t>(hero->getTotalStrength());
	result["armyStrength"].Integer() = static_cast<int64_t>(hero->getArmyStrength());
	result["primarySkills"] = primarySkillsSnapshot(hero);
	result["secSkills"] = heroSecondarySkillsSnapshot(hero->secSkills);
	result["patrol"].setType(JsonNode::JsonType::DATA_STRUCT);
	result["patrol"]["patrolling"].Bool() = hero->patrol.patrolling;
	if(role)
		result["role"].Integer() = static_cast<int>(*role);
	return result;
}

JsonNode heroLevelHeroesSnapshot(const std::vector<const CGHeroInstance *> & heroes, const HeroManager & heroManager)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * hero : heroes)
		result.Vector().push_back(heroLevelHeroSnapshot(hero, heroManager, heroManager.getHeroRoleOrDefaultInefficient(hero)));
	return result;
}

JsonNode heroLevelTownsSnapshot(const std::vector<const CGTownInstance *> & towns)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * town : towns)
	{
		JsonNode entry;
		entry.setType(JsonNode::JsonType::DATA_STRUCT);
		entry["id"].Integer() = town->id.getNum();
		result.Vector().push_back(std::move(entry));
	}
	return result;
}

JsonNode mapSizeSnapshot(const int3 & mapSize)
{
	JsonNode result;
	result.setType(JsonNode::JsonType::DATA_STRUCT);
	result["x"].Integer() = mapSize.x;
	result["y"].Integer() = mapSize.y;
	result["z"].Integer() = mapSize.z;
	return result;
}

JsonNode heroGotLevelInput(
	QueryID queryID,
	const HeroPtr & heroPtr,
	PrimarySkill primarySkill,
	const std::vector<SecondarySkill> & skills,
	const std::shared_ptr<CCallback> & callback,
	const HeroManager & heroManager,
	std::optional<HeroRole> role)
{
	JsonNode input;
	input.setType(JsonNode::JsonType::DATA_STRUCT);
	input["queryID"].Integer() = queryID.getNum();
	input["primarySkill"].Integer() = primarySkill.getNum();
	input["skills"] = secondarySkillIDsSnapshot(skills);
	input["currentDay"].Integer() = callback->getCalendar().getCurrentDay();
	input["mapSize"] = mapSizeSnapshot(callback->getMapSize());
	input["townsInfo"] = heroLevelTownsSnapshot(callback->getTownsInfo());
	input["heroesInfo"] = heroLevelHeroesSnapshot(callback->getHeroesInfo(), heroManager);

	input["hero"].setType(JsonNode::JsonType::DATA_STRUCT);
	input["hero"]["verified"].Bool() = heroPtr.isVerified(false);
	if(heroPtr.isVerified(false))
		input["hero"] = heroLevelHeroSnapshot(heroPtr.get(), heroManager, role);

	return input;
}

JsonNode heroGotLevelNativeOutput(QueryID queryID, int selection, std::optional<HeroRole> role)
{
	JsonNode output = answerQueryNativeOutput(queryID, selection);
	if(role)
		output["role"].Integer() = static_cast<int>(*role);
	return output;
}

JsonNode surrenderRetreatInput(const BattleStateInfoForRetreat & battleState, size_t townsCount, const Settings & settings)
{
	JsonNode input;
	input.setType(JsonNode::JsonType::DATA_STRUCT);
	input["townsCount"].Integer() = static_cast<int64_t>(townsCount);
	input["settings"].setType(JsonNode::JsonType::DATA_STRUCT);
	input["settings"]["values"].setType(JsonNode::JsonType::DATA_STRUCT);
	input["settings"]["values"]["retreatThresholdAbsolute"].Float() = settings.getRetreatThresholdAbsolute();
	input["settings"]["values"]["retreatThresholdRelative"].Float() = settings.getRetreatThresholdRelative();
	input["battleState"].setType(JsonNode::JsonType::DATA_STRUCT);
	input["battleState"]["ourStrength"].Integer() = static_cast<int64_t>(battleState.getOurStrength());
	input["battleState"]["enemyStrength"].Integer() = static_cast<int64_t>(battleState.getEnemyStrength());
	input["battleState"]["canFlee"].Bool() = battleState.canFlee;
	input["battleState"]["canSurrender"].Bool() = battleState.canSurrender;
	input["battleState"]["isLastTurnBeforeDie"].Bool() = battleState.isLastTurnBeforeDie;
	input["battleState"]["ourSide"].Integer() = static_cast<int>(battleState.ourSide);

	if(battleState.ourHero)
	{
		input["battleState"]["ourHero"].setType(JsonNode::JsonType::DATA_STRUCT);
		input["battleState"]["ourHero"]["id"].Integer() = battleState.ourHero->id.getNum();
		input["battleState"]["ourHero"]["patrol"].setType(JsonNode::JsonType::DATA_STRUCT);
		input["battleState"]["ourHero"]["patrol"]["patrolling"].Bool() = battleState.ourHero->patrol.patrolling;
	}

	return input;
}

JsonNode surrenderRetreatNativeOutput(std::optional<int> retreatSide)
{
	JsonNode output;
	output.setType(JsonNode::JsonType::DATA_STRUCT);
	if(retreatSide)
	{
		output["status"].String() = "retreat";
		output["side"].Integer() = *retreatSide;
	}
	else
	{
		output["status"].String() = "none";
	}
	return output;
}
}

AIGateway::AIGateway()
	:status(this)
{
	LOG_TRACE(logAi);
	destinationTeleport = ObjectInstanceID();
	destinationTeleportPos = int3(-1);
	nullkiller.reset(new Nullkiller());
	nativeTrace = std::make_unique<NativeTrace>();
	asyncTasks = std::make_unique<AsyncRunner>();
}

AIGateway::~AIGateway()
{
	LOG_TRACE(logAi);
	finish();
	nullkiller.reset();
}

void AIGateway::availableCreaturesChanged(const CGDwelling * town)
{
	LOG_TRACE(logAi);
}

void AIGateway::heroMoved(const TryMoveHero & details, bool verbose)
{
	LOG_TRACE(logAi);
	const auto hero = cc->getHero(details.id);
	if(!hero)
		validateObject(ObjectIdRef(details.id, cc.get())); //enemy hero may have left visible area

	nullkiller->invalidatePathfinderData();

	const int3 from = hero ? hero->convertToVisitablePos(details.start) : (details.start - int3(0,1,0));
	const int3 to   = hero ? hero->convertToVisitablePos(details.end)   : (details.end   - int3(0,1,0));

	const CGObjectInstance * o1 = vstd::frontOrNull(cc->getVisitableObjs(from, verbose));
	const CGObjectInstance * o2 = vstd::frontOrNull(cc->getVisitableObjs(to, verbose));

	if(details.result == TryMoveHero::TELEPORTATION)
	{
		auto t1 = dynamic_cast<const CGTeleport *>(o1);
		auto t2 = dynamic_cast<const CGTeleport *>(o2);
		if(t1 && t2)
		{
			if(cc->isTeleportChannelBidirectional(t1->channel))
			{
				if(o1->ID == Obj::SUBTERRANEAN_GATE && o1->ID == o2->ID) // We need to only add subterranean gates in knownSubterraneanGates. Used for features not yet ported to use teleport channels
				{
					nullkiller->memory->addSubterraneanGate(o1, o2);
				}
			}
		}
	}
	else if(details.result == TryMoveHero::EMBARK && hero)
	{
		//make sure AI not attempt to visit used boat
		validateObject(hero->getBoat());
	}
	else if(details.result == TryMoveHero::DISEMBARK && o1)
	{
		auto boat = dynamic_cast<const CGBoat *>(o1);
		if(boat)
			memorizeVisitableObj(boat, nullkiller->memory, nullkiller->dangerHitMap, playerID, cc);
	}
}

void AIGateway::heroInGarrisonChange(const CGTownInstance * town)
{
	LOG_TRACE(logAi);
}

void AIGateway::centerView(int3 pos, int focusTime)
{
	LOG_TRACE_PARAMS(logAi, "focusTime '%i'", focusTime);
}

void AIGateway::artifactMoved(const ArtifactLocation & src, const ArtifactLocation & dst)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	input["src"] = artifactLocationReferenceSnapshot(src);
	input["dst"] = artifactLocationReferenceSnapshot(dst);
	const auto id = boost::str(
		boost::format("artifactMoved.%d.%d.%d.%d")
		% src.artHolder.getNum()
		% src.slot.getNum()
		% dst.artHolder.getNum()
		% dst.slot.getNum());
	recordStatusNativeTrace(
		nativeTrace.get(),
		id,
		"artifactMoved",
		std::move(input),
		"artifact_moved");
}

void AIGateway::artifactAssembled(const ArtifactLocation & al)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	input["location"] = artifactLocationReferenceSnapshot(al);
	const auto id = boost::str(
		boost::format("artifactAssembled.%d.%d")
		% al.artHolder.getNum()
		% al.slot.getNum());
	recordStatusNativeTrace(
		nativeTrace.get(),
		id,
		"artifactAssembled",
		std::move(input),
		"artifact_assembled");
}

void AIGateway::showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QueryID queryID)
{
	LOG_TRACE(logAi);
	status.addQuery(queryID, "TavernWindow");
	executeActionAsync("showTavernWindow", [this, queryID]()
	{
		recordAnswerQueryNativeTrace(nativeTrace.get(), "showTavernWindow", queryID, 0);
		answerQuery(queryID, 0);
	});
}

void AIGateway::showThievesGuildWindow(const CGObjectInstance * obj)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	if(obj)
		input["object"] = objectReferenceSnapshot(obj);
	recordStatusNativeTrace(
		nativeTrace.get(),
		boost::str(boost::format("showThievesGuildWindow.%d") % (obj ? obj->id.getNum() : -1)),
		"showThievesGuildWindow",
		std::move(input),
		"show_thieves_guild_window");
}

void AIGateway::playerBlocked(int reason, bool start)
{
	LOG_TRACE_PARAMS(logAi, "reason '%i', start '%i'", reason % start);
	if(start && reason == PlayerBlocked::UPCOMING_BATTLE)
		status.setBattle(UPCOMING_BATTLE);

	if(reason == PlayerBlocked::ONGOING_MOVEMENT)
		status.setMove(start);
}

void AIGateway::showPuzzleMap()
{
	LOG_TRACE(logAi);
}

void AIGateway::showShipyardDialog(const IShipyard * obj)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	const auto * mapObject = dynamic_cast<const CGObjectInstance *>(obj);
	if(mapObject)
		input["object"] = objectReferenceSnapshot(mapObject);
	recordStatusNativeTrace(
		nativeTrace.get(),
		boost::str(boost::format("showShipyardDialog.%d") % (mapObject ? mapObject->id.getNum() : -1)),
		"showShipyardDialog",
		std::move(input),
		"show_shipyard_dialog");
}

void AIGateway::gameOver(PlayerColor player, const EVictoryLossCheckResult & victoryLossCheckResult)
{
	LOG_TRACE_PARAMS(logAi, "victoryLossCheckResult '%s'", victoryLossCheckResult.messageToSelf.toString());
	logAi->debug("Player %d (%s): I heard that player %d (%s) %s.", playerID, playerID.toString(), player, player.toString(), (victoryLossCheckResult.victory() ? "won" : "lost"));

	// some whitespace to flush stream
	logAi->debug(std::string(200, ' '));

	if(player == playerID)
	{
		if(victoryLossCheckResult.victory())
		{
			logAi->debug("AIGateway: Player %d (%s) won. I won! Incredible!", player, player.toString());
			logAi->debug("Turn nr %d", cc->getCalendar().getCurrentDay());
		}
		else
		{
			logAi->debug("AIGateway: Player %d (%s) lost. It's me. What a disappointment! :(", player, player.toString());
		}

		nullkiller->makingTurnInterruption.interruptThread();
	}
}

void AIGateway::artifactPut(const ArtifactLocation & al)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	input["location"] = artifactLocationReferenceSnapshot(al);
	const auto id = boost::str(
		boost::format("artifactPut.%d.%d")
		% al.artHolder.getNum()
		% al.slot.getNum());
	recordStatusNativeTrace(
		nativeTrace.get(),
		id,
		"artifactPut",
		std::move(input),
		"artifact_put");
}

void AIGateway::artifactRemoved(const ArtifactLocation & al)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	input["location"] = artifactLocationReferenceSnapshot(al);
	const auto id = boost::str(
		boost::format("artifactRemoved.%d.%d")
		% al.artHolder.getNum()
		% al.slot.getNum());
	recordStatusNativeTrace(
		nativeTrace.get(),
		id,
		"artifactRemoved",
		std::move(input),
		"artifact_removed");
}

void AIGateway::artifactDisassembled(const ArtifactLocation & al)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	input["location"] = artifactLocationReferenceSnapshot(al);
	const auto id = boost::str(
		boost::format("artifactDisassembled.%d.%d")
		% al.artHolder.getNum()
		% al.slot.getNum());
	recordStatusNativeTrace(
		nativeTrace.get(),
		id,
		"artifactDisassembled",
		std::move(input),
		"artifact_disassembled");
}

void AIGateway::heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start)
{
	LOG_TRACE_PARAMS(logAi, "start '%i'; obj '%s'", start % (visitedObj ? visitedObj->getObjectName() : std::string("n/a")));

	if(start && visitedObj) //we can end visit with null object, anyway
	{
		nullkiller->memory->markObjectVisited(visitedObj);
		nullkiller->objectClusterizer->invalidate(visitedObj->id);
	}

	status.heroVisit(visitedObj, start);
}

void AIGateway::availableArtifactsChanged(const CGBlackMarket * bm)
{
	LOG_TRACE(logAi);
}

void AIGateway::heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town)
{
	LOG_TRACE(logAi);
}

void AIGateway::tileHidden(const FowTilesType & pos)
{
	LOG_TRACE(logAi);

	nullkiller->memory->removeInvisibleOrDeletedObjects(*cc);
}

void AIGateway::tileRevealed(const FowTilesType & pos)
{
	LOG_TRACE(logAi);

	for(int3 tile : pos)
	{
		for(const CGObjectInstance * obj : cc->getVisitableObjs(tile))
			memorizeVisitableObj(obj, nullkiller->memory, nullkiller->dangerHitMap, playerID, cc);
	}

	if (nullkiller->settings->isUpdateHitmapOnTileReveal() && !pos.empty())
		nullkiller->dangerHitMap->resetTileOwners();
}

void AIGateway::heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query)
{
	LOG_TRACE(logAi);
	auto firstHero = cc->getHero(hero1);
	auto secondHero = cc->getHero(hero2);

	status.addQuery(
		query,
		boost::str(
			boost::format("Exchange between heroes %s (%d) and %s (%d)") % firstHero->getNameTranslated() % firstHero->tempOwner
			% secondHero->getNameTranslated() % secondHero->tempOwner
		)
	);

	executeActionAsync(
		"heroExchangeStarted",
		[this, firstHero, secondHero, query]()
		{
			auto transferFrom2to1 = [this](const CGHeroInstance * h1, const CGHeroInstance * h2) -> void
			{
				this->pickBestCreatures(h1, h2);
				pickBestArtifacts(cc, h1, h2);
			};

			//Do not attempt army or artifacts exchange if we visited ally player
			//Visits can still be useful if hero have skills like Scholar
			if(firstHero->tempOwner != secondHero->tempOwner)
			{
				logAi->debug("Heroes owned by different players. Do not exchange army or artifacts.");
			}
			else
			{
				if(nullkiller->isActive(firstHero))
					transferFrom2to1(secondHero, firstHero);
				else
					transferFrom2to1(firstHero, secondHero);
			}

			answerQuery(query, 0);
		}
	);
}

void AIGateway::heroExperienceChanged(const CGHeroInstance * hero, si64 val)
{
	LOG_TRACE_PARAMS(logAi, "val '%i'", val);
}

void AIGateway::heroPrimarySkillChanged(const CGHeroInstance * hero, PrimarySkill which, si64 val)
{
	LOG_TRACE_PARAMS(logAi, "which '%i', val '%i'", which.getNum() % val);
}

void AIGateway::showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QueryID queryID)
{
	LOG_TRACE_PARAMS(logAi, "level '%i'", level);
	status.addQuery(queryID, "RecruitmentDialog");

	executeActionAsync("showRecruitmentDialog", [this, dwelling, dst, queryID](){
		recruitCreatures(dwelling, dst);
		answerQuery(queryID, 0);
	});
}

void AIGateway::heroMovePointsChanged(const CGHeroInstance * hero)
{
	LOG_TRACE(logAi);
}

void AIGateway::garrisonsChanged(ObjectInstanceID id1, ObjectInstanceID id2)
{
	LOG_TRACE(logAi);
}

void AIGateway::newObject(const CGObjectInstance * obj)
{
	LOG_TRACE(logAi);
	nullkiller->invalidatePathfinderData();
	if(obj->isVisitable())
		memorizeVisitableObj(obj, nullkiller->memory, nullkiller->dangerHitMap, playerID, cc);
}

//to prevent AI from accessing objects that got deleted while they became invisible (Cover of Darkness, enemy hero moved etc.) below code allows AI to know deletion of objects out of sight
//see: RemoveObject::applyFirstCl, to keep AI "not cheating" do not use advantage of this and use this function just to prevent crashes
void AIGateway::objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator)
{
	LOG_TRACE(logAi);
	if(!nullkiller) // crash protection
		return;

	nullkiller->memory->removeFromMemory(obj);
	nullkiller->objectClusterizer->onObjectRemoved(obj->id);

	if(nullkiller->baseGraph && nullkiller->isObjectGraphAllowed())
	{
		nullkiller->baseGraph->removeObject(obj, *nullkiller->cc);
	}

	if(obj->ID == Obj::HERO && obj->tempOwner == playerID)
	{
		lostHero(HeroPtr(cc->getHero(obj->id), cc.get())); //we can promote, since objectRemoved is called just before actual deletion
	}

	if(obj->ID == Obj::HERO && cc->getPlayerRelations(obj->tempOwner, playerID) == PlayerRelations::ENEMIES)
		nullkiller->dangerHitMap->resetHitmap();

	if(obj->ID == Obj::TOWN)
		nullkiller->dangerHitMap->resetTileOwners();
}

void AIGateway::showHillFortWindow(const CGObjectInstance * object, const CGHeroInstance * visitor)
{
	LOG_TRACE(logAi);
	JsonNode input = emptyInput();
	if(object)
		input["object"] = objectReferenceSnapshot(object);
	if(visitor)
		input["visitor"] = heroReferenceSnapshot(visitor);
	recordStatusNativeTrace(
		nativeTrace.get(),
		boost::str(boost::format("showHillFortWindow.%d.%d") % (object ? object->id.getNum() : -1) % (visitor ? visitor->id.getNum() : -1)),
		"showHillFortWindow",
		std::move(input),
		"show_hill_fort_window");
}

void AIGateway::playerBonusChanged(const Bonus & bonus, bool gain)
{
	LOG_TRACE_PARAMS(logAi, "gain '%i'", gain);
}

void AIGateway::heroCreated(const CGHeroInstance * h)
{
	LOG_TRACE(logAi);
	nullkiller->invalidatePathfinderData(); // new hero needs to look around
}

void AIGateway::advmapSpellCast(const CGHeroInstance * caster, SpellID spellID)
{
	LOG_TRACE_PARAMS(logAi, "spellID '%i", spellID);
}

void AIGateway::showInfoDialog(EInfoWindowMode type, const std::string & text, const std::vector<Component> & components, int soundID)
{
	LOG_TRACE_PARAMS(logAi, "soundID '%i'", soundID);
	JsonNode input = emptyInput();
	input["type"].Integer() = static_cast<int>(type);
	input["text"].String() = text;
	input["soundID"].Integer() = soundID;
	input["components"] = componentsSnapshot(components);
	recordStatusNativeTrace(
		nativeTrace.get(),
		boost::str(boost::format("showInfoDialog.%d") % soundID),
		"showInfoDialog",
		std::move(input),
		"show_info_dialog");
}

void AIGateway::requestRealized(PackageApplied * pa)
{
	LOG_TRACE(logAi);
	if(status.haveTurn())
	{
		if(pa->packType == CTypeList::getInstance().getTypeID<EndTurn>(nullptr))
		{
			if(pa->result)
				status.madeTurn();
		}
	}

	if(pa->packType == CTypeList::getInstance().getTypeID<QueryReply>(nullptr))
	{
		status.receivedAnswerConfirmation(pa->requestID, pa->result);
	}
}

void AIGateway::receivedResource()
{
	LOG_TRACE(logAi);
}

void AIGateway::showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	LOG_TRACE(logAi);

	status.addQuery(queryID, "UniversityWindow");
	executeActionAsync("showUniversityWindow", [this, queryID]()
	{
		recordAnswerQueryNativeTrace(nativeTrace.get(), "showUniversityWindow", queryID, 0);
		answerQuery(queryID, 0);
	});
}

void AIGateway::heroManaPointsChanged(const CGHeroInstance * hero)
{
	LOG_TRACE(logAi);
}

void AIGateway::heroSecondarySkillChanged(const CGHeroInstance * hero, int which, int val)
{
	LOG_TRACE_PARAMS(logAi, "which '%d', val '%d'", which % val);
}

void AIGateway::battleResultsApplied()
{
	LOG_TRACE(logAi);
	assert(status.getBattle() == ENDING_BATTLE);
}

void AIGateway::battleEnded()
{
	LOG_TRACE(logAi);
	assert(status.getBattle() == ENDING_BATTLE);
	status.setBattle(NO_BATTLE);
}

void AIGateway::beforeObjectPropertyChanged(const SetObjectProperty * sop)
{

}


void AIGateway::objectPropertyChanged(const SetObjectProperty * sop)
{
	LOG_TRACE(logAi);
	if(sop->what == ObjProperty::OWNER)
	{
		auto relations = cc->getPlayerRelations(playerID, sop->identifier.as<PlayerColor>());
		auto obj = cc->getObj(sop->id, false);

		if(!nullkiller) // crash protection
			return;

		if(obj)
		{
			if(relations == PlayerRelations::ENEMIES)
			{
				//we want to visit objects owned by oppponents
				//addVisitableObj(obj); // TODO: Remove once save compatibility broken. In past owned objects were removed from this set
				nullkiller->memory->markObjectUnvisited(obj);
			}
			else if(relations == PlayerRelations::SAME_PLAYER && obj->ID == Obj::TOWN)
			{
				// reevaluate defence for a new town
				nullkiller->dangerHitMap->resetHitmap();
			}
		}
	}
}

void AIGateway::buildChanged(const CGTownInstance * town, BuildingID buildingID, int what)
{
	LOG_TRACE_PARAMS(logAi, "what '%i'", what);
}

void AIGateway::heroBonusChanged(const CGHeroInstance * hero, const Bonus & bonus, bool gain)
{
	LOG_TRACE_PARAMS(logAi, "gain '%i'", gain);
}

void AIGateway::showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	LOG_TRACE(logAi);
	status.addQuery(queryID, "MarketWindow");
	executeActionAsync("showMarketWindow", [this, queryID]()
	{
		recordAnswerQueryNativeTrace(nativeTrace.get(), "showMarketWindow", queryID, 0);
		answerQuery(queryID, 0);
	});
}

void AIGateway::showWorldViewEx(const std::vector<ObjectPosInfo> & objectPositions, bool showTerrain)
{
	//TODO: AI support for ViewXXX spell
	LOG_TRACE(logAi);
}

std::optional<BattleAction> AIGateway::makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState)
{
	LOG_TRACE(logAi);
	const auto townsCount = cc->getTownsInfo().size();
	const auto traceDecision = [this, &battleID, &battleState, townsCount](std::optional<int> retreatSide)
	{
		if(nativeTrace)
		{
			const std::vector<std::string> compareFields = retreatSide
				? std::vector<std::string>{ "status", "side" }
				: std::vector<std::string>{ "status" };

			nativeTrace->recordDecision(
				boost::str(boost::format("makeSurrenderRetreatDecision.%d") % battleID.getNum()),
				"makeSurrenderRetreatDecision",
				surrenderRetreatInput(battleState, townsCount, *nullkiller->settings),
				surrenderRetreatNativeOutput(retreatSide),
				compareFields);
		}
	};

	if(battleState.ourHero && battleState.ourHero->patrol.patrolling)
	{
		traceDecision(std::nullopt);
		return std::nullopt;
	}

	double ourStrength = battleState.getOurStrength();
	double fightRatio = ourStrength / (double)battleState.getEnemyStrength();

	// if we have no towns - things are already bad, so retreat is not an option.
	if(townsCount && ourStrength < nullkiller->settings->getRetreatThresholdAbsolute() && fightRatio < nullkiller->settings->getRetreatThresholdRelative() && battleState.canFlee)
	{
		traceDecision(static_cast<int>(battleState.ourSide));
		return BattleAction::makeRetreat(battleState.ourSide);
	}

	traceDecision(std::nullopt);
	return std::nullopt;
}


void AIGateway::initGameInterface(std::shared_ptr<Environment> env, std::shared_ptr<CCallback> callback)
{
	LOG_TRACE(logAi);
	cbc = callback;
	cc = callback;
	this->env = env;
	playerID = *cc->getPlayerID();
	cc->waitTillRealize = true;

	nullkiller->init(callback, this);
	memorizeVisitableObjs(nullkiller->memory, nullkiller->dangerHitMap, playerID, cc);
}

void AIGateway::yourTurn(QueryID queryID)
{
	LOG_TRACE_PARAMS(logAi, "queryID '%i'", queryID);
	nullkiller->invalidatePathfinderData();
	status.addQuery(queryID, "YourTurn");
	executeActionAsync("yourTurn", [this, queryID](){ answerQuery(queryID, 0); });
	status.startedTurn();

	nullkiller->makingTurnInterruption.reset();

	asyncTasks->run([this]()
	{
		ScopedThreadName guard("NK2AI::AIGateway::makingTurn");
		status.waitTillFree();
		makeTurn();
	});
}

void AIGateway::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID)
{
	LOG_TRACE_PARAMS(logAi, "queryID '%i'", queryID);
	status.addQuery(queryID, boost::str(boost::format("Hero %s got level %d") % hero->getNameTranslated() % hero->level));
	HeroPtr heroPtr(hero, cc.get());

	executeActionAsync("heroGotLevel", [this, heroPtr, pskill, skills, queryID]()
	{
		int sel = 0;
		std::optional<HeroRole> role;
		std::optional<JsonNode> traceInput;

		if(heroPtr.isVerified())
		{
			std::unique_lock lockGuard(nullkiller->aiStateMutex);
			nullkiller->heroManager->update();
			role = nullkiller->heroManager->getHeroRoleOrDefault(heroPtr);
			sel = nullkiller->heroManager->selectBestSkillIndex(heroPtr, skills);
			if(nativeTrace)
				traceInput = heroGotLevelInput(queryID, heroPtr, pskill, skills, cc, *nullkiller->heroManager, role);
		}
		else if(nativeTrace)
		{
			std::unique_lock lockGuard(nullkiller->aiStateMutex);
			nullkiller->heroManager->update();
			traceInput = heroGotLevelInput(queryID, heroPtr, pskill, skills, cc, *nullkiller->heroManager, role);
		}

		if(nativeTrace && traceInput)
		{
			std::vector<std::string> compareFields{ "status", "selection", "commandJournal" };
			if(role)
				compareFields.insert(compareFields.begin() + 2, "role");

			nativeTrace->recordDecision(
				boost::str(boost::format("heroGotLevel.%d") % queryID.getNum()),
				"heroGotLevel",
				std::move(*traceInput),
				heroGotLevelNativeOutput(queryID, sel, role),
				std::move(compareFields));
		}

		answerQuery(queryID, sel);
	});
}

void AIGateway::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID)
{
	LOG_TRACE_PARAMS(logAi, "queryID '%i'", queryID);
	status.addQuery(queryID, boost::str(boost::format("Commander %s of %s got level %d") % commander->name % commander->getArmy()->nodeName() % (int)commander->level));
	executeActionAsync("commanderGotLevel", [this, queryID]()
	{
		recordAnswerQueryNativeTrace(nativeTrace.get(), "commanderGotLevel", queryID, 0);
		answerQuery(queryID, 0);
	});
}

void AIGateway::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	LOG_TRACE_PARAMS(logAi, "text '%s', askID '%i', soundID '%i', selection '%i', cancel '%i', autoaccept '%i'", text % askID % soundID % selection % cancel % safeToAutoaccept);
	status.addQuery(askID, boost::str(boost::format("Blocking dialog query with %d components - %s")
									  % components.size() % text));

	auto heroPtr = nullkiller->getActiveHero();
	auto target = nullkiller->getTargetTile();

	if(!selection && cancel)
	{
		executeActionAsync("showBlockingDialog", [this, heroPtr, target, askID, text, components, soundID, selection, cancel, safeToAutoaccept]()
		{
			//yes&no -> always answer yes, we are a brave AI :)
			bool answer = true;
			auto objects = cc->getVisitableObjs(target);
			JsonNode traceInput = blockingDialogInput(askID, text, components, soundID, selection, cancel, safeToAutoaccept);
			traceInput["target"] = tileSnapshot(target);
			traceInput["targetObject"].Integer() = nullkiller->getTargetObject().getNum();
			traceInput["settings"].setType(JsonNode::JsonType::DATA_STRUCT);
			traceInput["settings"]["safeAttackRatio"].Float() = nullkiller->settings->getSafeAttackRatio();

			if(heroPtr.isVerified() && target.isValid() && !objects.empty())
			{
				auto topObj = objects.front()->id == heroPtr->id ? objects.back() : objects.front();
				auto objType = topObj->ID; // top object should be our hero
				auto goalObjectID = nullkiller->getTargetObject();
				auto danger = nullkiller->dangerEvaluator->evaluateDanger(target, heroPtr.get());
				auto ratio = static_cast<float>(danger) / heroPtr->getTotalStrength();
				traceInput["hero"] = blockingHeroSnapshot(heroPtr);
				traceInput["objects"] = blockingObjectsSnapshot(objects, nullkiller->dangerEvaluator);
				traceInput["danger"].Integer() = static_cast<int64_t>(danger);

				answer = true;

				if(topObj->id != goalObjectID && nullkiller->dangerEvaluator->evaluateDanger(topObj) > 0)
				{
					// no if we do not aim to visit this object
					answer = false;
				}

				logAi->trace("Query hook: %s(%s) by %s danger ratio %f", target.toString(), topObj->getObjectName(), heroPtr.nameOrDefault(), ratio);

				if(cc->getObj(goalObjectID, false))
				{
					logAi->trace("AI expected %s", cc->getObj(goalObjectID, false)->getObjectName());
				}

				if(objType == Obj::BORDERGUARD || objType == Obj::QUEST_GUARD)
				{
					answer = true;
				}
				else if(objType == Obj::ARTIFACT || objType == Obj::RESOURCE)
				{
					bool dangerUnknown = danger == 0;
					bool dangerTooHigh = ratio * nullkiller->settings->getSafeAttackRatio() > 1;

					answer = !dangerUnknown && !dangerTooHigh;
				}
			}

			const int answerSelection = answer ? 1 : 0;
			if(nativeTrace)
			{
				nativeTrace->recordDecision(
					boost::str(boost::format("showBlockingDialog.%d") % askID.getNum()),
					"showBlockingDialog",
					std::move(traceInput),
					blockingDialogNativeOutput(askID, answerSelection),
					{ "status", "selection", "commandJournal" });
			}
			answerQuery(askID, answerSelection);
		});

		return;
	}

	executeActionAsync("showBlockingDialog", [this, selection, components, heroPtr, askID, text, soundID, cancel, safeToAutoaccept]()
	{
		int sel = 0;
		JsonNode traceInput = blockingDialogInput(askID, text, components, soundID, selection, cancel, safeToAutoaccept);

		if(selection) // select the last component; they are indexed in range [1, size]
			sel = components.size();
		{
				std::unique_lock mxLock(nullkiller->aiStateMutex);
				std::optional<HeroRole> role;
				bool goldPressureOverMax = false;
				if(heroPtr.isVerified())
				{
					role = nullkiller->heroManager->getHeroRoleOrDefault(heroPtr);
					goldPressureOverMax = nullkiller->buildAnalyzer->isGoldPressureOverMax();
					traceInput["hero"] = blockingHeroSnapshot(heroPtr, role);
					traceInput["goldPressureOverMax"].Bool() = goldPressureOverMax;
				}
				const bool preferResourceReward = role && (*role != HeroRole::MAIN || goldPressureOverMax);

				// TODO: Find better way to understand it is Chest of Treasures
				if(heroPtr.isVerified()
					&& components.size() == 2
					&& components.front().type == ComponentType::RESOURCE
					&& preferResourceReward)
				{
					sel = 1;
				}
		}

		if(nativeTrace)
		{
			nativeTrace->recordDecision(
				boost::str(boost::format("showBlockingDialog.%d") % askID.getNum()),
				"showBlockingDialog",
				std::move(traceInput),
				blockingDialogNativeOutput(askID, sel),
				{ "status", "selection", "commandJournal" });
		}
		answerQuery(askID, sel);
	});
}

void AIGateway::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	status.addQuery(askID, boost::str(boost::format("Teleport dialog query with %d exits") % exits.size()));

	JsonNode traceInput = teleportDialogInput(
		askID,
		channel,
		exits,
		impassable,
		destinationTeleport,
		destinationTeleportPos,
		status.channelProbing(),
		teleportChannelProbingList,
		cc);
	int chosenExit = -1;
	if(impassable)
	{
		nullkiller->memory->knownTeleportChannels[channel]->passability = TeleportChannel::IMPASSABLE;
	}
	else if(destinationTeleport != ObjectInstanceID() && destinationTeleportPos.isValid())
	{
		auto neededExit = std::make_pair(destinationTeleport, destinationTeleportPos);
		if(destinationTeleport != ObjectInstanceID() && vstd::contains(exits, neededExit))
			chosenExit = vstd::find_pos(exits, neededExit);
	}

	for(auto exit : exits)
	{
		if(status.channelProbing() && exit.first == destinationTeleport)
		{
			chosenExit = vstd::find_pos(exits, exit);
			break;
		}
		else
		{
			// TODO: Implement checking if visiting that teleport will uncovert any FoW
			// So far this is the best option to handle decision about probing
			auto obj = cc->getObj(exit.first, false);
			if(obj == nullptr && !vstd::contains(teleportChannelProbingList, exit.first))
			{
				if(exit.first != destinationTeleport)
					teleportChannelProbingList.push_back(exit.first);
			}
		}
	}

	executeActionAsync("showTeleportDialog", [this, askID, chosenExit, traceInput = std::move(traceInput)]() mutable
	{
		if(nativeTrace)
		{
			nativeTrace->recordDecision(
				boost::str(boost::format("showTeleportDialog.%d") % askID.getNum()),
				"showTeleportDialog",
				std::move(traceInput),
				answerQueryNativeOutput(askID, chosenExit),
				{ "status", "selection", "commandJournal" });
		}
		answerQuery(askID, chosenExit);
	});
}

void AIGateway::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	LOG_TRACE_PARAMS(logAi, "removableUnits '%i', queryID '%i'", removableUnits % queryID);
	std::string s1 = up->nodeName();
	std::string s2 = down->nodeName();

	status.addQuery(queryID, boost::str(boost::format("Garrison dialog with %s and %s") % s1 % s2));

	//you can't request action from action-response thread
	executeActionAsync("showGarrisonDialog", [this, up, down, removableUnits, queryID]()
	{
		if(removableUnits && up->tempOwner == down->tempOwner && nullkiller->settings->isGarrisonTroopsUsageAllowed() && !cc->getStartInfo()->restrictedGarrisonsForAI())
		{
			pickBestCreatures(down, up);
		}

		answerQuery(queryID, 0);
	});
}

void AIGateway::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	status.addQuery(askID, "Map object select query");
	executeActionAsync("showMapObjectSelectDialog", [this, askID, objects]()
	{
		const int selection = selectedObject.getNum();
		if(nativeTrace)
		{
			nativeTrace->recordDecision(
				boost::str(boost::format("showMapObjectSelectDialog.%d") % askID.getNum()),
				"showMapObjectSelectDialog",
				mapObjectSelectInput(askID, selection, objects),
				mapObjectSelectNativeOutput(askID, selection),
				{ "status", "selection", "commandJournal" });
		}
		answerQuery(askID, selection);
	});
}

bool AIGateway::makePossibleUpgrades(const CArmedInstance * obj)
{
	if(!obj)
		return false;

	bool upgraded = false;

	for(int i = 0; i < GameConstants::ARMY_SIZE; i++)
	{
		if(const CStackInstance * s = obj->getStackPtr(SlotID(i)))
		{
			UpgradeInfo upgradeInfo(s->getId());
			do
			{
				cc->fillUpgradeInfo(obj, SlotID(i), upgradeInfo);

				if(upgradeInfo.hasUpgrades())
				{
					// creature at given slot might have alternative upgrades, pick best one
					CreatureID upgID = *vstd::maxElementByFun(upgradeInfo.getAvailableUpgrades(), [](const CreatureID & id)
						{
							return id.toCreature()->getAIValue();
						});

					int oldValue = s->getCreature()->getAIValue();
					int newValue = upgID.toCreature()->getAIValue();

					if(newValue > oldValue && nullkiller->getFreeResources().canAfford(upgradeInfo.getUpgradeCostsFor(upgID) * s->getCount()))
					{
						cc->upgradeCreature(obj, SlotID(i), upgID);
						upgraded = true;
						logAi->debug("Upgraded %d %s to %s", s->getCount(), upgradeInfo.oldID.toCreature()->getNamePluralTranslated(),
							upgradeInfo.getUpgrade().toCreature()->getNamePluralTranslated());
					}
					else
						break;
				}
			}
			while(upgradeInfo.hasUpgrades());
		}
	}

	return upgraded;
}

void AIGateway::makeTurn()
{
	try
	{
		auto day = cc->getCalendar().getCurrentDay();
		logAi->info("Player %d (%s) starting turn, day %d", playerID, playerID.toString(), day);

		std::shared_lock gsLock(CGameState::mutex);
		cheatMapReveal(nullkiller);
		memorizeVisitableObjs(nullkiller->memory, nullkiller->dangerHitMap, playerID, cc);
		memorizeRevisitableObjs(nullkiller->memory, playerID, cc);

		const auto start = std::chrono::high_resolution_clock::now();
		nullkiller->makeTurn();
		const auto timeElapsedMs = timeElapsed(start);
		if(timeElapsedMs > 5000)
			logAi->warn("PERFORMANCE: NK2 makeTurn took %ld ms", timeElapsedMs);
		else
			logAi->info("PERFORMANCE: NK2 makeTurn took %ld ms", timeElapsedMs);

		for (const auto *h : cc->getHeroesInfo())
		{
			if (h->movementPointsRemaining())
				logAi->warn("Hero %s has %d MP left", h->getNameTranslated(), h->movementPointsRemaining());
		}

		endTurn();
	}
	catch (const InterruptionRequestedException &)
	{
		logAi->debug("Making turn thread has been interrupted. We'll end without calling endTurn.");
	}
	catch (const TerminationRequestedException &)
	{
		logAi->debug("Making turn thread has been terminated. We'll end without calling endTurn");
	}
}

void AIGateway::performObjectInteraction(const CGObjectInstance * obj, HeroPtr heroPtr)
{
	LOG_TRACE_PARAMS(logAi, "Hero %s and object %s at %s", heroPtr->getNameTranslated() % obj->getObjectName() % obj->anchorPos().toString());
	switch(obj->ID)
	{
	case Obj::TOWN:
		if(heroPtr->getVisitedTown()) //we are inside, not just attacking
		{
			makePossibleUpgrades(heroPtr.get());

			std::unique_lock lockGuard(nullkiller->aiStateMutex);

			if(!heroPtr->getVisitedTown()->getGarrisonHero() || !nullkiller->isHeroLocked(heroPtr->getVisitedTown()->getGarrisonHero()))
				moveCreaturesToHero(heroPtr->getVisitedTown());

			if(nullkiller->heroManager->getHeroRoleOrDefault(heroPtr) == HeroRole::MAIN && !heroPtr->hasSpellbook()
				&& nullkiller->getFreeGold() >= GameConstants::SPELLBOOK_GOLD_COST)
			{
				if(heroPtr->getVisitedTown()->hasBuilt(BuildingID::MAGES_GUILD_1))
					cc->buyArtifact(heroPtr.get(), ArtifactID::SPELLBOOK);
			}
		}
		break;
	case Obj::HILL_FORT:
		makePossibleUpgrades(heroPtr.get());
		break;
	}
}

void AIGateway::moveCreaturesToHero(const CGTownInstance * t)
{
	if(t->getVisitingHero() && t->armedGarrison() && t->getVisitingHero()->tempOwner == t->tempOwner)
	{
		pickBestCreatures(t->getVisitingHero(), t->getUpperArmy());
	}
}

void AIGateway::pickBestCreatures(const CArmedInstance * destinationArmy, const CArmedInstance * source)
{
	if(source->stacksCount() == 0)
		return;

	const CArmedInstance * armies[] = {destinationArmy, source};

	auto bestArmy = nullkiller->armyManager->getBestArmy(destinationArmy, destinationArmy, source, cc->getTile(source->visitablePos())->getTerrainID());

	for(auto army : armies)
	{
		// move first stack at first slot if empty to avoid can not take away last creature
		if(!army->hasStackAtSlot(SlotID(0)) && army->stacksCount() > 0)
		{
			cc->mergeOrSwapStacks(
				army,
				army,
				SlotID(0),
				army->Slots().begin()->first);
		}
	}

	//foreach best type -> iterate over slots in both armies and if it's the appropriate type, send it to the slot where it belongs
	for(SlotID i = SlotID(0); i.validSlot(); i.advance(1)) //i-th strongest creature type will go to i-th slot
	{
		if(i.getNum() >= bestArmy.size())
		{
			if(destinationArmy->hasStackAtSlot(i))
			{
				auto creature = destinationArmy->getCreature(i);
				auto targetSlot = source->getSlotFor(creature);

				if(targetSlot.validSlot())
				{
					// remove unwanted creatures
					cc->mergeOrSwapStacks(destinationArmy, source, i, targetSlot);
				}
				else if(destinationArmy->getStack(i).getPower() < destinationArmy->getArmyStrength() / 100)
				{
					// dismiss creatures if the amount is small
					cc->dismissCreature(destinationArmy, i);
				}
			}

			continue;
		}

		const CCreature * targetCreature = bestArmy[i.getNum()].creature;

		for(auto armyPtr : armies)
		{
			for(SlotID j = SlotID(0); j.validSlot(); j.advance(1))
			{
				if(armyPtr->getCreature(j) == targetCreature && (i != j || armyPtr != destinationArmy)) //it's a searched creature not in dst SLOT
				{
					//can't take away last creature without split. generate a new stack with 1 creature which is weak but fast
					if(armyPtr == source
						&& source->needsLastStack()
						&& source->stacksCount() == 1
						&& (!destinationArmy->hasStackAtSlot(i) || destinationArmy->getCreature(i) == targetCreature))
					{
						auto weakest = nullkiller->armyManager->getBestUnitForScout(bestArmy, cc->getTile(source->visitablePos())->getTerrainID());

						if(weakest->creature == targetCreature)
						{
							if(1 == source->getStackCount(j))
								break;

							// move all except 1 of weakest creature from source to destination
							cc->splitStack(
								source,
								destinationArmy,
								j,
								destinationArmy->getSlotFor(targetCreature),
								destinationArmy->getStackCount(i) + source->getStackCount(j) - 1);

							break;
						}
						else
						{
							// Source last stack is not weakest. Move 1 of weakest creature from destination to source
							cc->splitStack(
								destinationArmy,
								source,
								destinationArmy->getSlotFor(weakest->creature),
								source->getFreeSlot(),
								1);
						}
					}

					cc->mergeOrSwapStacks(armyPtr, destinationArmy, j, i);
				}
			}
		}
	}

	//TODO - having now strongest possible army, we may want to think about arranging stacks
}

void AIGateway::recruitCreatures(const CGDwelling * d, const CArmedInstance * recruiter)
{
	//now used only for visited dwellings / towns, not BuyArmy goal
	for(int i = 0; i < d->creatures.size(); i++)
	{
		if(!d->creatures[i].second.size())
			continue;

		int count = d->creatures[i].first;
		CreatureID creID = d->creatures[i].second.back();

		if(!recruiter->getSlotFor(creID).validSlot())
		{
			for(const auto & stack : recruiter->Slots())
			{
				if(!stack.second->getType())
					continue;

				auto duplicatingSlot = recruiter->getSlotFor(stack.second->getCreature());

				if(duplicatingSlot != stack.first)
				{
					cc->mergeStacks(recruiter, recruiter, stack.first, duplicatingSlot);
					break;
				}
			}

			if(!recruiter->getSlotFor(creID).validSlot())
			{
				continue;
			}
		}

		vstd::amin(count, cc->getResourceAmount() / creID.toCreature()->getFullRecruitCost());
		if(count > 0)
			cc->recruitCreatures(d, recruiter, creID, count, i);
	}
}

void AIGateway::battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed)
{
	assert(!playerID.isValidPlayer() || status.getBattle() == UPCOMING_BATTLE);
	status.setBattle(ONGOING_BATTLE);
	const CGObjectInstance * presumedEnemy = vstd::backOrNull(cc->getVisitableObjs(tile)); //may be nullptr in some very are cases -> eg. visited monolith and fighting with an enemy at the FoW covered exit
	battlename = boost::str(boost::format("Starting battle of %s attacking %s at %s") % (hero1 ? hero1->getNameTranslated() : "a army") % (presumedEnemy ? presumedEnemy->getObjectName() : "unknown enemy") % tile.toString());
	CAdventureAI::battleStart(battleID, army1, army2, tile, hero1, hero2, side, replayAllowed);
}

void AIGateway::battleEnd(const BattleID & battleID, const BattleResult * br, QueryID queryID)
{
	assert(status.getBattle() == ONGOING_BATTLE);
	status.setBattle(ENDING_BATTLE);
	bool won = br->winner == cc->getBattle(battleID)->battleGetMySide();
	logAi->debug("Player %d (%s): I %s the %s!", playerID, playerID.toString(), (won ? "won" : "lost"), battlename);
	battlename.clear();

	CAdventureAI::battleEnd(battleID, br, queryID);

	// gosolo
	if(queryID != QueryID::NONE && cc->getPlayerState(playerID)->isHuman())
	{
		status.addQuery(queryID, "Confirm battle query");

		executeActionAsync("battleEnd", [this, queryID]()
		{
			answerQuery(queryID, 0);
		});
	}
}

void AIGateway::waitTillFree()
{
	auto unlock = vstd::makeUnlockSharedGuard(CGameState::mutex);
	status.waitTillFree();
}

std::vector<const CGObjectInstance *> AIGateway::getFlaggedObjects() const
{
	std::vector<const CGObjectInstance *> ret;
	for(const ObjectInstanceID objId : nullkiller->memory->visitableObjs)
	{
		const CGObjectInstance * obj = cc->getObjInstance(objId);
		if(obj && obj->tempOwner == playerID)
			ret.push_back(obj);
	}
	return ret;
}

bool AIGateway::moveHeroToTile(const int3 dst, const HeroPtr & heroPtr)
{
	if(heroPtr->isGarrisoned() && heroPtr->getVisitedTown())
	{
		cc->swapGarrisonHero(heroPtr->getVisitedTown());
		moveCreaturesToHero(heroPtr->getVisitedTown());
	}

	//TODO: consider if blockVisit objects change something in our checks: AIUtility::isBlockVisitObj()

	auto afterMovementCheck = [&]() -> void
	{
		waitTillFree(); //movement may cause battle or blocking dialog
		if(!heroPtr.isVerified())
		{
			lostHero(heroPtr);
			teleportChannelProbingList.clear();
			if(status.channelProbing()) // if hero lost during channel probing we need to switch this mode off
				status.setChannelProbing(false);
			throw cannotFulfillGoalException("Hero was lost!");
		}
	};

	logAi->debug("Moving hero %s to tile %s", heroPtr->getNameTranslated(), dst.toString());
	int3 startHpos = heroPtr->visitablePos();
	bool ret = false;
	if(startHpos == dst)
	{
		//FIXME: this assertion fails also if AI moves onto defeated guarded object
		//assert(cb->getVisitableObjs(dst).size() > 1); //there's no point in revisiting tile where there is no visitable object
		cc->moveHero(*heroPtr, heroPtr->convertFromVisitablePos(dst), false);
		afterMovementCheck(); // TODO: is it feasible to hero get killed there if game work properly?
		// If revisiting, teleport probing is never done, and so the entries into the list would remain unused and uncleared
		teleportChannelProbingList.clear();
		// not sure if AI can currently reconsider to attack bank while staying on it. Check issue 2084 on mantis for more information.
		ret = true;
	}
	else
	{
		CGPath path;
		nullkiller->getPathsInfo(heroPtr.get())->getPath(path, dst);
		if(path.nodes.empty())
		{
			logAi->error("Hero %s cannot reach %s.", heroPtr->getNameTranslated(), dst.toString());
			return true;
		}
		int i = (int)path.nodes.size() - 1;

		auto getObj = [&](int3 coord, bool ignoreHero)
		{
			auto tile = cc->getTile(coord, false);
			assert(tile);
			return cc->getObj(tile->topVisitableObj(ignoreHero), false);
		};

		auto isTeleportAction = [&](EPathNodeAction action) -> bool
		{
			if(action != EPathNodeAction::TELEPORT_NORMAL && action != EPathNodeAction::TELEPORT_BLOCKING_VISIT)
			{
				if(action != EPathNodeAction::TELEPORT_BATTLE)
				{
					return false;
				}
			}

			return true;
		};

		auto getDestTeleportObj = [&](const CGObjectInstance * currentObject, const CGObjectInstance * nextObjectTop, const CGObjectInstance * nextObject) -> const CGObjectInstance *
		{
			if(CGTeleport::isConnected(currentObject, nextObjectTop))
				return nextObjectTop;
			if(nextObjectTop && nextObjectTop->ID == Obj::HERO)
			{
				if(CGTeleport::isConnected(currentObject, nextObject))
					return nextObject;
			}

			return nullptr;
		};

		auto doMovement = [&](int3 dst, bool transit)
		{
			cc->moveHero(*heroPtr, heroPtr->convertFromVisitablePos(dst), transit);
		};

		auto doTeleportMovement = [&](ObjectInstanceID exitId, int3 exitPos)
		{
			if(cc->getObj(exitId) && cc->getObj(exitId)->ID == Obj::WHIRLPOOL)
			{
				nullkiller->armyFormation->rearrangeArmyForWhirlpool(*heroPtr);
			}

			destinationTeleport = exitId;
			if(exitPos.isValid())
				destinationTeleportPos = exitPos;
			cc->moveHero(*heroPtr, heroPtr->pos, false);
			destinationTeleport = ObjectInstanceID();
			destinationTeleportPos = int3(-1);
			afterMovementCheck();
		};

		auto doChannelProbing = [&]() -> void
		{
			auto currentPos = heroPtr->visitablePos();
			auto currentTeleport = getObj(currentPos, true);

			if(currentTeleport)
			{
				auto currentExit = currentTeleport->id;

				status.setChannelProbing(true);
				for(auto exit : teleportChannelProbingList)
					doTeleportMovement(exit, int3(-1));

				teleportChannelProbingList.clear();
				status.setChannelProbing(false);

				doTeleportMovement(currentExit, currentPos);
			}
			else
			{
				logAi->debug("Unexpected channel probbing at " + currentPos.toString());

				teleportChannelProbingList.clear();
				status.setChannelProbing(false);
			}
		};

		teleportChannelProbingList.clear();
		status.setChannelProbing(false);

		for(; i > 0; i--)
		{
			int3 currentCoord = path.nodes[i].coord;
			int3 nextCoord = path.nodes[i - 1].coord;

			auto currentObject = getObj(currentCoord, currentCoord == heroPtr->visitablePos());
			auto nextObjectTop = getObj(nextCoord, false);
			auto nextObject = getObj(nextCoord, true);
			auto destTeleportObj = getDestTeleportObj(currentObject, nextObjectTop, nextObject);
			if(isTeleportAction(path.nodes[i - 1].action) && destTeleportObj != nullptr)
			{
				//we use special login if hero standing on teleporter it's mean we need
				doTeleportMovement(destTeleportObj->id, nextCoord);
				if(teleportChannelProbingList.size())
					doChannelProbing();
				nullkiller->memory->markObjectVisited(destTeleportObj); //FIXME: Monoliths are not correctly visited

				continue;
			}

			//stop sending move requests if the next node can't be reached at the current turn (hero exhausted his move points)
			if(path.nodes[i - 1].turns)
			{
				//blockedHeroes.insert(h); //to avoid attempts of moving heroes with very little MPs
				return false;
			}

			int3 endpos = path.nodes[i - 1].coord;
			if(endpos == heroPtr->visitablePos())
				continue;

			bool isConnected = false;
			bool isNextObjectTeleport = false;
			// Check there is node after next one; otherwise transit is pointless
			if(i - 2 >= 0)
			{
				isConnected = CGTeleport::isConnected(nextObjectTop, getObj(path.nodes[i - 2].coord, false));
				isNextObjectTeleport = CGTeleport::isTeleport(nextObjectTop);
			}
			if(isConnected || isNextObjectTeleport)
			{
				// Hero should be able to go through object if it's allow transit
				doMovement(endpos, true);
			}
			else if(path.nodes[i - 1].layer == EPathfindingLayer::AIR)
			{
				doMovement(endpos, true);
			}
			else
			{
				doMovement(endpos, false);
			}

			afterMovementCheck();

			if(teleportChannelProbingList.size())
				doChannelProbing();
		}

		if(path.nodes[0].action == EPathNodeAction::BLOCKING_VISIT || path.nodes[0].action == EPathNodeAction::BATTLE)
		{
			// when we take resource we do not reach its position. We even might not move
			// also guarded town is not get visited automatically after capturing
			ret = heroPtr.isVerified() && i == 0;
		}
	}
	if(heroPtr.isVerified())
	{
		if(auto visitedObject = vstd::frontOrNull(cc->getVisitableObjs(heroPtr->visitablePos()))) //we stand on something interesting
		{
			if(visitedObject != *heroPtr)
			{
				performObjectInteraction(visitedObject, heroPtr);
				ret = true;
			}
		}
	}
	if(heroPtr.isVerified()) //we could have lost hero after last move
	{
		ret = ret || (dst == heroPtr->visitablePos());

		if(startHpos == heroPtr->visitablePos() && !ret) //we didn't move and didn't reach the target
		{
			throw cannotFulfillGoalException("Invalid path found!");
		}

		logAi->debug("Hero %s moved from %s to %s. Returning %d.", heroPtr->getNameTranslated(), startHpos.toString(), heroPtr->visitablePos().toString(), ret);
	}
	return ret;
}

void AIGateway::buildStructure(const CGTownInstance * t, BuildingID building)
{
	auto name = t->getTown()->buildings.at(building)->getNameTranslated();
	logAi->debug("Player %d will build %s in town of %s at %s", playerID, name, t->getNameTranslated(), t->anchorPos().toString());
	cc->buildBuilding(t, building); //just do this;
}

void AIGateway::tryRealize(Goals::DigAtTile & g)
{
	assert(g.hero->visitablePos() == g.tile); //surely we want to crash here?
	if(g.hero->diggingStatus() == EDiggingStatus::CAN_DIG)
	{
		cc->dig(g.hero);
	}
	else
	{
		throw cannotFulfillGoalException("A hero can't dig!\n");
	}
}

void AIGateway::tryRealize(Goals::Trade & g) //trade
{
	if(cc->getResourceAmount(GameResID(g.resID)) >= g.value) //goal is already fulfilled. Why we need this check, anyway?
		throw goalFulfilledException(sptr(g));

	int acquiredResources = 0;
	if(const CGObjectInstance * obj = cc->getObj(ObjectInstanceID(g.objid), false))
	{
		if(const auto * m = dynamic_cast<const IMarket*>(obj))
		{
			auto freeRes = cc->getResourceAmount(); //trade only resources which are not reserved
			for(auto it = ResourceSet::nziterator(freeRes); it.valid(); it++)
			{
				auto res = it->resType;
				if(res.getNum() == g.resID) //sell any other resource
					continue;

				int toGive;
				int toGet;
				m->getOffer(res.getNum(), g.resID, toGive, toGet, EMarketMode::RESOURCE_RESOURCE);
				toGive = static_cast<int>(toGive * (it->resVal / toGive)); //round down
				//TODO trade only as much as needed
				if (toGive) //don't try to sell 0 resources
				{
					cc->trade(m->getObjInstanceID(), EMarketMode::RESOURCE_RESOURCE, res, GameResID(g.resID), toGive);
					acquiredResources = static_cast<int>(toGet * (it->resVal / toGive));
					logAi->debug("Traded %d of %s for %d of %s at %s", toGive, res, acquiredResources, g.resID, obj->getObjectName());
				}
				if (cc->getResourceAmount(GameResID(g.resID)))
					throw goalFulfilledException(sptr(g)); //we traded all we needed
			}

			throw cannotFulfillGoalException("I cannot get needed resources by trade!");
		}
		else
		{
			throw cannotFulfillGoalException("I don't know how to use this object to raise resources!");
		}
	}
	else
	{
		throw cannotFulfillGoalException("No object that could be used to raise resources!");
	}
}

void AIGateway::endTurn()
{
	logAi->info("Player %d (%s) ends turn", playerID, playerID.toString());
	if(!status.haveTurn())
	{
		logAi->error("Not having turn at the end of turn???");
	}

	logAi->debug("Resources at the end of turn: %s", cc->getResourceAmount().toString());

	if(cc->getPlayerStatus(playerID) != EPlayerStatus::INGAME)
	{
		logAi->info("Ending turn is not needed because we already lost");
		return;
	}

	do
	{
		cc->endTurn();
	}
	while(status.haveTurn()); //for some reasons, our request may fail -> stop requesting end of turn only after we've received a confirmation that it's over

	logGlobal->info("Player %d (%s) ended turn", playerID, playerID.toString());
}

void AIGateway::buildArmyIn(const CGTownInstance * t)
{
	makePossibleUpgrades(t->getVisitingHero());
	makePossibleUpgrades(t);
	recruitCreatures(t, t->getUpperArmy());
	moveCreaturesToHero(t);
}

void AIGateway::finish()
{
	nullkiller->makingTurnInterruption.interruptThread();

	if (asyncTasks)
	{
		asyncTasks->wait();
		asyncTasks.reset();
	}
}

void AIGateway::executeActionAsync(const std::string & description, const std::function<void()> & whatToDo)
{
	if (!asyncTasks)
		throw std::runtime_error("Attempt to execute task on shut down AI state!");

	asyncTasks->run([description, whatToDo]() noexcept
	{
		ScopedThreadName guard("NK2AI::AIGateway::" + description);
		std::shared_lock gsLock(CGameState::mutex);
		try
		{
			whatToDo();
		}
		catch (const TerminationRequestedException &)
		{
			logAi->debug("%s thread has been terminated. We'll end it immediately", description);
		}
	});
}

void AIGateway::lostHero(const HeroPtr & heroPtr) const
{
	logAi->debug("I lost my hero %s. It's best to forget and move on.", heroPtr.nameOrDefault());
	nullkiller->invalidatePathfinderData();
}

void AIGateway::answerQuery(const QueryID queryID, const int selection) const
{
	logAi->debug("I'll answer the query %d giving the choice %d", queryID, selection);
	if(queryID != QueryID(-1))
	{
		cc->selectionMade(selection, queryID);
	}
	else
	{
		logAi->debug("Since the query ID is %d, the answer won't be sent. This is not a real query!", queryID);
		//do nothing
	}
}

void AIGateway::requestSent(const CPackForServer * pack, int requestID)
{
	//BNLOG("I have sent request of type %s", typeid(*pack).name());
	if(auto reply = dynamic_cast<const QueryReply *>(pack))
	{
		status.attemptedAnsweringQuery(reply->qid, requestID);
	}
}

std::string AIGateway::getBattleAIName() const
{
	if(settings["ai"]["combatEnemyAI"].getType() == JsonNode::JsonType::DATA_STRING)
		return settings["ai"]["combatEnemyAI"].String();
	else
		return "BattleAI";
}

void AIGateway::validateObject(const CGObjectInstance * obj)
{
	validateObject(ObjectIdRef(obj->id, cc.get()));
}

void AIGateway::validateObject(ObjectIdRef obj)
{
	if(!obj)
	{
		nullkiller->memory->removeFromMemory(obj);
	}
}

AIStatus::AIStatus(AIGateway * aiGw)
	: aiGw(aiGw)
{
	battle = NO_BATTLE;
	havingTurn = false;
	ongoingHeroMovement = false;
	ongoingChannelProbing = false;
}

AIStatus::~AIStatus()
{

}

void AIStatus::setBattle(BattleState BS)
{
	std::unique_lock<std::mutex> lock(mx);
	LOG_TRACE_PARAMS(logAi, "battle state=%d", (int)BS);
	battle = BS;
	cv.notify_all();
}

BattleState AIStatus::getBattle()
{
	std::unique_lock<std::mutex> lock(mx);
	return battle;
}

void AIStatus::addQuery(QueryID ID, std::string description)
{
	if(ID == QueryID(-1))
	{
		logAi->debug("The \"query\" has an id %d, it'll be ignored as non-query. Description: %s", ID, description);
		return;
	}

	assert(ID.getNum() >= 0);
	std::unique_lock<std::mutex> lock(mx);

	assert(!vstd::contains(remainingQueries, ID));

	remainingQueries[ID] = description;

	cv.notify_all();
	logAi->debug("Adding query %d - %s. Total queries count: %d", ID, description, remainingQueries.size());
}

void AIStatus::removeQuery(QueryID ID)
{
	std::unique_lock<std::mutex> lock(mx);
	assert(vstd::contains(remainingQueries, ID));

	std::string description = remainingQueries[ID];
	remainingQueries.erase(ID);

	cv.notify_all();
	logAi->debug("Removing query %d - %s. Total queries count: %d", ID, description, remainingQueries.size());
}

int AIStatus::getQueriesCount()
{
	std::unique_lock<std::mutex> lock(mx);
	return static_cast<int>(remainingQueries.size());
}

void AIStatus::startedTurn()
{
	std::unique_lock<std::mutex> lock(mx);
	havingTurn = true;
	cv.notify_all();
}

void AIStatus::madeTurn()
{
	std::unique_lock<std::mutex> lock(mx);
	havingTurn = false;
	cv.notify_all();
}

void AIStatus::waitTillFree()
{
	std::unique_lock<std::mutex> lock(mx);
	while(battle != NO_BATTLE || !remainingQueries.empty() || !objectsBeingVisited.empty() || ongoingHeroMovement)
	{
		cv.wait_for(lock, std::chrono::milliseconds(10));
		aiGw->nullkiller->makingTurnInterruption.interruptionPoint();
	}
}

bool AIStatus::haveTurn()
{
	std::unique_lock<std::mutex> lock(mx);
	return havingTurn;
}

void AIStatus::attemptedAnsweringQuery(QueryID queryID, int answerRequestID)
{
	std::unique_lock<std::mutex> lock(mx);
	assert(vstd::contains(remainingQueries, queryID));
	std::string description = remainingQueries[queryID];
	logAi->debug("Attempted answering query %d - %s. Request id=%d. Waiting for results...", queryID, description, answerRequestID);
	requestToQueryID[answerRequestID] = queryID;
}

void AIStatus::receivedAnswerConfirmation(int answerRequestID, int result)
{
	assert(vstd::contains(requestToQueryID, answerRequestID));
	QueryID query = requestToQueryID[answerRequestID];
	assert(vstd::contains(remainingQueries, query));
	requestToQueryID.erase(answerRequestID);

	if(result)
	{
		removeQuery(query);
	}
	else
	{
		logAi->error("Something went really wrong, failed to answer query %d : %s", query.getNum(), remainingQueries[query]);
		//TODO safely retry
	}
}

void AIStatus::heroVisit(const CGObjectInstance * obj, bool started)
{
	std::unique_lock<std::mutex> lock(mx);
	if(started)
	{
		objectsBeingVisited.push_back(obj);
	}
	else
	{
		// There can be more than one object visited at the time (eg. hero visits Subterranean Gate
		// causing visit to hero on the other side.
		// However, we are guaranteed that start/end visit notification maintain stack order.
		assert(!objectsBeingVisited.empty());
		objectsBeingVisited.pop_back();
	}
	cv.notify_all();
}

void AIStatus::setMove(bool ongoing)
{
	std::unique_lock<std::mutex> lock(mx);
	ongoingHeroMovement = ongoing;
	cv.notify_all();
}

void AIStatus::setChannelProbing(bool ongoing)
{
	std::unique_lock<std::mutex> lock(mx);
	ongoingChannelProbing = ongoing;
	cv.notify_all();
}

bool AIStatus::channelProbing()
{
	return ongoingChannelProbing;
}

void AIGateway::invalidatePaths()
{
	nullkiller->invalidatePaths();
}

/*
 * ////////////////////////////////////
 * ////////// STATIC Methods //////////
 * ////////////////////////////////////
 */

void AIGateway::cheatMapReveal(const std::unique_ptr<Nullkiller> & nullkiller)
{
	if(nullkiller->cc->getCalendar().getCurrentDay() == 1) // No need to execute every day, only the first time
	{
		if(nullkiller->isOpenMap())
		{
			nullkiller->cc->sendMessage("vcmieagles");
		}
	}
}

void AIGateway::memorizeVisitableObjs(const std::unique_ptr<AIMemory> & memory,
                                      const std::unique_ptr<DangerHitMapAnalyzer> & dangerHitMap,
                                      const PlayerColor & playerID,
                                      const std::shared_ptr<CCallback> & cc)
{
	foreach_tile_pos(*cc, [&](const int3 & pos)
	{
		// TODO: Inspect what not visible means when using verbose true
		for(const CGObjectInstance * obj : cc->getVisitableObjs(pos, false))
		{
			memorizeVisitableObj(obj, memory, dangerHitMap, playerID, cc);
		}
	});
}

void AIGateway::memorizeVisitableObj(const CGObjectInstance * obj,
                                     const std::unique_ptr<AIMemory> & memory,
                                     const std::unique_ptr<DangerHitMapAnalyzer> & dangerHitMap,
                                     const PlayerColor & playerID,
                                     const std::shared_ptr<CCallback> & cc)
{
	if(obj->ID == Obj::EVENT)
		return;

	memory->addVisitableObject(obj);

	if(obj->ID == Obj::HERO && cc->getPlayerRelations(obj->tempOwner, playerID) == PlayerRelations::ENEMIES)
	{
		dangerHitMap->resetHitmap();
	}
}

void AIGateway::memorizeRevisitableObjs(const std::unique_ptr<AIMemory> & memory, const PlayerColor & playerID, const std::shared_ptr<CCallback> & cc)
{
	if(cc->getCalendar().getDayOfWeek() == 1)
	{
		for(const ObjectInstanceID objId : memory->visitableObjs)
		{
			const CGObjectInstance * obj = cc->getObjInstance(objId);
			if(obj && isWeeklyRevisitable(playerID, obj))
				memory->markObjectUnvisited(obj);
		}
	}
}

void AIGateway::pickBestArtifacts(const std::shared_ptr<CCallback> & cc, const CGHeroInstance * h, const CGHeroInstance * other)
{
	auto equipBest = [cc](const CGHeroInstance * h, const CGHeroInstance * otherh, bool giveStuffToFirstHero) -> void
	{
		bool changeMade = false;
		std::set<std::pair<ArtifactInstanceID, ArtifactInstanceID>> swappedSet;
		do
		{
			changeMade = false;

			//we collect gear always in same order
			std::vector<ArtifactLocation> allArtifacts;
			if(giveStuffToFirstHero)
			{
				for(auto p : h->artifactsWorn)
				{
					if(p.second.getArt())
						allArtifacts.push_back(ArtifactLocation(h->id, p.first));
				}
			}
			for(auto slot : h->artifactsInBackpack)
				allArtifacts.push_back(ArtifactLocation(h->id, h->getArtPos(slot.getArt())));

			if(otherh)
			{
				for(auto p : otherh->artifactsWorn)
				{
					if(p.second.getArt())
						allArtifacts.push_back(ArtifactLocation(otherh->id, p.first));
				}
				for(auto slot : otherh->artifactsInBackpack)
					allArtifacts.push_back(ArtifactLocation(otherh->id, otherh->getArtPos(slot.getArt())));
			}
			//we give stuff to one hero or another, depending on giveStuffToFirstHero

			const CGHeroInstance * target = nullptr;
			if(giveStuffToFirstHero || !otherh)
				target = h;
			else
				target = otherh;

			for(auto location : allArtifacts)
			{
				if(location.artHolder == target->id && ArtifactUtils::isSlotEquipment(location.slot))
					continue; //don't reequip artifact we already wear

				if(location.slot == ArtifactPosition::MACH4) // don't attempt to move catapult
					continue;

				auto artHolder = cc->getHero(location.artHolder);
				auto s = artHolder->getSlot(location.slot);
				if(!s || s->locked) //we can't move locks
					continue;
				auto artifact = s->getArt();
				if(!artifact)
					continue;
				//FIXME: why are the above possible to be null?

				bool emptySlotFound = false;
				for(auto slot : artifact->getType()->getPossibleSlots().at(target->bearerType()))
				{
					if(target->isPositionFree(slot) && artifact->canBePutAt(target, slot, true)) //combined artifacts are not always allowed to move
					{
						ArtifactLocation destLocation(target->id, slot);
						cc->swapArtifacts(location, destLocation); //just put into empty slot
						emptySlotFound = true;
						changeMade = true;
						break;
					}
				}
				if(!emptySlotFound) //try to put that atifact in already occupied slot
				{
					int64_t artifactScore = getArtifactScoreForHero(target, artifact);

					for(auto slot : artifact->getType()->getPossibleSlots().at(target->bearerType()))
					{
						auto otherSlot = target->getSlot(slot);
						if(otherSlot && otherSlot->getArt()) //we need to exchange artifact for better one
						{
							int64_t otherArtifactScore = getArtifactScoreForHero(target, otherSlot->getArt());
							logAi->trace(
								"Comparing artifacts of %s: %s vs %s. Score: %d vs %d",
								target->getHeroTypeName(),
								artifact->getType()->getJsonKey(),
								otherSlot->getArt()->getType()->getJsonKey(),
								artifactScore,
								otherArtifactScore
							);

							//if that artifact is better than what we have, pick it
							//combined artifacts are not always allowed to move
							if(artifactScore > otherArtifactScore && artifact->canBePutAt(target, slot, true))
							{
								std::pair swapPair = std::minmax<ArtifactInstanceID>({artifact->getId(), otherSlot->artifactID});
								if(swappedSet.find(swapPair) != swappedSet.end())
								{
									logAi->warn(
										"Artifacts % s < -> % s have already swapped before, ignored.",
										artifact->getType()->getJsonKey(),
										otherSlot->getArt()->getType()->getJsonKey()
									);
									continue;
								}
								logAi->trace("Exchange artifacts %s <-> %s", artifact->getType()->getJsonKey(), otherSlot->getArt()->getType()->getJsonKey());

								if(!otherSlot->getArt()->canBePutAt(artHolder, location.slot, true))
								{
									ArtifactLocation destLocation(target->id, slot);
									ArtifactLocation backpack(artHolder->id, ArtifactPosition::BACKPACK_START);

									cc->swapArtifacts(destLocation, backpack);
									cc->swapArtifacts(location, destLocation);
								}
								else
								{
									cc->swapArtifacts(location, ArtifactLocation(target->id, target->getArtPos(otherSlot->getArt())));
								}

								changeMade = true;
								swappedSet.insert(swapPair);
								break;
							}
						}
					}
				}
				if(changeMade)
					break; //start evaluating artifacts from scratch
			}
		} while(changeMade);
	};

	equipBest(h, other, true);

	if(other)
		equipBest(h, other, false);
}

}
