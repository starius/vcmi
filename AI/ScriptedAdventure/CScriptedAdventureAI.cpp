/*
 * CScriptedAdventureAI.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CScriptedAdventureAI.h"

#include "../../lib/AsyncRunner.h"
#include "../../lib/CCreatureHandler.h"
#include "../../lib/CPlayerState.h"
#include "../../lib/ResourceSet.h"
#include "../../lib/VCMIDirs.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CTown.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/mapObjects/CGDwelling.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGObjectInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../lib/networkPacks/SaveLocalState.h"
#include "../../lib/pathfinder/CGPathNode.h"
#include "../../lib/pathfinder/PathfinderOptions.h"
#include "../../lib/serializer/CTypeList.h"
#include "../../luascript/LuaAdventureScriptRunner.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <tuple>

namespace ScriptedAdventureAI
{
namespace
{

const std::string SCRIPT_MEMORY_LOCAL_STATE_KEY = "scriptedAdventureAI";

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

int32_t readInteger(const JsonNode & node, const std::string & field)
{
	if(!node[field].isNumber())
		throw std::invalid_argument("Missing or non-integer script action field: " + field);

	if(node[field].getType() == JsonNode::JsonType::DATA_INTEGER)
		return static_cast<int32_t>(node[field].Integer());

	const double value = node[field].Float();
	const double integralValue = std::trunc(value);
	if(!std::isfinite(value)
		|| value != integralValue
		|| value < static_cast<double>(std::numeric_limits<int32_t>::min())
		|| value > static_cast<double>(std::numeric_limits<int32_t>::max()))
	{
		throw std::invalid_argument("Missing or non-integer script action field: " + field);
	}
	return static_cast<int32_t>(integralValue);
}

int32_t readInteger(const JsonNode & node, const std::string & field, int32_t defaultValue)
{
	if(!hasField(node, field))
		return defaultValue;
	return readInteger(node, field);
}

bool readBool(const JsonNode & node, const std::string & field, bool defaultValue)
{
	if(!hasField(node, field))
		return defaultValue;
	if(!node[field].isBool())
		throw std::invalid_argument("Non-boolean script AI config field: " + field);
	return node[field].Bool();
}

size_t readSize(const JsonNode & node, const std::string & field, size_t defaultValue, size_t minValue, size_t maxValue)
{
	if(!hasField(node, field))
		return defaultValue;
	if(!node[field].isNumber() || node[field].getType() != JsonNode::JsonType::DATA_INTEGER || node[field].Integer() < 0)
		throw std::invalid_argument("Non-negative integer script AI config field expected: " + field);
	return static_cast<size_t>(std::clamp<int64_t>(node[field].Integer(), static_cast<int64_t>(minValue), static_cast<int64_t>(maxValue)));
}

std::string readString(const JsonNode & node, const std::string & field)
{
	if(!node[field].isString())
		throw std::invalid_argument("Missing or non-string script action field: " + field);
	return node[field].String();
}

std::optional<std::string> readOptionalString(const JsonNode & node, const std::string & field)
{
	if(!hasField(node, field))
		return std::nullopt;
	return readString(node, field);
}

std::string normalizeScriptPath(std::string path)
{
	if(path.starts_with("file:"))
		return path;
	if(path.starts_with("scripts/"))
		path.erase(0, 8);
	if(path.starts_with("SCRIPTS/"))
		path.erase(0, 8);
	return path;
}

std::string toLowerAscii(std::string value)
{
	for(char & character : value)
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return value;
}

std::string toUpperAscii(std::string value)
{
	for(char & character : value)
		character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	return value;
}

std::optional<std::string> readEnvironmentString(const char * name)
{
	const char * value = std::getenv(name);
	if(!value || !*value)
		return std::nullopt;
	return std::string(value);
}

std::optional<bool> readEnvironmentBool(const char * name)
{
	const auto value = readEnvironmentString(name);
	if(!value)
		return std::nullopt;

	const std::string lower = toLowerAscii(*value);
	if(lower == "1" || lower == "true" || lower == "yes" || lower == "on")
		return true;
	if(lower == "0" || lower == "false" || lower == "no" || lower == "off")
		return false;
	return std::nullopt;
}

std::optional<std::string> externalScriptFilePath(const std::string & scriptPath)
{
	const std::string prefix = "file:";
	if(!scriptPath.starts_with(prefix))
		return std::nullopt;

	const std::string filePath = scriptPath.substr(prefix.size());
	if(filePath.empty())
		return std::nullopt;
	return filePath;
}

std::string jsonText(std::string value)
{
	for(char & character : value)
	{
		if(static_cast<unsigned char>(character) < 0x20)
			character = ' ';
	}
	return value;
}

std::string jsonPlayerColor(PlayerColor color)
{
	if(color == PlayerColor::UNFLAGGABLE)
		return "unflaggable";
	if(color == PlayerColor::CANNOT_DETERMINE)
		return "cannot_determine";

	return color.toString();
}

class ScopedCallbackWaitMode
{
	std::shared_ptr<CCallback> callback;
	bool previousWaitTillRealize;

public:
	ScopedCallbackWaitMode(std::shared_ptr<CCallback> callback, bool waitTillRealize)
		: callback(std::move(callback))
		, previousWaitTillRealize(this->callback ? this->callback->waitTillRealize : false)
	{
		if(this->callback)
			this->callback->waitTillRealize = waitTillRealize;
	}

	~ScopedCallbackWaitMode()
	{
		if(callback)
			callback->waitTillRealize = previousWaitTillRealize;
	}
};

JsonNode jsonPosition(const int3 & position)
{
	JsonNode node;
	node["x"] = JsonNode(position.x);
	node["y"] = JsonNode(position.y);
	node["z"] = JsonNode(position.z);
	return node;
}

JsonNode jsonResources(const ResourceSet & resources)
{
	JsonNode node;
	for(size_t i = 0; i < GameConstants::RESOURCE_QUANTITY; ++i)
		node[GameConstants::RESOURCE_NAMES[i]] = JsonNode(resources[i]);
	return node;
}

JsonNode jsonObjectId(const CGObjectInstance * object)
{
	JsonNode node;
	if(object)
		node = JsonNode(object->id.getNum());
	return node;
}

JsonNode jsonArmy(const CCreatureSet & army)
{
	JsonNode node;
	node.Vector();
	for(const auto & slot : army.Slots())
	{
		if(!slot.second)
			continue;

		JsonNode stack;
		stack["slot"] = JsonNode(slot.first.getNum());
		stack["creatureId"] = JsonNode(slot.second->getCreatureID().getNum());
		stack["count"] = JsonNode(slot.second->getCount());
		stack["name"] = JsonNode(jsonText(slot.second->getName()));
		node.Vector().push_back(stack);
	}
	return node;
}

std::string battleStateName(NK2AI::BattleState battleState)
{
	switch(battleState)
	{
		case NK2AI::NO_BATTLE:
			return "none";
		case NK2AI::UPCOMING_BATTLE:
			return "upcoming";
		case NK2AI::ONGOING_BATTLE:
			return "ongoing";
		case NK2AI::ENDING_BATTLE:
			return "ending";
	}
	return "unknown";
}

std::string pathActionName(EPathNodeAction action)
{
	switch(action)
	{
		case EPathNodeAction::UNKNOWN:
			return "unknown";
		case EPathNodeAction::EMBARK:
			return "embark";
		case EPathNodeAction::DISEMBARK:
			return "disembark";
		case EPathNodeAction::NORMAL:
			return "normal";
		case EPathNodeAction::BATTLE:
			return "battle";
		case EPathNodeAction::VISIT:
			return "visit";
		case EPathNodeAction::BLOCKING_VISIT:
			return "blocking_visit";
		case EPathNodeAction::TELEPORT_NORMAL:
			return "teleport_normal";
		case EPathNodeAction::TELEPORT_BLOCKING_VISIT:
			return "teleport_blocking_visit";
		case EPathNodeAction::TELEPORT_BATTLE:
			return "teleport_battle";
	}
	return "unknown";
}

std::string movementResultName(TryMoveHero::EResult result)
{
	switch(result)
	{
		case TryMoveHero::FAILED:
			return "failed";
		case TryMoveHero::SUCCESS:
			return "success";
		case TryMoveHero::TELEPORTATION:
			return "teleportation";
		case TryMoveHero::BLOCKING_VISIT:
			return "blocking_visit";
		case TryMoveHero::EMBARK:
			return "embark";
		case TryMoveHero::DISEMBARK:
			return "disembark";
	}
	return "unknown";
}

JsonNode jsonPositions(const FowTilesType & positions, size_t maxPositions)
{
	JsonNode node;
	node.Vector();
	size_t count = 0;
	for(const int3 & position : positions)
	{
		if(count++ >= maxPositions)
			break;
		node.Vector().push_back(jsonPosition(position));
	}
	return node;
}

JsonNode jsonPositions(const std::vector<int3> & positions)
{
	JsonNode node;
	node.Vector();
	for(const int3 & position : positions)
		node.Vector().push_back(jsonPosition(position));
	return node;
}

std::string makeRouteId(
	int32_t heroId,
	const int3 & start,
	const int3 & destination,
	const EPathfindingLayer & layer,
	int32_t movementRemaining)
{
	std::ostringstream stream;
	stream
		<< "h" << heroId
		<< ":s" << start.x << "," << start.y << "," << start.z
		<< ":d" << destination.x << "," << destination.y << "," << destination.z
		<< ":l" << layer.getNum()
		<< ":m" << movementRemaining;
	return stream.str();
}

JsonNode jsonPathNode(const CGPathNode & pathNode)
{
	JsonNode node;
	node["destination"] = jsonPosition(pathNode.coord);
	node["layerId"] = JsonNode(pathNode.layer.getNum());
	node["turns"] = JsonNode(static_cast<int32_t>(pathNode.turns));
	node["movementRemaining"] = JsonNode(pathNode.moveRemains);
	node["cost"].Float() = pathNode.cost;
	node["pathAction"] = JsonNode(pathActionName(pathNode.action));
	node["isTeleportAction"] = JsonNode(pathNode.isTeleportAction());
	return node;
}

std::string riskLabel(uint64_t danger, double dangerRatio, bool safe)
{
	if(danger == 0)
		return "none";
	if(safe)
		return "acceptable";
	if(dangerRatio >= 1.5)
		return "critical";
	if(dangerRatio >= 1.0)
		return "high";
	return "risky";
}

JsonNode jsonRisk(const CGHeroInstance * hero, uint64_t danger, bool safe)
{
	const int64_t heroStrength = hero ? static_cast<int64_t>(hero->getArmyStrength()) : 0;
	const double dangerRatio = static_cast<double>(danger) / std::max(1.0, static_cast<double>(heroStrength));

	JsonNode node;
	node["danger"] = JsonNode(static_cast<int64_t>(danger));
	node["heroStrength"] = JsonNode(heroStrength);
	node["dangerRatio"] = JsonNode(dangerRatio);
	node["safe"] = JsonNode(safe);
	node["risk"] = JsonNode(riskLabel(danger, dangerRatio, safe));
	node["estimatedLoss"] = JsonNode(safe ? 0 : static_cast<int64_t>(danger));
	return node;
}

JsonNode jsonMapObject(const CGObjectInstance * object, PlayerColor player, const CGHeroInstance * contextHero)
{
	JsonNode node;
	node["id"] = JsonNode(object->id.getNum());
	node["typeId"] = JsonNode(object->ID.getNum());
	node["subtypeId"] = JsonNode(object->subID.getNum());
	node["type"] = JsonNode(jsonText(object->getTypeName()));
	node["subtype"] = JsonNode(jsonText(object->getSubtypeName()));
	node["name"] = JsonNode(jsonText(object->getObjectName()));
	node["hoverText"] = JsonNode(jsonText(contextHero ? object->getHoverText(contextHero) : object->getHoverText(player)));
	node["owner"] = JsonNode(jsonPlayerColor(object->tempOwner));
	node["position"] = jsonPosition(object->visitablePos());
	node["passableForPlayer"] = JsonNode(object->passableFor(player));
	return node;
}

JsonNode jsonHero(const CGHeroInstance * hero)
{
	JsonNode node;
	node["id"] = JsonNode(hero->id.getNum());
	node["name"] = JsonNode(jsonText(hero->getNameTranslated()));
	node["position"] = jsonPosition(hero->visitablePos());
	node["owner"] = JsonNode(jsonPlayerColor(hero->tempOwner));
	node["level"] = JsonNode(static_cast<int32_t>(hero->level));
	node["mana"] = JsonNode(hero->mana);
	node["manaLimit"] = JsonNode(hero->manaLimit());
	node["movement"] = JsonNode(hero->movementPointsRemaining());
	node["movementLimit"] = JsonNode(hero->movementPointsLimit());
	node["primarySkills"]["attack"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::ATTACK));
	node["primarySkills"]["defense"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::DEFENSE));
	node["primarySkills"]["spellPower"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER));
	node["primarySkills"]["knowledge"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::KNOWLEDGE));
	node["armyStrength"] = JsonNode(static_cast<int64_t>(hero->getArmyStrength()));
	node["army"] = jsonArmy(*hero);
	return node;
}

JsonNode jsonRecruitOption(const CGDwelling * dwelling, const CArmedInstance * destination, int32_t level, const ResourceSet & resources)
{
	JsonNode node;
	if(level < 0 || level >= static_cast<int32_t>(dwelling->creatures.size()))
		return node;
	if(dwelling->creatures[level].second.empty())
		return node;

	const CreatureID creatureID = dwelling->creatures[level].second.back();
	const CCreature * creature = creatureID.toCreature();
	const int32_t available = static_cast<int32_t>(dwelling->creatures[level].first);
	ResourceSet spendableResources = resources;
	const int32_t affordable = creature ? spendableResources / creature->getFullRecruitCost() : available;
	const int32_t amount = std::min(available, affordable);
	if(destination && destination->stacksCount() >= GameConstants::ARMY_SIZE && !destination->getSlotFor(creatureID).validSlot())
		return node;

	node["source_id"] = JsonNode(dwelling->id.getNum());
	node["destination_id"] = JsonNode(destination ? destination->id.getNum() : dwelling->id.getNum());
	node["level"] = JsonNode(level);
	node["creature_id"] = JsonNode(creatureID.getNum());
	node["creature"] = JsonNode(creature ? jsonText(creature->getNameSingularTranslated()) : "");
	node["available"] = JsonNode(available);
	node["affordable"] = JsonNode(affordable);
	node["amount"] = JsonNode(amount);
	if(creature)
		node["cost"] = jsonResources(creature->getFullRecruitCost());
	node["planAction"]["type"] = JsonNode("recruit");
	node["planAction"]["source_id"] = node["source_id"];
	node["planAction"]["destination_id"] = node["destination_id"];
	node["planAction"]["level"] = node["level"];
	node["planAction"]["creature_id"] = node["creature_id"];
	node["planAction"]["amount"] = node["amount"];
	return node;
}

JsonNode jsonTown(const CGTownInstance * town, const ResourceSet & resources)
{
	JsonNode node;
	node["id"] = JsonNode(town->id.getNum());
	node["name"] = JsonNode(jsonText(town->getNameTranslated()));
	node["position"] = jsonPosition(town->visitablePos());
	node["owner"] = JsonNode(jsonPlayerColor(town->tempOwner));
	node["fortLevel"] = JsonNode(static_cast<int32_t>(town->fortLevel()));
	node["visitingHeroId"] = jsonObjectId(town->getVisitingHero());
	node["garrisonHeroId"] = jsonObjectId(town->getGarrisonHero());
	node["armyStrength"] = JsonNode(static_cast<int64_t>(town->getUpperArmy()->getArmyStrength(town->fortLevel())));
	node["army"] = jsonArmy(*town);
	node["buildings"].Vector();
	for(const BuildingID & building : town->getBuildings())
		node["buildings"].Vector().push_back(JsonNode(building.getNum()));
	node["recruitOptions"].Vector();
	if(!town->getVisitingHero())
	{
		const CArmedInstance * destination = town->getUpperArmy();
		for(int32_t level = 0; level < static_cast<int32_t>(town->creatures.size()); ++level)
		{
			JsonNode option = jsonRecruitOption(town, destination, level, resources);
			if(option.isStruct() && option["available"].Integer() > 0)
				node["recruitOptions"].Vector().push_back(option);
		}
	}
	return node;
}

JsonNode jsonBuildOption(const CGTownInstance * town, const CBuilding * building)
{
	JsonNode node;
	node["town_id"] = JsonNode(town->id.getNum());
	node["town"] = JsonNode(jsonText(town->getNameTranslated()));
	node["building_id"] = JsonNode(building->bid.getNum());
	node["building"] = JsonNode(jsonText(building->getNameTranslated()));
	node["cost"] = jsonResources(building->resources);
	node["income"] = jsonResources(building->produce);
	node["planAction"]["type"] = JsonNode("build");
	node["planAction"]["town_id"] = node["town_id"];
	node["planAction"]["building_id"] = node["building_id"];
	return node;
}

bool isObjectPathAction(EPathNodeAction action)
{
	return action == EPathNodeAction::BATTLE
		|| action == EPathNodeAction::VISIT
		|| action == EPathNodeAction::BLOCKING_VISIT
		|| action == EPathNodeAction::TELEPORT_BLOCKING_VISIT
		|| action == EPathNodeAction::TELEPORT_BATTLE;
}

bool isScriptObjectTarget(const CGObjectInstance * object, PlayerColor player)
{
	const auto * hero = dynamic_cast<const CGHeroInstance *>(object);
	if(hero && hero->tempOwner == player)
		return false;
	if(object->ID == Obj::TREASURE_CHEST || object->ID == Obj::SEA_CHEST)
		return false;

	return true;
}

}

CScriptedAdventureAI::CScriptedAdventureAI()
	: scriptPath("ai/defaultAdventure.lua")
{
}

CScriptedAdventureAI::~CScriptedAdventureAI() = default;

void CScriptedAdventureAI::initGameInterface(std::shared_ptr<Environment> env, std::shared_ptr<CCallback> callback)
{
	AIGateway::initGameInterface(std::move(env), std::move(callback));
	loadConfig();
	loadScriptMemoryFromLocalState();
}

void CScriptedAdventureAI::yourTurn(QueryID queryID)
{
	LOG_TRACE_PARAMS(logAi, "queryID '%i'", queryID);
	nullkiller->invalidatePathfinderData();
	status.addQuery(queryID, "YourTurn");
	answerQueryWithoutGameStateLock("scriptedYourTurn", queryID, 0);
	status.startedTurn();

	nullkiller->makingTurnInterruption.reset();

	asyncTasks->run([this]()
	{
		ScopedThreadName guard("ScriptedAdventureAI::makingTurn");
		status.waitTillFree();
		makeScriptedTurn();
	});
}

void CScriptedAdventureAI::answerQueryWithoutGameStateLock(const std::string & description, QueryID queryID, int selection)
{
	if(!asyncTasks)
		throw std::runtime_error("Attempt to answer query on shut down AI state.");

	asyncTasks->run([this, description, queryID, selection]() noexcept
	{
		ScopedThreadName guard("ScriptedAdventureAI::" + description);
		try
		{
			answerQuery(queryID, selection);
		}
		catch(const TerminationRequestedException &)
		{
			logAi->debug("%s thread has been terminated. We'll end it immediately", description);
		}
	});
}

void CScriptedAdventureAI::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID)
{
	(void)hero;
	(void)pskill;
	(void)skills;
	status.addQuery(queryID, "ScriptedAdventureAI hero level dialog");
	answerQueryWithoutGameStateLock("scriptedHeroGotLevel", queryID, 0);
}

void CScriptedAdventureAI::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID)
{
	(void)commander;
	(void)skills;
	status.addQuery(queryID, "ScriptedAdventureAI commander level dialog");
	answerQueryWithoutGameStateLock("scriptedCommanderGotLevel", queryID, 0);
}

void CScriptedAdventureAI::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	(void)text;
	(void)soundID;
	(void)safeToAutoaccept;
	int answer = 0;
	if(selection && !components.empty())
		answer = static_cast<int>(components.size());
	else if(!selection && cancel)
		answer = 1;

	status.addQuery(askID, "ScriptedAdventureAI blocking dialog");
	answerQueryWithoutGameStateLock("scriptedShowBlockingDialog", askID, answer);
}

void CScriptedAdventureAI::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	(void)hero;
	(void)channel;
	const int answer = (!impassable && !exits.empty()) ? 0 : -1;
	status.addQuery(askID, "ScriptedAdventureAI teleport dialog");
	answerQueryWithoutGameStateLock("scriptedShowTeleportDialog", askID, answer);
}

void CScriptedAdventureAI::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	(void)icon;
	(void)title;
	(void)description;
	const int answer = objects.empty() ? 0 : objects.front().getNum();
	status.addQuery(askID, "ScriptedAdventureAI map object select dialog");
	answerQueryWithoutGameStateLock("scriptedShowMapObjectSelectDialog", askID, answer);
}

void CScriptedAdventureAI::buildChanged(const CGTownInstance * town, BuildingID buildingID, int what)
{
	if(town && cc && cc->isVisibleFor(town, playerID))
	{
		JsonNode data;
		data["town"] = jsonTown(town, cc->getResourceAmount());
		data["building_id"] = JsonNode(buildingID.getNum());
		data["change"] = JsonNode(what);
		appendScriptUpdate("build_changed", data, isOpponent(town->tempOwner));
	}

	AIGateway::buildChanged(town, buildingID, what);
}

void CScriptedAdventureAI::heroMoved(const TryMoveHero & details, bool verbose)
{
	const CGHeroInstance * hero = cc ? cc->getHero(details.id) : nullptr;
	const bool visibleMovement = cc
		&& ((hero && cc->isVisibleFor(hero, playerID))
			|| cc->isVisibleFor(details.start, playerID)
			|| cc->isVisibleFor(details.end, playerID));
	if(visibleMovement)
	{
		JsonNode data;
		data["hero_id"] = JsonNode(details.id.getNum());
		data["result"] = JsonNode(movementResultName(details.result));
		data["movementRemaining"] = JsonNode(static_cast<int32_t>(details.movePoints));
		data["startAnchor"] = jsonPosition(details.start);
		data["endAnchor"] = jsonPosition(details.end);
		data["attackedFrom"] = jsonPosition(details.attackedFrom);
		data["revealedTilesCount"] = JsonNode(static_cast<int32_t>(details.fowRevealed.size()));
		data["revealedTiles"] = jsonPositions(details.fowRevealed, 16);
		if(hero && cc->isVisibleFor(hero, playerID))
			data["hero"] = jsonHero(hero);

		appendScriptUpdate("hero_moved", data, hero ? isOpponent(hero->tempOwner) : true);
	}
	AIGateway::heroMoved(details, verbose);
}

void CScriptedAdventureAI::heroCreated(const CGHeroInstance * hero)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID))
	{
		JsonNode data;
		data["hero"] = jsonHero(hero);
		appendScriptUpdate("hero_created", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroCreated(hero);
}

void CScriptedAdventureAI::heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town)
{
	if(hero && town && cc && (cc->isVisibleFor(hero, playerID) || cc->isVisibleFor(town, playerID)))
	{
		JsonNode data;
		data["hero"] = jsonHero(hero);
		data["town"] = jsonMapObject(town, playerID, hero);
		appendScriptUpdate("hero_visits_town", data, isOpponent(hero->tempOwner) || isOpponent(town->tempOwner));
	}

	AIGateway::heroVisitsTown(hero, town);
}

void CScriptedAdventureAI::showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QueryID queryID)
{
	(void)object;
	(void)visitor;
	status.addQuery(queryID, "ScriptedAdventureAI tavern dialog");
	answerQueryWithoutGameStateLock("scriptedShowTavernWindow", queryID, 0);
}

void CScriptedAdventureAI::heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query)
{
	(void)hero1;
	(void)hero2;
	status.addQuery(query, "ScriptedAdventureAI hero exchange dialog");
	answerQueryWithoutGameStateLock("scriptedHeroExchangeStarted", query, 0);
}

void CScriptedAdventureAI::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	(void)up;
	(void)down;
	(void)removableUnits;
	(void)customTitle;
	status.addQuery(queryID, "ScriptedAdventureAI garrison dialog");
	answerQueryWithoutGameStateLock("scriptedShowGarrisonDialog", queryID, 0);
}

void CScriptedAdventureAI::showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QueryID queryID)
{
	(void)dwelling;
	(void)dst;
	(void)level;
	status.addQuery(queryID, "ScriptedAdventureAI recruitment dialog");
	answerQueryWithoutGameStateLock("scriptedShowRecruitmentDialog", queryID, 0);
}

void CScriptedAdventureAI::showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	(void)market;
	(void)visitor;
	status.addQuery(queryID, "ScriptedAdventureAI university dialog");
	answerQueryWithoutGameStateLock("scriptedShowUniversityWindow", queryID, 0);
}

void CScriptedAdventureAI::showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	(void)market;
	(void)visitor;
	status.addQuery(queryID, "ScriptedAdventureAI market dialog");
	answerQueryWithoutGameStateLock("scriptedShowMarketWindow", queryID, 0);
}

void CScriptedAdventureAI::tileRevealed(const FowTilesType & pos)
{
	JsonNode data;
	data["count"] = JsonNode(static_cast<int32_t>(pos.size()));
	data["tiles"] = jsonPositions(pos, 32);
	appendScriptUpdate("tile_revealed", data, false);

	AIGateway::tileRevealed(pos);
}

void CScriptedAdventureAI::newObject(const CGObjectInstance * obj)
{
	if(obj && cc && cc->isVisibleFor(obj, playerID))
	{
		JsonNode data;
		data["object"] = jsonMapObject(obj, playerID, nullptr);
		appendScriptUpdate("new_object", data, isOpponent(obj->tempOwner));
	}

	AIGateway::newObject(obj);
}

void CScriptedAdventureAI::objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator)
{
	if(obj && cc && cc->isVisibleFor(obj, playerID))
	{
		JsonNode data;
		data["object"] = jsonMapObject(obj, playerID, nullptr);
		data["initiator"] = JsonNode(initiator.toString());
		appendScriptUpdate("object_removed", data, isOpponent(obj->tempOwner) || isOpponent(initiator));
	}

	AIGateway::objectRemoved(obj, initiator);
}

void CScriptedAdventureAI::requestSent(const CPackForServer * pack, int requestID)
{
	if(const auto * reply = dynamic_cast<const QueryReply *>(pack))
	{
		std::optional<bool> earlyResult;
		{
			std::lock_guard lock(queryReplyMutex);
			queryReplyRequests[requestID] = reply->qid;
			if(const auto early = earlyQueryReplyResults.find(requestID); early != earlyQueryReplyResults.end())
			{
				earlyResult = early->second;
				earlyQueryReplyResults.erase(early);
			}
		}

		status.attemptedAnsweringQuery(reply->qid, requestID);
		if(earlyResult)
		{
			status.receivedAnswerConfirmation(requestID, *earlyResult);
			std::lock_guard lock(queryReplyMutex);
			queryReplyRequests.erase(requestID);
		}
	}
	else
	{
		AIGateway::requestSent(pack, requestID);
	}

	std::lock_guard lock(requestMutex);
	if(pendingRequest && pack && pendingRequest->requestID < 0 && pendingRequest->typeName == typeid(*pack).name())
	{
		pendingRequest->requestID = requestID;
		requestCv.notify_all();
	}
}

void CScriptedAdventureAI::requestRealized(PackageApplied * pa)
{
	const uint16_t queryReplyType = CTypeList::getInstance().getTypeID<QueryReply>(nullptr);
	if(pa && pa->packType == queryReplyType)
	{
		std::optional<QueryID> queryID;
		{
			std::lock_guard lock(queryReplyMutex);
			if(const auto query = queryReplyRequests.find(static_cast<int>(pa->requestID)); query != queryReplyRequests.end())
			{
				queryID = query->second;
				queryReplyRequests.erase(query);
			}
			else
			{
				earlyQueryReplyResults[static_cast<int>(pa->requestID)] = pa->result;
			}
		}

		if(queryID)
			status.receivedAnswerConfirmation(static_cast<int>(pa->requestID), pa->result);
	}
	else
	{
		AIGateway::requestRealized(pa);
	}

	std::lock_guard lock(requestMutex);
	if(pendingRequest && pa
		&& ((pendingRequest->requestID >= 0 && pendingRequest->requestID == static_cast<int>(pa->requestID))
			|| (pendingRequest->requestID < 0 && pendingRequest->expectedPackType != 0 && pendingRequest->expectedPackType == pa->packType)))
	{
		pendingRequest->requestID = static_cast<int>(pa->requestID);
		pendingRequest->packType = pa->packType;
		pendingRequest->realized = true;
		pendingRequest->applied = pa->result;
		requestCv.notify_all();
	}
}

CScriptedAdventureAI::RequestWaitResult CScriptedAdventureAI::submitAndWaitForRequest(const std::type_info & requestType, uint16_t expectedPackType, const std::function<void()> & submit)
{
	PendingRequest request;
	request.token = ++nextRequestToken;
	request.typeName = requestType.name();
	request.expectedPackType = expectedPackType;

	{
		std::lock_guard lock(requestMutex);
		pendingRequest = request;
	}

	submit();

	std::unique_lock lock(requestMutex);
	requestCv.wait_for(lock, std::chrono::seconds(10), [&]
	{
		return pendingRequest
			&& pendingRequest->token == request.token
			&& (pendingRequest->requestID >= 0 || pendingRequest->realized);
	});

	if(!pendingRequest || pendingRequest->token != request.token || (pendingRequest->requestID < 0 && !pendingRequest->realized))
	{
		pendingRequest.reset();
		return {};
	}

	requestCv.wait_for(lock, std::chrono::seconds(10), [&]
	{
		return pendingRequest && pendingRequest->token == request.token && pendingRequest->realized;
	});

	RequestWaitResult result;
	if(pendingRequest && pendingRequest->token == request.token)
	{
		result.sent = pendingRequest->requestID >= 0;
		result.realized = pendingRequest->realized;
		result.applied = pendingRequest->applied;
		result.requestID = pendingRequest->requestID;
		result.packType = pendingRequest->packType;
		pendingRequest.reset();
	}
	return result;
}

JsonNode CScriptedAdventureAI::jsonRequestWaitResult(const RequestWaitResult & request) const
{
	JsonNode result;
	result["ok"] = JsonNode(request.sent && request.realized && request.applied);
	result["sent"] = JsonNode(request.sent);
	result["realized"] = JsonNode(request.realized);
	result["applied"] = JsonNode(request.applied);
	result["requestId"] = JsonNode(request.requestID);
	result["packType"] = JsonNode(static_cast<int32_t>(request.packType));
	if(request.sent && !request.realized)
		result["error"] = JsonNode("Timed out waiting for server request result");
	else if(!request.sent)
		result["error"] = JsonNode("No matching request was sent");
	else if(!request.applied)
		result["error"] = JsonNode("Server rejected the request");
	return result;
}

void CScriptedAdventureAI::makeScriptedTurn()
{
	try
	{
		if(tryMakeScriptedTurn())
		{
			consecutiveScriptFailures = 0;
		}
		else
		{
			AIGateway::makeTurn();
		}
	}
	catch(const InterruptionRequestedException &)
	{
		logAi->debug("ScriptedAdventureAI turn thread has been interrupted. We'll end without calling endTurn.");
	}
	catch(const TerminationRequestedException &)
	{
		logAi->debug("ScriptedAdventureAI turn thread has been terminated. We'll end without calling endTurn.");
	}
	catch(const std::exception & e)
	{
		fallbackToNullkiller(e.what());
		AIGateway::makeTurn();
	}
}

bool CScriptedAdventureAI::tryMakeScriptedTurn()
{
	failureRecordedThisTurn = false;
	const int currentDay = cc->getCalendar().getCurrentDay();
	if(disabledUntilDay > currentDay)
	{
		logAi->warn("ScriptedAdventureAI disabled until day %d after repeated script failures.", disabledUntilDay);
		return false;
	}

	const auto source = getScriptSource();
	if(!source)
	{
		fallbackToNullkiller("script source is not available");
		return false;
	}

	auto runner = makeRunner(*source);
	JsonNode progress;

	for(size_t callIndex = 0; callIndex < maxScriptCallsPerTurn && status.haveTurn(); ++callIndex)
	{
		AI::AdventureScriptInput input;
		input.state = makeScriptInputState();
		input.updates = makeScriptUpdates(false);
		input.opponentUpdates = makeScriptUpdates(true);
		input.progress = progress;
		input.memory = scriptMemory;
		input.actionSpace = makeScriptActionSpace();
		input.analysis = makeScriptAnalysis();
		input.limits = makeScriptInputLimits();

		if(scriptConfig.trace)
		{
			JsonNode trace;
			trace["callIndex"] = JsonNode(static_cast<int32_t>(callIndex));
			trace["input"] = input.toJson();
			writeTraceEvent("input", trace);
		}

		const AI::AdventureScriptOutput output = runner->planDay(input);
		scriptMemory = output.memory;
		saveScriptMemoryToLocalState();
		if(scriptConfig.trace)
		{
			JsonNode trace;
			trace["callIndex"] = JsonNode(static_cast<int32_t>(callIndex));
			trace["output"] = AI::makeAdventureScriptOutputJson(output);
			writeTraceEvent("output", trace);
		}

		if(output.status == AI::AdventureScriptStatus::FALLBACK)
		{
			fallbackToNullkiller("script requested fallback");
			return false;
		}

		JsonNode executed;
		JsonNode failed;
		JsonNode remaining;
		executed.Vector();
		failed.Vector();
		remaining.Vector();

		bool stopped = false;
		for(size_t actionIndex = 0; actionIndex < output.actions.size(); ++actionIndex)
		{
			const JsonNode & action = output.actions[actionIndex];
			JsonNode actionResult;
			actionResult["index"] = JsonNode(static_cast<int32_t>(actionIndex));
			if(hasField(action, "id"))
				actionResult["id"] = action["id"];

			try
			{
				if(!executeScriptAction(action, actionResult))
					stopped = true;
			}
			catch(const std::exception & e)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode(e.what());
				failed.Vector().push_back(actionResult);
				for(size_t remainingIndex = actionIndex + 1; remainingIndex < output.actions.size(); ++remainingIndex)
					remaining.Vector().push_back(output.actions[remainingIndex]);
				stopped = true;
				break;
			}

			if(actionResult["ok"].isBool() && !actionResult["ok"].Bool())
			{
				failed.Vector().push_back(actionResult);
				for(size_t remainingIndex = actionIndex + 1; remainingIndex < output.actions.size(); ++remainingIndex)
					remaining.Vector().push_back(output.actions[remainingIndex]);
				stopped = true;
				break;
			}

			executed.Vector().push_back(actionResult);
			if(stopped || !status.haveTurn())
				break;
		}

		progress = makeProgressJson(executed, failed, remaining);
		if(scriptConfig.trace)
		{
			JsonNode trace;
			trace["callIndex"] = JsonNode(static_cast<int32_t>(callIndex));
			trace["stopped"] = JsonNode(stopped);
			trace["progress"] = progress;
			writeTraceEvent("progress", trace);
		}

		if(!status.haveTurn())
			return true;
		if(stopped)
			continue;
		if(output.status == AI::AdventureScriptStatus::END_TURN)
		{
			endTurn();
			return true;
		}
		if(output.status == AI::AdventureScriptStatus::NEED_REPLAN)
			continue;
		if(output.actions.empty())
		{
			fallbackToNullkiller("script returned no actions");
			return false;
		}
	}

	fallbackToNullkiller("script call limit reached");
	return false;
}

bool CScriptedAdventureAI::executeScriptAction(const JsonNode & action, JsonNode & actionResult)
{
	ScopedCallbackWaitMode nonBlockingCallback(cc, false);
	const std::string type = readString(action, "type");
	actionResult["type"] = JsonNode(type);

	if(type == "build")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Unknown town or town is not owned by scripted AI");

		const BuildingID buildingID(readInteger(action, "building_id"));
		if(cc->canBuildStructure(town, buildingID) != EBuildingState::ALLOWED)
			throw std::invalid_argument("Building is not currently allowed");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(BuildStructure), CTypeList::getInstance().getTypeID<BuildStructure>(nullptr), [&]
		{
			buildStructure(town, buildingID);
		});
		waitTillFree();
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["building_id"] = JsonNode(buildingID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Build request was rejected by server" : "Build request was not realized by server");
		return true;
	}

	if(type == "recruit")
	{
		const int32_t sourceID = hasField(action, "source_id") ? readInteger(action, "source_id") : readInteger(action, "town_id");
		const CGObjectInstance * sourceObject = cc->getObj(ObjectInstanceID(sourceID), false);
		const CGDwelling * dwelling = dynamic_cast<const CGDwelling *>(sourceObject);
		const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(sourceObject);
		const CArmedInstance * destination = town ? town->getUpperArmy() : dynamic_cast<const CArmedInstance *>(sourceObject);
		if(hasField(action, "destination_id"))
			destination = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(readInteger(action, "destination_id")), false));

		if(!dwelling || !destination || dwelling->tempOwner != playerID)
			throw std::invalid_argument("Unknown recruitment source, destination, or source is not owned by scripted AI");

		const int32_t level = readInteger(action, "level");
		if(level < 0 || level >= static_cast<int32_t>(dwelling->creatures.size()) || dwelling->creatures[level].second.empty())
			throw std::invalid_argument("Recruitment level is not available");

		const CreatureID creatureID = hasField(action, "creature_id") ? CreatureID(readInteger(action, "creature_id")) : dwelling->creatures[level].second.back();
		if(!vstd::contains(dwelling->creatures[level].second, creatureID))
			throw std::invalid_argument("creature_id is not available at this recruitment level");
		if(destination->stacksCount() >= GameConstants::ARMY_SIZE && !destination->getSlotFor(creatureID).validSlot())
			throw std::invalid_argument("No free army slot for recruited creature");

		int32_t amount = readInteger(action, "amount", static_cast<int32_t>(dwelling->creatures[level].first));
		amount = std::clamp<int32_t>(amount, 0, static_cast<int32_t>(dwelling->creatures[level].first));
		if(const CCreature * creature = creatureID.toCreature())
			amount = std::min<int32_t>(amount, cc->getResourceAmount() / creature->getFullRecruitCost());
		if(amount <= 0)
			throw std::invalid_argument("No recruitable or affordable creatures for this action");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(RecruitCreatures), CTypeList::getInstance().getTypeID<RecruitCreatures>(nullptr), [&]
		{
			cc->recruitCreatures(dwelling, destination, creatureID, amount, level);
		});
		waitTillFree();
		actionResult["source_id"] = JsonNode(dwelling->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["level"] = JsonNode(level);
		actionResult["creature_id"] = JsonNode(creatureID.getNum());
		actionResult["amount"] = JsonNode(amount);
		actionResult["request"] = jsonRequestWaitResult(request);
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Recruit request was rejected by server" : "Recruit request was not realized by server");
		return true;
	}

	if(type == "move_hero" || type == "visit_object")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		int3 destination;
		if(type == "visit_object")
		{
			const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(action, "object_id")), false);
			if(!object)
				throw std::invalid_argument("Unknown or not visible object_id");
			if(!isScriptObjectTarget(object, playerID))
				throw std::invalid_argument("visit_object cannot target an owned hero exchange");
			destination = object->visitablePos();
			actionResult["object_id"] = JsonNode(object->id.getNum());
		}
		else
		{
			const int z = hasField(action, "z") ? readInteger(action, "z") : hero->visitablePos().z;
			destination = int3(readInteger(action, "x"), readInteger(action, "y"), z);
		}

		const RoutePlan route = makeRoutePlan(hero, destination, readOptionalString(action, "route_id"));
		if(!route.ok)
			throw std::invalid_argument(route.error);

		const RequestWaitResult request = submitAndWaitForRequest(typeid(MoveHero), CTypeList::getInstance().getTypeID<MoveHero>(nullptr), [&]
		{
			cc->moveHero(hero, route.requestPath, route.transit, route.layer);
		});
		waitTillFree();
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["route_id"] = JsonNode(route.routeID);
		actionResult["destination"] = jsonPosition(route.destination);
		actionResult["submittedPath"] = jsonPositions(route.requestPath);
		actionResult["request"] = jsonRequestWaitResult(request);
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Move request was rejected by server" : "Move request was not realized by server");
		return request.applied && type != "visit_object" && !route.stopAfterMove;
	}

	if(type == "answer_query")
	{
		const QueryID queryID(readInteger(action, "query_id"));
		const int answer = hasField(action, "answer") ? readInteger(action, "answer") : 0;
		answerQuery(queryID, answer);
		waitTillFree();
		actionResult["query_id"] = JsonNode(queryID.getNum());
		actionResult["answer"] = JsonNode(answer);
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "end_turn")
	{
		const RequestWaitResult request = submitAndWaitForRequest(typeid(EndTurn), CTypeList::getInstance().getTypeID<EndTurn>(nullptr), [&]
		{
			cc->endTurn();
		});
		actionResult["request"] = jsonRequestWaitResult(request);
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "End turn request was rejected by server" : "End turn request was not realized by server");
		return false;
	}

	throw std::invalid_argument("Unsupported script action type: " + type);
}

JsonNode CScriptedAdventureAI::makeScriptInputState()
{
	JsonNode state;
	std::shared_lock gameStateLock(CGameState::mutex);

	state["day"] = JsonNode(cc->getCalendar().getCurrentDay());
	state["player"]["id"] = JsonNode(playerID.getNum());
	state["player"]["color"] = JsonNode(playerID.toString());
	state["turn"]["active"] = JsonNode(status.haveTurn());
	state["turn"]["pendingQueries"] = JsonNode(status.getQueriesCount());
	state["battle"]["state"] = JsonNode(battleStateName(status.getBattle()));

	if(const PlayerState * playerState = cc->getPlayerState(playerID, false))
	{
		state["player"]["team"] = JsonNode(playerState->team.getNum());
		state["player"]["status"] = JsonNode(static_cast<int32_t>(playerState->status));
		state["resources"] = jsonResources(playerState->resources);
	}
	else
	{
		state["resources"] = jsonResources(cc->getResourceAmount());
	}

	state["heroes"].Vector();
	for(const CGHeroInstance * hero : cc->getHeroesInfo())
	{
		if(hero)
			state["heroes"].Vector().push_back(jsonHero(hero));
	}

	const ResourceSet resources = cc->getResourceAmount();
	state["towns"].Vector();
	for(const CGTownInstance * town : cc->getTownsInfo())
	{
		if(town)
			state["towns"].Vector().push_back(jsonTown(town, resources));
	}
	return state;
}

JsonNode CScriptedAdventureAI::makeScriptActionSpace() const
{
	JsonNode actionSpace;
	actionSpace["acceptedActionTypes"].Vector();
	for(const std::string & type : AI::acceptedPlanActionTypes())
		actionSpace["acceptedActionTypes"].Vector().push_back(JsonNode(type));

	actionSpace["buildOptions"].Vector();
	actionSpace["recruitOptions"].Vector();
	actionSpace["reachableObjects"].Vector();
	actionSpace["movementOptions"].Vector();
	actionSpace["recommendedActions"].Vector();

	std::shared_lock gameStateLock(CGameState::mutex);
	const ResourceSet resources = cc->getResourceAmount();

	for(const CGTownInstance * town : cc->getTownsInfo(true))
	{
		if(!town || town->tempOwner != playerID)
			continue;

		for(const auto & buildingEntry : town->getTown()->buildings)
		{
			const CBuilding * building = buildingEntry.second.get();
			if(!building || building->mode != CBuilding::BUILD_NORMAL)
				continue;

			if(cc->canBuildStructure(town, building->bid) == EBuildingState::ALLOWED)
			{
				JsonNode option = jsonBuildOption(town, building);
				actionSpace["buildOptions"].Vector().push_back(option);
				actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
			}
		}

		if(!town->getVisitingHero())
		{
			const CArmedInstance * destination = town->getUpperArmy();
			for(int32_t level = 0; level < static_cast<int32_t>(town->creatures.size()); ++level)
			{
				JsonNode option = jsonRecruitOption(town, destination, level, resources);
				if(option.isStruct() && option["amount"].Integer() > 0)
				{
					actionSpace["recruitOptions"].Vector().push_back(option);
					actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
				}
			}
		}
	}

	struct MovementCandidate
	{
		JsonNode json;
		float cost = 0.0f;
		int32_t turns = 0;
		int32_t x = 0;
		int32_t y = 0;
		int32_t z = 0;
	};

	std::vector<MovementCandidate> movementCandidates;
	std::vector<MovementCandidate> objectCandidates;
	std::set<int32_t> seenTargetObjects;
	constexpr int radius = 16;
	constexpr size_t maxMovementOptions = 48;
	constexpr size_t maxObjectTargets = 24;

	for(const CGHeroInstance * hero : cc->getHeroesInfo())
	{
		if(!hero || hero->tempOwner != playerID || hero->movementPointsRemaining() <= 0)
			continue;

		CPathsInfo paths(cc->getMapSize(), hero);
		auto config = std::make_shared<SingleHeroPathfinderConfig>(paths, *cc, hero);
		cc->calculatePaths(config);

		FowTilesType tiles;
		cc->getTilesInRange(tiles, hero->visitablePos(), radius, ETileVisibility::REVEALED, playerID);

		for(const int3 & position : tiles)
		{
			if(position == hero->visitablePos())
				continue;
			if(!cc->isInTheMap(position) || !cc->isVisibleFor(position, playerID))
				continue;

			const CGPathNode * pathNode = paths.getPathInfo(position);
			if(!pathNode || !pathNode->reachable() || pathNode->turns != 0)
				continue;
			if(pathNode->accessible == EPathAccessibility::NOT_SET || pathNode->accessible == EPathAccessibility::BLOCKED)
				continue;

			const std::string routeID = makeRouteId(hero->id.getNum(), hero->visitablePos(), position, pathNode->layer, pathNode->moveRemains);
			const uint64_t danger = nullkiller && nullkiller->dangerEvaluator ? nullkiller->dangerEvaluator->evaluateDanger(position, hero, true) : 0;
			const bool safe = !danger || (nullkiller && nullkiller->settings && NK2AI::isSafeToVisit(hero, danger, nullkiller->settings->getSafeAttackRatio()));
			const JsonNode risk = jsonRisk(hero, danger, safe);
			const double dangerRatio = risk["dangerRatio"].Float();
			const double estimatedValue = std::max(0.0, 1000.0 - pathNode->cost * 160.0 - dangerRatio * 350.0);
			std::string reason = danger ? "reachable but guarded or dangerous" : "reachable this turn";
			if(pathNode->isTeleportAction())
				reason += "; teleport requires replan";

			JsonNode option;
			option["hero_id"] = JsonNode(hero->id.getNum());
			option["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
			option["path"] = jsonPathNode(*pathNode);
			option["route_id"] = JsonNode(routeID);
			option["danger"] = risk["danger"];
			option["dangerRatio"] = risk["dangerRatio"];
			option["estimatedLoss"] = risk["estimatedLoss"];
			option["risk"] = risk["risk"];
			option["safe"] = risk["safe"];
			option["riskInfo"] = risk;
			option["value"] = JsonNode(estimatedValue);
			option["reason"] = JsonNode(reason);
			option["blockedBy"].Vector();
			option["planAction"]["type"] = JsonNode("move_hero");
			option["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
			option["planAction"]["x"] = JsonNode(position.x);
			option["planAction"]["y"] = JsonNode(position.y);
			option["planAction"]["z"] = JsonNode(position.z);
			option["planAction"]["route_id"] = JsonNode(routeID);

			movementCandidates.push_back(MovementCandidate{option, pathNode->cost, static_cast<int32_t>(pathNode->turns), position.x, position.y, position.z});

			const CGObjectInstance * topObject = cc->getTopObj(position);
			if(topObject
				&& isObjectPathAction(pathNode->action)
				&& isScriptObjectTarget(topObject, playerID)
				&& seenTargetObjects.insert(topObject->id.getNum()).second)
			{
				JsonNode target;
				target["object"] = jsonMapObject(topObject, playerID, hero);
				target["hero_id"] = JsonNode(hero->id.getNum());
				target["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
				target["path"] = jsonPathNode(*pathNode);
				target["route_id"] = JsonNode(routeID);
				target["danger"] = risk["danger"];
				target["dangerRatio"] = risk["dangerRatio"];
				target["estimatedLoss"] = risk["estimatedLoss"];
				target["risk"] = risk["risk"];
				target["safe"] = risk["safe"];
				target["riskInfo"] = risk;
				target["value"] = JsonNode(estimatedValue);
				target["reason"] = JsonNode(reason + "; object target");
				target["blockedBy"].Vector();
				target["planAction"]["type"] = JsonNode("visit_object");
				target["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
				target["planAction"]["object_id"] = JsonNode(topObject->id.getNum());
				target["planAction"]["route_id"] = JsonNode(routeID);
				objectCandidates.push_back(MovementCandidate{target, pathNode->cost, static_cast<int32_t>(pathNode->turns), position.x, position.y, position.z});
			}
		}
	}

	std::sort(movementCandidates.begin(), movementCandidates.end(), [](const MovementCandidate & left, const MovementCandidate & right)
	{
		return std::tie(left.turns, left.cost, left.z, left.x, left.y) < std::tie(right.turns, right.cost, right.z, right.x, right.y);
	});
	std::sort(objectCandidates.begin(), objectCandidates.end(), [](const MovementCandidate & left, const MovementCandidate & right)
	{
		return std::tie(left.turns, left.cost, left.z, left.x, left.y) < std::tie(right.turns, right.cost, right.z, right.x, right.y);
	});

	for(size_t index = 0; index < movementCandidates.size() && index < maxMovementOptions; ++index)
		actionSpace["movementOptions"].Vector().push_back(movementCandidates[index].json);
	for(size_t index = 0; index < objectCandidates.size() && index < maxObjectTargets; ++index)
	{
		actionSpace["reachableObjects"].Vector().push_back(objectCandidates[index].json);
		actionSpace["recommendedActions"].Vector().push_back(objectCandidates[index].json["planAction"]);
	}

	actionSpace["endTurnAction"]["type"] = JsonNode("end_turn");
	return actionSpace;
}

JsonNode CScriptedAdventureAI::makeScriptAnalysis() const
{
	JsonNode analysis;
	analysis["candidateLimits"]["reachableRadius"] = JsonNode(16);
	analysis["candidateLimits"]["maxMovementOptions"] = JsonNode(48);
	analysis["candidateLimits"]["maxObjectTargets"] = JsonNode(24);
	analysis["candidateLimits"]["maxUpdateEvents"] = JsonNode(static_cast<int32_t>(maxScriptUpdateJournal));
	analysis["execution"]["validatesOwnership"] = JsonNode(true);
	analysis["execution"]["validatesVisibility"] = JsonNode(true);
	analysis["execution"]["validatesRouteIds"] = JsonNode(true);
	analysis["execution"]["replansAfterObjectVisit"] = JsonNode(true);
	analysis["execution"]["fallbackAI"] = JsonNode("Nullkiller2");
	analysis["scriptMemory"]["persistedInPlayerLocalSettings"] = JsonNode(true);
	analysis["scriptMemory"]["localStateKey"] = JsonNode(SCRIPT_MEMORY_LOCAL_STATE_KEY);
	analysis["candidateFields"].Vector();
	for(const char * field : { "reason", "value", "risk", "safe", "danger", "dangerRatio", "estimatedLoss", "blockedBy" })
		analysis["candidateFields"].Vector().push_back(JsonNode(field));
	analysis["danger"]["candidateDangerSource"] = JsonNode("Nullkiller direct object/guard danger evaluator");
	analysis["danger"]["enemyReachSource"] = JsonNode("visible enemy distance and strength alerts");
	analysis["visibleEnemyHeroes"].Vector();
	analysis["visibleEnemyTowns"].Vector();
	analysis["defenseAlerts"].Vector();
	analysis["heroThreatAlerts"].Vector();

	std::shared_lock gameStateLock(CGameState::mutex);
	const ResourceSet resources = cc->getResourceAmount();
	std::vector<const CGHeroInstance *> enemyHeroes;

	for(const CGObjectInstance * object : cc->getAllVisitableObjs())
	{
		if(!object || !object->tempOwner.isValidPlayer() || !isOpponent(object->tempOwner))
			continue;

		if(const auto * enemyHero = dynamic_cast<const CGHeroInstance *>(object))
		{
			enemyHeroes.push_back(enemyHero);
			JsonNode enemyHeroJson = jsonHero(enemyHero);
			enemyHeroJson["visibleObject"] = jsonMapObject(enemyHero, playerID, nullptr);
			analysis["visibleEnemyHeroes"].Vector().push_back(enemyHeroJson);
		}
		else if(const auto * enemyTown = dynamic_cast<const CGTownInstance *>(object))
		{
			analysis["visibleEnemyTowns"].Vector().push_back(jsonTown(enemyTown, resources));
		}
	}

	constexpr int defenseAlertRadius = 12;
	for(const CGTownInstance * town : cc->getTownsInfo(true))
	{
		if(!town || town->tempOwner != playerID)
			continue;

		const CArmedInstance * upperArmy = town->getUpperArmy();
		const int64_t townStrength = static_cast<int64_t>(upperArmy ? upperArmy->getArmyStrength(town->fortLevel()) : town->getArmyStrength(town->fortLevel()));
		for(const CGHeroInstance * enemyHero : enemyHeroes)
		{
			if(enemyHero->visitablePos().z != town->visitablePos().z)
				continue;

			const ui32 distanceSquared = town->visitablePos().dist2dSQ(enemyHero->visitablePos());
			if(distanceSquared > defenseAlertRadius * defenseAlertRadius)
				continue;

			const int64_t enemyStrength = static_cast<int64_t>(enemyHero->getArmyStrength());
			const double strengthRatio = static_cast<double>(enemyStrength) / std::max(1.0, static_cast<double>(townStrength));
			std::string level = "watch";
			if(strengthRatio >= 1.5)
				level = "critical";
			else if(strengthRatio >= 1.0)
				level = "high";

			JsonNode alert;
			alert["level"] = JsonNode(level);
			alert["town_id"] = JsonNode(town->id.getNum());
			alert["town"] = JsonNode(jsonText(town->getNameTranslated()));
			alert["townPosition"] = jsonPosition(town->visitablePos());
			alert["townStrength"] = JsonNode(townStrength);
			alert["enemyHeroId"] = JsonNode(enemyHero->id.getNum());
			alert["enemyHero"] = JsonNode(jsonText(enemyHero->getNameTranslated()));
			alert["enemyPosition"] = jsonPosition(enemyHero->visitablePos());
			alert["enemyStrength"] = JsonNode(enemyStrength);
			alert["distance"] = JsonNode(town->visitablePos().dist2d(enemyHero->visitablePos()));
			alert["distanceSquared"] = JsonNode(static_cast<int32_t>(distanceSquared));
			alert["strengthRatio"] = JsonNode(strengthRatio);
			analysis["defenseAlerts"].Vector().push_back(alert);
		}
	}

	for(const CGHeroInstance * hero : cc->getHeroesInfo())
	{
		if(!hero || hero->tempOwner != playerID)
			continue;

		const int64_t heroStrength = static_cast<int64_t>(hero->getArmyStrength());
		for(const CGHeroInstance * enemyHero : enemyHeroes)
		{
			if(enemyHero->visitablePos().z != hero->visitablePos().z)
				continue;

			const ui32 distanceSquared = hero->visitablePos().dist2dSQ(enemyHero->visitablePos());
			if(distanceSquared > defenseAlertRadius * defenseAlertRadius)
				continue;

			const int64_t enemyStrength = static_cast<int64_t>(enemyHero->getArmyStrength());
			const double strengthRatio = static_cast<double>(enemyStrength) / std::max(1.0, static_cast<double>(heroStrength));
			std::string level = "watch";
			if(strengthRatio >= 1.5)
				level = "critical";
			else if(strengthRatio >= 1.0)
				level = "high";

			JsonNode alert;
			alert["level"] = JsonNode(level);
			alert["hero_id"] = JsonNode(hero->id.getNum());
			alert["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
			alert["heroPosition"] = jsonPosition(hero->visitablePos());
			alert["heroStrength"] = JsonNode(heroStrength);
			alert["enemyHeroId"] = JsonNode(enemyHero->id.getNum());
			alert["enemyHero"] = JsonNode(jsonText(enemyHero->getNameTranslated()));
			alert["enemyPosition"] = jsonPosition(enemyHero->visitablePos());
			alert["enemyStrength"] = JsonNode(enemyStrength);
			alert["distance"] = JsonNode(hero->visitablePos().dist2d(enemyHero->visitablePos()));
			alert["distanceSquared"] = JsonNode(static_cast<int32_t>(distanceSquared));
			alert["strengthRatio"] = JsonNode(strengthRatio);
			analysis["heroThreatAlerts"].Vector().push_back(alert);
		}
	}

	return analysis;
}

JsonNode CScriptedAdventureAI::makeScriptUpdates(bool opponentOnly) const
{
	JsonNode updates;
	updates["opponentOnly"] = JsonNode(opponentOnly);
	updates["events"].Vector();

	std::lock_guard guard(scriptUpdateMutex);
	updates["latestRevision"] = JsonNode(static_cast<int64_t>(scriptUpdateRevision));
	for(const JsonNode & event : scriptUpdateJournal)
	{
		if(opponentOnly && (!event["opponent"].isBool() || !event["opponent"].Bool()))
			continue;
		updates["events"].Vector().push_back(event);
	}
	return updates;
}

JsonNode CScriptedAdventureAI::makeScriptInputLimits() const
{
	JsonNode node;
	node["maxActions"] = JsonNode(static_cast<int32_t>(limits.maxActions));
	node["maxMemoryBytes"] = JsonNode(static_cast<int32_t>(limits.maxMemoryBytes));
	node["maxScriptCallsPerTurn"] = JsonNode(static_cast<int32_t>(maxScriptCallsPerTurn));
	return node;
}

JsonNode CScriptedAdventureAI::makeProgressJson(const JsonNode & executed, const JsonNode & failed, const JsonNode & remaining) const
{
	JsonNode progress;
	progress["executed"] = executed;
	progress["failed"] = failed;
	progress["remaining"] = remaining;
	return progress;
}

CScriptedAdventureAI::RoutePlan CScriptedAdventureAI::makeRoutePlan(
	const CGHeroInstance * hero,
	const int3 & destination,
	const std::optional<std::string> & expectedRouteID) const
{
	RoutePlan result;
	if(!cc->isInTheMap(destination))
	{
		result.error = "Destination is outside of the map";
		return result;
	}
	if(destination == hero->visitablePos())
	{
		result.error = "Destination is the hero current tile";
		return result;
	}

	std::shared_lock gameStateLock(CGameState::mutex);
	CPathsInfo paths(cc->getMapSize(), hero);
	auto config = std::make_shared<SingleHeroPathfinderConfig>(paths, *cc, hero);
	cc->calculatePaths(config);

	const CGPathNode * pathNode = paths.getPathInfo(destination);
	if(!pathNode || !pathNode->reachable())
	{
		result.error = "Destination is not reachable";
		return result;
	}
	if(pathNode->turns != 0)
	{
		result.error = "Destination is not reachable this turn";
		return result;
	}
	if(pathNode->accessible == EPathAccessibility::NOT_SET || pathNode->accessible == EPathAccessibility::BLOCKED)
	{
		result.error = "Destination is blocked";
		return result;
	}

	CGPath path;
	if(!paths.getPath(path, destination, pathNode->layer))
	{
		result.error = "Could not reconstruct path to destination";
		return result;
	}

	result.destination = destination;
	result.layer = pathNode->layer;
	result.transit = pathNode->layer == EPathfindingLayer::AIR || pathNode->layer == EPathfindingLayer::WATER;
	result.stopAfterMove = pathNode->isTeleportAction();
	result.routeID = makeRouteId(hero->id.getNum(), hero->visitablePos(), destination, pathNode->layer, pathNode->moveRemains);
	if(expectedRouteID && *expectedRouteID != result.routeID)
	{
		result.error = "route_id is stale for the hero current position or destination";
		return result;
	}

	EPathfindingLayer currentLayer = pathNode->layer;
	for(auto it = path.nodes.rbegin(); it != path.nodes.rend(); ++it)
	{
		const CGPathNode & node = *it;
		if(node.coord == hero->visitablePos())
			continue;
		if(node.isTeleportAction())
			break;
		if(node.turns != 0)
			break;
		if(node.layer != currentLayer)
			break;

		result.requestPath.push_back(hero->convertFromVisitablePos(node.coord));

		const int3 guardingPosition = cc->guardingCreaturePosition(node.coord);
		if(guardingPosition.isValid())
			break;
		if(!cc->getVisitableObjs(node.coord).empty())
			break;
	}

	if(result.requestPath.empty())
	{
		result.error = "Path has no executable movement steps";
		return result;
	}

	result.ok = true;
	return result;
}

void CScriptedAdventureAI::loadConfig()
{
	try
	{
		const JsonPath path = JsonPath::builtin("config/ai/scriptedAdventure.json");
		auto * loader = CResourceHandler::get("core");
		if(!loader || !loader->existsResource(path))
		{
			logAi->info("ScriptedAdventureAI config not found, using defaults.");
		}
		else
		{
			const JsonNode config(path);
			applyConfig(config, "global");

			const JsonNode & players = config["players"];
			if(players.isStruct())
			{
				const std::vector<std::string> keys = {
					playerID.toString(),
					toLowerAscii(playerID.toString()),
					std::to_string(playerID.getNum())
				};

				for(const std::string & key : keys)
				{
					if(hasField(players, key) && players[key].isStruct())
					{
						applyConfig(players[key], "player " + key);
						break;
					}
				}
			}
		}

		const std::vector<std::string> scriptOverrideNames = {
			"VCMI_SCRIPTED_ADVENTURE_" + toUpperAscii(playerID.toString()) + "_SCRIPT",
			"VCMI_SCRIPTED_ADVENTURE_PLAYER_" + std::to_string(playerID.getNum()) + "_SCRIPT",
			"VCMI_SCRIPTED_ADVENTURE_SCRIPT"
		};
		for(const std::string & envName : scriptOverrideNames)
		{
			if(const auto script = readEnvironmentString(envName.c_str()))
			{
				scriptPath = normalizeScriptPath(*script);
				cachedScriptSource.reset();
				logAi->info("ScriptedAdventureAI applied script override from %s.", envName.c_str());
				break;
			}
		}

		if(const auto trace = readEnvironmentBool("VCMI_SCRIPTED_ADVENTURE_TRACE"))
		{
			scriptConfig.trace = *trace;
			logAi->info("ScriptedAdventureAI applied trace override from VCMI_SCRIPTED_ADVENTURE_TRACE.");
		}
		else if(readEnvironmentString("VCMI_SCRIPTED_ADVENTURE_TRACE"))
		{
			logAi->warn("ScriptedAdventureAI ignored invalid VCMI_SCRIPTED_ADVENTURE_TRACE value.");
		}

		logAi->info(
			"ScriptedAdventureAI config loaded: script '%s', reload per turn %d, trace %d, max calls %d, max actions %d, max memory bytes %d, max failures %d, disable turns %d",
			scriptPath.c_str(),
			scriptConfig.reloadScriptEachTurn,
			scriptConfig.trace,
			static_cast<int>(maxScriptCallsPerTurn),
			static_cast<int>(limits.maxActions),
			static_cast<int>(limits.maxMemoryBytes),
			static_cast<int>(scriptConfig.maxConsecutiveFailures),
			scriptConfig.disableTurnsAfterFailures);
	}
	catch(const std::exception & e)
	{
		logAi->warn("ScriptedAdventureAI config failed to load, using defaults: %s", e.what());
	}
}

void CScriptedAdventureAI::applyConfig(const JsonNode & config, const std::string & sourceLabel)
{
	if(!config.isStruct())
		throw std::invalid_argument("ScriptedAdventureAI config section must be an object: " + sourceLabel);

	if(hasField(config, "script"))
	{
		scriptPath = normalizeScriptPath(readString(config, "script"));
		cachedScriptSource.reset();
	}

	scriptConfig.reloadScriptEachTurn = readBool(config, "reloadScriptEachTurn", scriptConfig.reloadScriptEachTurn);
	scriptConfig.trace = readBool(config, "trace", scriptConfig.trace);
	maxScriptCallsPerTurn = readSize(config, "maxScriptCallsPerTurn", maxScriptCallsPerTurn, 1, 64);
	limits.maxActions = readSize(config, "maxActionsPerPlan", limits.maxActions, 1, 256);
	limits.maxMemoryBytes = readSize(config, "maxMemoryBytes", limits.maxMemoryBytes, 1024, 4 * 1024 * 1024);
	maxScriptUpdateJournal = readSize(config, "maxUpdateEvents", maxScriptUpdateJournal, 16, 4096);
	scriptConfig.maxConsecutiveFailures = readSize(config, "maxConsecutiveFailures", scriptConfig.maxConsecutiveFailures, 1, 100);
	scriptConfig.disableTurnsAfterFailures = static_cast<int>(readSize(config, "disableTurnsAfterFailures", scriptConfig.disableTurnsAfterFailures, 1, 100));

	logAi->debug("ScriptedAdventureAI applied %s config section.", sourceLabel.c_str());
}

JsonNode CScriptedAdventureAI::makeScriptMemoryLocalState() const
{
	JsonNode state;
	state["version"] = JsonNode(1);
	state["script"] = JsonNode(scriptPath);
	state["memory"] = scriptMemory;
	return state;
}

void CScriptedAdventureAI::loadScriptMemoryFromLocalState()
{
	try
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		const PlayerState * playerState = cc ? cc->getPlayerState(playerID, false) : nullptr;
		if(!playerState || !playerState->playerLocalSettings)
			return;

		const JsonNode & storedState = (*playerState->playerLocalSettings)[SCRIPT_MEMORY_LOCAL_STATE_KEY];
		if(!storedState.isStruct())
			return;

		if(!storedState["version"].isNumber()
			|| storedState["version"].getType() != JsonNode::JsonType::DATA_INTEGER
			|| storedState["version"].Integer() != 1)
		{
			logAi->warn("ScriptedAdventureAI ignored local memory with unsupported storage version.");
			return;
		}
		if(!storedState["script"].isString() || storedState["script"].String() != scriptPath)
		{
			logAi->info("ScriptedAdventureAI ignored local memory for a different script.");
			return;
		}

		JsonNode storedMemory = storedState["memory"];
		if(storedMemory.toCompactString().size() > limits.maxMemoryBytes)
		{
			logAi->warn("ScriptedAdventureAI ignored local memory exceeding configured size limit.");
			return;
		}

		scriptMemory = storedMemory;
		lastPersistedScriptState = makeScriptMemoryLocalState().toCompactString();
		logAi->info("ScriptedAdventureAI restored script memory from player local state.");
	}
	catch(const std::exception & e)
	{
		logAi->warn("ScriptedAdventureAI failed to restore local script memory: %s", e.what());
	}
}

void CScriptedAdventureAI::saveScriptMemoryToLocalState()
{
	try
	{
		const JsonNode persistedState = makeScriptMemoryLocalState();
		const std::string compactState = persistedState.toCompactString();
		if(compactState == lastPersistedScriptState)
			return;
		if(scriptMemory.toCompactString().size() > limits.maxMemoryBytes)
		{
			logAi->warn("ScriptedAdventureAI skipped local memory persistence because memory exceeds configured size limit.");
			return;
		}

		JsonNode localState;
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			const PlayerState * playerState = cc ? cc->getPlayerState(playerID, false) : nullptr;
			if(playerState && playerState->playerLocalSettings && playerState->playerLocalSettings->isStruct())
				localState = *playerState->playerLocalSettings;
		}
		if(!localState.isStruct())
			localState.Struct();

		localState[SCRIPT_MEMORY_LOCAL_STATE_KEY] = persistedState;
		ScopedCallbackWaitMode nonBlockingCallback(cc, false);
		const RequestWaitResult request = submitAndWaitForRequest(typeid(SaveLocalState), CTypeList::getInstance().getTypeID<SaveLocalState>(nullptr), [&]
		{
			cc->saveLocalState(localState);
		});
		if(request.applied)
		{
			lastPersistedScriptState = compactState;
			logAi->debug("ScriptedAdventureAI persisted script memory to player local state.");
		}
		else
			logAi->warn("ScriptedAdventureAI local memory persistence was not applied by server.");
	}
	catch(const std::exception & e)
	{
		logAi->warn("ScriptedAdventureAI failed to persist local script memory: %s", e.what());
	}
}

std::optional<std::string> CScriptedAdventureAI::getScriptSource()
{
	if(scriptConfig.reloadScriptEachTurn || !cachedScriptSource)
		cachedScriptSource = loadScriptSource();
	return cachedScriptSource;
}

std::optional<std::string> CScriptedAdventureAI::loadScriptSource() const
{
	if(const auto filePath = externalScriptFilePath(scriptPath))
	{
		std::ifstream stream(*filePath, std::ios::binary);
		if(!stream)
			return std::nullopt;

		std::ostringstream buffer;
		buffer << stream.rdbuf();
		return buffer.str();
	}

	const ScriptPath path = ScriptPath::builtinTODO(scriptPath).addPrefix("SCRIPTS/");
	auto * loader = CResourceHandler::get("core");
	if(!loader || !loader->existsResource(path))
		return std::nullopt;

	auto rawData = loader->load(path)->readAll();
	return std::string(reinterpret_cast<char *>(rawData.first.get()), rawData.second);
}

std::unique_ptr<scripting::LuaAdventureScriptRunner> CScriptedAdventureAI::makeRunner(const std::string & source) const
{
	const std::string sourceName = externalScriptFilePath(scriptPath) ? scriptPath : "core:" + scriptPath;
	return std::make_unique<scripting::LuaAdventureScriptRunner>(sourceName, source, limits);
}

bool CScriptedAdventureAI::isOpponent(const PlayerColor & owner) const
{
	return cc && owner.isValidPlayer() && cc->getPlayerRelations(owner, playerID) == PlayerRelations::ENEMIES;
}

void CScriptedAdventureAI::appendScriptUpdate(const std::string & type, JsonNode data, bool opponent)
{
	std::lock_guard guard(scriptUpdateMutex);

	JsonNode event;
	event["revision"] = JsonNode(static_cast<int64_t>(++scriptUpdateRevision));
	event["day"] = JsonNode(cc ? cc->getCalendar().getCurrentDay() : 0);
	event["type"] = JsonNode(type);
	event["opponent"] = JsonNode(opponent);
	event["data"] = data;

	scriptUpdateJournal.push_back(event);
	while(scriptUpdateJournal.size() > maxScriptUpdateJournal)
		scriptUpdateJournal.pop_front();
}

void CScriptedAdventureAI::writeTraceEvent(const std::string & label, const JsonNode & payload)
{
	try
	{
		const auto directory = VCMIDirs::get().userLogsPath() / "scriptedAdventureAI";
		boost::filesystem::create_directories(directory);
		const std::string filename = "player-" + playerID.toString()
			+ "-day-" + std::to_string(cc ? cc->getCalendar().getCurrentDay() : 0)
			+ "-event-" + std::to_string(traceSequence++)
			+ "-" + label + ".json";
		std::ofstream stream((directory / filename).string(), std::ios::out | std::ios::trunc);
		if(!stream)
		{
			logAi->warn("ScriptedAdventureAI could not open trace file %s", (directory / filename).string().c_str());
			return;
		}

		JsonNode trace;
		trace["label"] = JsonNode(label);
		trace["player"] = JsonNode(playerID.toString());
		trace["script"] = JsonNode(scriptPath);
		trace["payload"] = payload;
		trace.setModScope("", true);
		stream << trace.toCompactString();
	}
	catch(const std::exception & e)
	{
		logAi->warn("ScriptedAdventureAI trace write failed: %s", e.what());
	}
}

void CScriptedAdventureAI::fallbackToNullkiller(const std::string & reason)
{
	logAi->warn("ScriptedAdventureAI falling back to Nullkiller: %s", reason.c_str());
	if(failureRecordedThisTurn)
		return;

	failureRecordedThisTurn = true;
	++consecutiveScriptFailures;
	if(consecutiveScriptFailures >= scriptConfig.maxConsecutiveFailures)
	{
		disabledUntilDay = cc->getCalendar().getCurrentDay() + scriptConfig.disableTurnsAfterFailures;
		logAi->warn(
			"ScriptedAdventureAI disabled until day %d after %d consecutive failures.",
			disabledUntilDay,
			static_cast<int>(consecutiveScriptFailures));
		consecutiveScriptFailures = 0;
	}
}

}
