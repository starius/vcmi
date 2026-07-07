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
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../lib/pathfinder/CGPathNode.h"
#include "../../lib/pathfinder/PathfinderOptions.h"
#include "../../luascript/LuaAdventureScriptRunner.h"

#include <algorithm>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <tuple>

namespace ScriptedAdventureAI
{
namespace
{

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

int32_t readInteger(const JsonNode & node, const std::string & field)
{
	if(!node[field].isNumber() || node[field].getType() != JsonNode::JsonType::DATA_INTEGER)
		throw std::invalid_argument("Missing or non-integer script action field: " + field);
	return static_cast<int32_t>(node[field].Integer());
}

int32_t readInteger(const JsonNode & node, const std::string & field, int32_t defaultValue)
{
	if(!hasField(node, field))
		return defaultValue;
	return readInteger(node, field);
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

std::string jsonText(std::string value)
{
	for(char & character : value)
	{
		if(static_cast<unsigned char>(character) < 0x20)
			character = ' ';
	}
	return value;
}

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
	node["owner"] = JsonNode(object->tempOwner.toString());
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
	node["owner"] = JsonNode(hero->tempOwner.toString());
	node["level"] = JsonNode(static_cast<int32_t>(hero->level));
	node["mana"] = JsonNode(hero->mana);
	node["manaLimit"] = JsonNode(hero->manaLimit());
	node["movement"] = JsonNode(hero->movementPointsRemaining());
	node["movementLimit"] = JsonNode(hero->movementPointsLimit());
	node["primarySkills"]["attack"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::ATTACK));
	node["primarySkills"]["defense"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::DEFENSE));
	node["primarySkills"]["spellPower"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER));
	node["primarySkills"]["knowledge"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::KNOWLEDGE));
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
	node["owner"] = JsonNode(town->tempOwner.toString());
	node["fortLevel"] = JsonNode(static_cast<int32_t>(town->fortLevel()));
	node["visitingHeroId"] = jsonObjectId(town->getVisitingHero());
	node["garrisonHeroId"] = jsonObjectId(town->getGarrisonHero());
	node["army"] = jsonArmy(*town);
	node["buildings"].Vector();
	for(const BuildingID & building : town->getBuildings())
		node["buildings"].Vector().push_back(JsonNode(building.getNum()));
	node["recruitOptions"].Vector();
	const CArmedInstance * destination = town->getUpperArmy();
	for(int32_t level = 0; level < static_cast<int32_t>(town->creatures.size()); ++level)
	{
		JsonNode option = jsonRecruitOption(town, destination, level, resources);
		if(option.isStruct() && option["available"].Integer() > 0)
			node["recruitOptions"].Vector().push_back(option);
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

}

CScriptedAdventureAI::CScriptedAdventureAI()
	: scriptPath("ai/defaultAdventure.lua")
{
}

CScriptedAdventureAI::~CScriptedAdventureAI() = default;

void CScriptedAdventureAI::yourTurn(QueryID queryID)
{
	LOG_TRACE_PARAMS(logAi, "queryID '%i'", queryID);
	nullkiller->invalidatePathfinderData();
	status.addQuery(queryID, "YourTurn");
	executeActionAsync("yourTurn", [this, queryID](){ answerQuery(queryID, 0); });
	status.startedTurn();

	nullkiller->makingTurnInterruption.reset();

	asyncTasks->run([this]()
	{
		ScopedThreadName guard("ScriptedAdventureAI::makingTurn");
		status.waitTillFree();
		makeScriptedTurn();
	});
}

void CScriptedAdventureAI::makeScriptedTurn()
{
	try
	{
		if(!tryMakeScriptedTurn())
			AIGateway::makeTurn();
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
	const auto source = loadScriptSource();
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
		input.progress = progress;
		input.memory = scriptMemory;
		input.actionSpace = makeScriptActionSpace();
		input.analysis = makeScriptAnalysis();
		input.limits = makeScriptInputLimits();

		const AI::AdventureScriptOutput output = runner->planDay(input);
		scriptMemory = output.memory;

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
				progress = makeProgressJson(executed, failed, remaining);
				fallbackToNullkiller(e.what());
				return false;
			}

			if(actionResult["ok"].isBool() && !actionResult["ok"].Bool())
			{
				failed.Vector().push_back(actionResult);
				for(size_t remainingIndex = actionIndex + 1; remainingIndex < output.actions.size(); ++remainingIndex)
					remaining.Vector().push_back(output.actions[remainingIndex]);
				progress = makeProgressJson(executed, failed, remaining);
				fallbackToNullkiller(actionResult["error"].isString() ? actionResult["error"].String() : "script action failed");
				return false;
			}

			executed.Vector().push_back(actionResult);
			if(stopped || !status.haveTurn())
				break;
		}

		progress = makeProgressJson(executed, failed, remaining);

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

		buildStructure(town, buildingID);
		waitTillFree();
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["building_id"] = JsonNode(buildingID.getNum());
		actionResult["ok"] = JsonNode(true);
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

		int32_t amount = readInteger(action, "amount", static_cast<int32_t>(dwelling->creatures[level].first));
		amount = std::clamp<int32_t>(amount, 0, static_cast<int32_t>(dwelling->creatures[level].first));
		if(const CCreature * creature = creatureID.toCreature())
			amount = std::min<int32_t>(amount, cc->getResourceAmount() / creature->getFullRecruitCost());
		if(amount <= 0)
			throw std::invalid_argument("No recruitable or affordable creatures for this action");

		cc->recruitCreatures(dwelling, destination, creatureID, amount, level);
		waitTillFree();
		actionResult["source_id"] = JsonNode(dwelling->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["level"] = JsonNode(level);
		actionResult["creature_id"] = JsonNode(creatureID.getNum());
		actionResult["amount"] = JsonNode(amount);
		actionResult["ok"] = JsonNode(true);
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

		const bool moved = moveHeroToTile(route.destination, NK2AI::HeroPtr(hero, cc.get()));
		waitTillFree();
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["route_id"] = JsonNode(route.routeID);
		actionResult["destination"] = jsonPosition(route.destination);
		actionResult["ok"] = JsonNode(moved);
		if(!moved)
			actionResult["error"] = JsonNode("Hero movement did not reach requested destination");
		return moved && type != "visit_object" && !route.stopAfterMove;
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
		actionResult["ok"] = JsonNode(true);
		endTurn();
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

			JsonNode option;
			option["hero_id"] = JsonNode(hero->id.getNum());
			option["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
			option["path"] = jsonPathNode(*pathNode);
			option["route_id"] = JsonNode(routeID);
			option["planAction"]["type"] = JsonNode("move_hero");
			option["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
			option["planAction"]["x"] = JsonNode(position.x);
			option["planAction"]["y"] = JsonNode(position.y);
			option["planAction"]["z"] = JsonNode(position.z);
			option["planAction"]["route_id"] = JsonNode(routeID);

			movementCandidates.push_back(MovementCandidate{option, pathNode->cost, static_cast<int32_t>(pathNode->turns), position.x, position.y, position.z});

			const CGObjectInstance * topObject = cc->getTopObj(position);
			if(topObject && isObjectPathAction(pathNode->action) && seenTargetObjects.insert(topObject->id.getNum()).second)
			{
				JsonNode target;
				target["object"] = jsonMapObject(topObject, playerID, hero);
				target["hero_id"] = JsonNode(hero->id.getNum());
				target["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
				target["path"] = jsonPathNode(*pathNode);
				target["route_id"] = JsonNode(routeID);
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
	analysis["execution"]["validatesOwnership"] = JsonNode(true);
	analysis["execution"]["validatesVisibility"] = JsonNode(true);
	analysis["execution"]["validatesRouteIds"] = JsonNode(true);
	analysis["execution"]["replansAfterObjectVisit"] = JsonNode(true);
	analysis["execution"]["fallbackAI"] = JsonNode("Nullkiller2");
	return analysis;
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

	result.destination = destination;
	result.layer = pathNode->layer;
	result.stopAfterMove = pathNode->isTeleportAction();
	result.routeID = makeRouteId(hero->id.getNum(), hero->visitablePos(), destination, pathNode->layer, pathNode->moveRemains);
	if(expectedRouteID && *expectedRouteID != result.routeID)
	{
		result.error = "route_id is stale for the hero current position or destination";
		return result;
	}

	result.ok = true;
	return result;
}

std::optional<std::string> CScriptedAdventureAI::loadScriptSource() const
{
	const ScriptPath path = ScriptPath::builtinTODO(scriptPath).addPrefix("SCRIPTS/");
	auto * loader = CResourceHandler::get("core");
	if(!loader || !loader->existsResource(path))
		return std::nullopt;

	auto rawData = loader->load(path)->readAll();
	return std::string(reinterpret_cast<char *>(rawData.first.get()), rawData.second);
}

std::unique_ptr<scripting::LuaAdventureScriptRunner> CScriptedAdventureAI::makeRunner(const std::string & source) const
{
	return std::make_unique<scripting::LuaAdventureScriptRunner>("core:" + scriptPath, source, limits);
}

void CScriptedAdventureAI::fallbackToNullkiller(const std::string & reason)
{
	logAi->warn("ScriptedAdventureAI falling back to Nullkiller: %s", reason);
}

}
