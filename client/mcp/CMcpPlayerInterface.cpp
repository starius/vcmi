/*
 * CMcpPlayerInterface.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CMcpPlayerInterface.h"

#include "McpAdventurePlan.h"
#include "McpGameHelpers.h"
#include "McpHttpServer.h"

#include "../../lib/CPlayerState.h"
#include "../../lib/CStack.h"
#include "../../lib/RiverHandler.h"
#include "../../lib/RoadHandler.h"
#include "../../lib/ResourceSet.h"
#include "../../lib/TerrainHandler.h"
#include "../../lib/battle/BattleAction.h"
#include "../../lib/battle/BattleHexArray.h"
#include "../../lib/battle/CPlayerBattleCallback.h"
#include "../../lib/battle/IBattleState.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CTown.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/logging/CLogger.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/CGDwelling.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/mapping/TerrainTile.h"
#include "../../lib/networkPacks/Component.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../lib/pathfinder/CGPathNode.h"
#include "../../lib/pathfinder/PathfinderOptions.h"
#include "../../lib/texts/MetaString.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <tuple>

namespace
{

constexpr uint16_t DEFAULT_MCP_PORT = 3033;

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

int32_t readInteger(const JsonNode & node, const std::string & field)
{
	if(!node[field].isNumber() || node[field].getType() != JsonNode::JsonType::DATA_INTEGER)
		throw std::invalid_argument("Missing or non-integer argument: " + field);
	return static_cast<int32_t>(node[field].Integer());
}

bool readBool(const JsonNode & node, const std::string & field, bool defaultValue)
{
	if(!hasField(node, field))
		return defaultValue;
	if(!node[field].isBool())
		throw std::invalid_argument("Non-boolean argument: " + field);
	return node[field].Bool();
}

std::string readString(const JsonNode & node, const std::string & field)
{
	if(!node[field].isString())
		throw std::invalid_argument("Missing or non-string argument: " + field);
	return node[field].String();
}

std::optional<std::string> readOptionalString(const JsonNode & node, const std::string & field)
{
	if(!hasField(node, field))
		return std::nullopt;
	return readString(node, field);
}

int32_t readInteger(const JsonNode & node, const std::string & field, int32_t defaultValue)
{
	if(!hasField(node, field))
		return defaultValue;
	return readInteger(node, field);
}

int32_t readClampedInteger(const JsonNode & node, const std::string & field, int32_t defaultValue, int32_t minValue, int32_t maxValue)
{
	return std::clamp(readInteger(node, field, defaultValue), minValue, maxValue);
}

JsonNode makeObjectSchema(std::initializer_list<std::pair<const char *, const char *>> properties, std::initializer_list<const char *> required)
{
	JsonNode schema;
	schema["type"] = JsonNode("object");
	schema["additionalProperties"] = JsonNode(false);

	for(const auto & property : properties)
		schema["properties"][property.first]["type"] = JsonNode(property.second);
	for(const char * field : required)
		schema["required"].Vector().push_back(JsonNode(field));

	return schema;
}

void setRequired(JsonNode & schema, std::initializer_list<const char *> fields)
{
	for(const char * field : fields)
		schema["required"].Vector().push_back(JsonNode(field));
}

JsonNode makeReachableSchema()
{
	JsonNode schema = makeObjectSchema({
		{"hero_id", "integer"},
		{"radius", "integer"},
		{"max_options", "integer"},
		{"max_objects", "integer"},
		{"max_tiles", "integer"},
		{"max_recommended", "integer"},
		{"include_future_turns", "boolean"},
		{"include_tiles", "boolean"},
		{"include_paths", "boolean"},
		{"compact", "boolean"}
	}, {"hero_id"});
	schema["properties"]["compact"]["description"] = JsonNode("Defaults to true for vcmi.get_reachable. Compact output is intended for LLM planning.");
	schema["properties"]["include_tiles"]["description"] = JsonNode("Set true only when raw reachable tile options are needed.");
	schema["properties"]["include_paths"]["description"] = JsonNode("Set true only when full path previews are needed.");
	return schema;
}

JsonNode makeDayContextSchema()
{
	JsonNode schema = makeObjectSchema({
		{"radius", "integer"},
		{"max_options", "integer"},
		{"max_objects", "integer"},
		{"max_recommended", "integer"},
		{"include_paths", "boolean"},
		{"since_revision", "integer"},
		{"max_updates", "integer"}
	}, {});
	return schema;
}

JsonNode jsonPosition(const int3 & position)
{
	JsonNode node;
	node["x"] = JsonNode(position.x);
	node["y"] = JsonNode(position.y);
	node["z"] = JsonNode(position.z);
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

JsonNode jsonBattleHex(const BattleHex & hex)
{
	JsonNode node;
	node["id"] = JsonNode(hex.toInt());
	if(hex.isValid())
	{
		node["x"] = JsonNode(hex.getX());
		node["y"] = JsonNode(hex.getY());
	}
	return node;
}

JsonNode jsonResources(const ResourceSet & resources)
{
	JsonNode node;
	for(size_t i = 0; i < GameConstants::RESOURCE_QUANTITY; ++i)
		node[GameConstants::RESOURCE_NAMES[i]] = JsonNode(resources[i]);
	return node;
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

JsonNode jsonRecruitOption(const CGDwelling * dwelling, const CArmedInstance * destination, int32_t level)
{
	JsonNode node;
	if(level < 0 || level >= static_cast<int32_t>(dwelling->creatures.size()))
		return node;
	if(dwelling->creatures[level].second.empty())
		return node;

	const CreatureID creatureID = dwelling->creatures[level].second.back();
	const CCreature * creature = creatureID.toCreature();
	const int32_t available = static_cast<int32_t>(dwelling->creatures[level].first);
	node["source_id"] = JsonNode(dwelling->id.getNum());
	node["destination_id"] = JsonNode(destination ? destination->id.getNum() : dwelling->id.getNum());
	node["level"] = JsonNode(level);
	node["creature_id"] = JsonNode(creatureID.getNum());
	node["creature"] = JsonNode(creature ? jsonText(creature->getNameSingularTranslated()) : "");
	node["available"] = JsonNode(available);
	if(creature)
		node["cost"] = jsonResources(creature->getFullRecruitCost());
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

JsonNode jsonObjectId(const CGObjectInstance * object)
{
	JsonNode node;
	if(object)
		node = JsonNode(object->id.getNum());
	return node;
}

JsonNode jsonTown(const CGTownInstance * town)
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
		JsonNode option = jsonRecruitOption(town, destination, level);
		if(option.isStruct() && option["available"].Integer() > 0)
			node["recruitOptions"].Vector().push_back(option);
	}

	return node;
}

std::string battleSideName(BattleSide side)
{
	switch(side)
	{
		case BattleSide::ATTACKER:
			return "attacker";
		case BattleSide::DEFENDER:
			return "defender";
		case BattleSide::NONE:
			return "none";
		case BattleSide::INVALID:
			return "invalid";
		case BattleSide::ALL_KNOWING:
			return "all_knowing";
	}
	return "unknown";
}

std::string pathAccessibilityName(EPathAccessibility accessibility)
{
	switch(accessibility)
	{
		case EPathAccessibility::NOT_SET:
			return "not_set";
		case EPathAccessibility::ACCESSIBLE:
			return "accessible";
		case EPathAccessibility::VISITABLE:
			return "visitable";
		case EPathAccessibility::GUARDED:
			return "guarded";
		case EPathAccessibility::BLOCKVIS:
			return "blocking_visitable";
		case EPathAccessibility::FLYABLE:
			return "flyable";
		case EPathAccessibility::BLOCKED:
			return "blocked";
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

JsonNode jsonPathLayer(const EPathfindingLayer & layer)
{
	JsonNode node;
	node["id"] = JsonNode(layer.getNum());
	if(layer >= EPathfindingLayer::LAND && layer < EPathfindingLayer::NUM_LAYERS)
		node["name"] = JsonNode(NPathfindingLayer::names[layer.getNum()]);
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
	node["anchorPosition"] = jsonPosition(object->anchorPos());
	node["topVisiblePosition"] = jsonPosition(object->getTopVisiblePos());
	node["isVisitable"] = JsonNode(object->isVisitable());
	node["isBlockedVisitable"] = JsonNode(object->isBlockedVisitable());
	node["isRemovable"] = JsonNode(object->isRemovable());
	node["passableForPlayer"] = JsonNode(object->passableFor(player));
	return node;
}

JsonNode jsonCompactMapObject(const CGObjectInstance * object, PlayerColor player, const CGHeroInstance * contextHero)
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

void addVisibleObjectFromId(JsonNode & objects, std::set<int32_t> & seenObjects, const CCallback & callback, ObjectInstanceID id, PlayerColor player, const CGHeroInstance * contextHero)
{
	const CGObjectInstance * object = callback.getObj(id, false);
	if(!object)
		return;
	if(!seenObjects.insert(object->id.getNum()).second)
		return;
	objects.Vector().push_back(jsonMapObject(object, player, contextHero));
}

JsonNode jsonTerrainTile(const TerrainTile & tile, const int3 & position, const CCallback & callback, PlayerColor player)
{
	JsonNode node;
	node["position"] = jsonPosition(position);
	node["terrain"]["id"] = JsonNode(tile.getTerrainID().getNum());
	node["terrain"]["name"] = JsonNode(tile.getTerrain() ? jsonText(tile.getTerrain()->getNameTranslated()) : "");
	node["terrain"]["isLand"] = JsonNode(tile.isLand());
	node["terrain"]["isWater"] = JsonNode(tile.isWater());
	node["terrain"]["isPassable"] = JsonNode(tile.getTerrain() ? tile.getTerrain()->isPassable() : false);
	node["river"]["id"] = JsonNode(tile.getRiverID().getNum());
	node["river"]["name"] = JsonNode(tile.getRiver() ? jsonText(tile.getRiver()->getNameTranslated()) : "");
	node["road"]["id"] = JsonNode(tile.getRoadID().getNum());
	node["road"]["name"] = JsonNode(tile.getRoad() ? jsonText(tile.getRoad()->getNameTranslated()) : "");
	node["hasRiver"] = JsonNode(tile.hasRiver());
	node["hasRoad"] = JsonNode(tile.hasRoad());
	node["blocked"] = JsonNode(tile.blocked());
	node["visitable"] = JsonNode(tile.visitable());
	node["diggingStatus"] = JsonNode(static_cast<int32_t>(callback.getTileDigStatus(position, false)));

	node["visitableObjectIds"].Vector();
	for(const ObjectInstanceID & objectID : tile.visitableObjects)
	{
		const CGObjectInstance * object = callback.getObj(objectID, false);
		if(object)
			node["visitableObjectIds"].Vector().push_back(JsonNode(object->id.getNum()));
	}

	node["blockingObjectIds"].Vector();
	for(const ObjectInstanceID & objectID : tile.blockingObjects)
	{
		const CGObjectInstance * object = callback.getObj(objectID, false);
		if(object)
			node["blockingObjectIds"].Vector().push_back(JsonNode(object->id.getNum()));
	}

	const CGObjectInstance * topObject = callback.getTopObj(position);
	node["topObjectId"] = jsonObjectId(topObject);

	const int3 guardingPosition = callback.guardingCreaturePosition(position);
	if(guardingPosition.isValid() && callback.isInTheMap(guardingPosition) && callback.isVisibleFor(guardingPosition, player))
		node["guardingCreaturePosition"] = jsonPosition(guardingPosition);

	return node;
}

JsonNode jsonPathPreview(const CPathsInfo & paths, const int3 & destination, const EPathfindingLayer & layer, size_t maxNodes)
{
	JsonNode node;
	node["positions"].Vector();
	node["truncated"] = JsonNode(false);

	CGPath path;
	if(!paths.getPath(path, destination, layer))
		return node;

	size_t emitted = 0;
	for(auto it = path.nodes.rbegin(); it != path.nodes.rend(); ++it)
	{
		if(emitted >= maxNodes)
		{
			node["truncated"] = JsonNode(true);
			break;
		}
		node["positions"].Vector().push_back(jsonPosition(it->coord));
		++emitted;
	}
	return node;
}

JsonNode jsonPathNode(const CGPathNode & pathNode)
{
	JsonNode node;
	node["destination"] = jsonPosition(pathNode.coord);
	node["layer"] = jsonPathLayer(pathNode.layer);
	node["turns"] = JsonNode(static_cast<int32_t>(pathNode.turns));
	node["movementRemaining"] = JsonNode(pathNode.moveRemains);
	node["cost"].Float() = pathNode.cost;
	node["accessibility"] = JsonNode(pathAccessibilityName(pathNode.accessible));
	node["pathAction"] = JsonNode(pathActionName(pathNode.action));
	node["isTeleportAction"] = JsonNode(pathNode.isTeleportAction());
	return node;
}

JsonNode jsonCompactPathNode(const CGPathNode & pathNode)
{
	JsonNode node;
	node["destination"] = jsonPosition(pathNode.coord);
	node["layerId"] = JsonNode(pathNode.layer.getNum());
	node["turns"] = JsonNode(static_cast<int32_t>(pathNode.turns));
	node["movementRemaining"] = JsonNode(pathNode.moveRemains);
	node["cost"].Float() = pathNode.cost;
	node["pathAction"] = JsonNode(pathActionName(pathNode.action));
	node["accessibility"] = JsonNode(pathAccessibilityName(pathNode.accessible));
	node["isTeleportAction"] = JsonNode(pathNode.isTeleportAction());
	return node;
}

JsonNode jsonBattleStack(const CPlayerBattleCallback & battle, const CStack * stack)
{
	JsonNode node;
	node["id"] = JsonNode(static_cast<int32_t>(stack->unitId()));
	node["name"] = JsonNode(jsonText(stack->getName()));
	node["creatureId"] = JsonNode(stack->creatureId().getNum());
	node["side"] = JsonNode(battleSideName(stack->unitSide()));
	node["sideId"] = JsonNode(static_cast<int32_t>(stack->unitSide()));
	node["owner"] = JsonNode(battle.battleGetOwner(stack).toString());
	node["initialOwner"] = JsonNode(stack->unitOwner().toString());
	node["slot"] = JsonNode(stack->unitSlot().getNum());
	node["position"] = jsonBattleHex(stack->getPosition());
	node["count"] = JsonNode(stack->getCount());
	node["baseCount"] = JsonNode(stack->unitBaseAmount());
	node["killed"] = JsonNode(stack->getKilled());
	node["alive"] = JsonNode(stack->alive());
	node["doubleWide"] = JsonNode(stack->doubleWide());
	node["waiting"] = JsonNode(stack->waiting);
	node["waitedThisTurn"] = JsonNode(stack->waitedThisTurn);
	node["defending"] = JsonNode(stack->defending);
	node["canShoot"] = JsonNode(battle.battleCanShoot(stack));
	node["shots"]["available"] = JsonNode(stack->shots.available());
	node["shots"]["total"] = JsonNode(stack->shots.total());
	return node;
}

JsonNode jsonBattleHexList(const BattleHexArray & hexes)
{
	JsonNode node;
	node.Vector();
	for(const BattleHex & hex : hexes)
		node.Vector().push_back(jsonBattleHex(hex));
	return node;
}

JsonNode jsonBattleLegalActions(const CPlayerBattleCallback & battle, const CStack * activeStack)
{
	JsonNode node;
	node["activeStackId"] = JsonNode(static_cast<int32_t>(activeStack->unitId()));
	node["canWait"] = JsonNode(!activeStack->waitedThisTurn);
	node["canDefend"] = JsonNode(true);
	node["moveHexes"] = jsonBattleHexList(battle.battleGetAvailableHexes(activeStack, false));
	node["shootTargets"].Vector();
	node["meleeTargets"].Vector();

	const BattleHexArray availableHexes = battle.battleGetAvailableHexes(activeStack, false);
	for(const CStack * target : battle.battleGetStacks(CBattleInfoEssentials::ONLY_ENEMY, true))
	{
		if(!target)
			continue;

		if(battle.battleCanShoot(activeStack, target->getPosition()))
		{
			JsonNode action;
			action["target_stack_id"] = JsonNode(static_cast<int32_t>(target->unitId()));
			action["target"] = JsonNode(jsonText(target->getName()));
			action["target_position"] = jsonBattleHex(target->getPosition());
			node["shootTargets"].Vector().push_back(action);
		}

		if(!battle.battleCanAttackUnit(activeStack, target))
			continue;

		for(const BattleHex::EDir direction : BattleHex::hexagonalDirections())
		{
			if(!battle.battleCanAttackHex(availableHexes, activeStack, target->getPosition(), direction))
				continue;

			const BattleHex attackFrom = battle.fromWhichHexAttack(activeStack, target->getPosition(), direction);
			if(!attackFrom.isValid())
				continue;

			JsonNode action;
			action["target_stack_id"] = JsonNode(static_cast<int32_t>(target->unitId()));
			action["target"] = JsonNode(jsonText(target->getName()));
			action["target_position"] = jsonBattleHex(target->getPosition());
			action["attack_from_hex"] = jsonBattleHex(attackFrom);
			node["meleeTargets"].Vector().push_back(action);
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
	return node;
}

std::optional<uint16_t> readPortFromEnvironment()
{
	const char * portValue = std::getenv("VCMI_MCP_PORT");
	if(!portValue || !*portValue)
		return std::nullopt;

	try
	{
		int port = std::stoi(portValue);
		if(port < 0 || port > std::numeric_limits<uint16_t>::max())
			throw std::out_of_range("port");
		return static_cast<uint16_t>(port);
	}
	catch(const std::exception &)
	{
		logGlobal->warn("Ignoring invalid VCMI_MCP_PORT value '%s'", portValue);
		return std::nullopt;
	}
}

std::string readTokenFromEnvironment()
{
	const char * token = std::getenv("VCMI_MCP_TOKEN");
	return token ? token : "";
}

std::string readTracePathFromEnvironment(PlayerColor playerID)
{
	const char * tracePath = std::getenv("VCMI_MCP_TRACE");
	if(tracePath && *tracePath)
		return tracePath;

	const char * traceDir = std::getenv("VCMI_MCP_TRACE_DIR");
	if(traceDir && *traceDir)
	{
		std::string path(traceDir);
		if(path.back() != '/')
			path += '/';
		path += "vcmi-mcp-";
		path += playerID.toString();
		path += ".jsonl";
		return path;
	}

	return "";
}

std::string timestampUtc()
{
	const auto now = std::chrono::system_clock::now();
	const auto seconds = std::chrono::system_clock::to_time_t(now);
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

	std::tm utcTime {};
#ifdef _WIN32
	gmtime_s(&utcTime, &seconds);
#else
	gmtime_r(&seconds, &utcTime);
#endif

	std::ostringstream out;
	out << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%S");
	out << '.' << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
	return out.str();
}

void writeJsonString(std::ostream & out, const std::string & value)
{
	static constexpr char HEX_DIGITS[] = "0123456789abcdef";

	out << '"';
	for(unsigned char character : value)
	{
		switch(character)
		{
			case '"':
				out << "\\\"";
				break;
			case '\\':
				out << "\\\\";
				break;
			case '\b':
				out << "\\b";
				break;
			case '\f':
				out << "\\f";
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
				if(character < 0x20)
				{
					out << "\\u00";
					out << HEX_DIGITS[character >> 4];
					out << HEX_DIGITS[character & 0x0f];
				}
				else
				{
					out << character;
				}
				break;
		}
	}
	out << '"';
}

void writeJsonLineValue(std::ostream & out, const JsonNode & node)
{
	switch(node.getType())
	{
		case JsonNode::JsonType::DATA_NULL:
			out << "null";
			break;
		case JsonNode::JsonType::DATA_BOOL:
			out << (node.Bool() ? "true" : "false");
			break;
		case JsonNode::JsonType::DATA_FLOAT:
		{
			const double value = node.Float();
			if(std::isfinite(value))
				out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
			else
				out << "null";
			break;
		}
		case JsonNode::JsonType::DATA_STRING:
			writeJsonString(out, node.String());
			break;
		case JsonNode::JsonType::DATA_VECTOR:
		{
			out << '[';
			bool first = true;
			for(const JsonNode & child : node.Vector())
			{
				if(!first)
					out << ',';
				first = false;
				writeJsonLineValue(out, child);
			}
			out << ']';
			break;
		}
		case JsonNode::JsonType::DATA_STRUCT:
		{
			out << '{';
			bool first = true;
			for(const auto & [key, child] : node.Struct())
			{
				if(!first)
					out << ',';
				first = false;
				writeJsonString(out, key);
				out << ':';
				writeJsonLineValue(out, child);
			}
			out << '}';
			break;
		}
		case JsonNode::JsonType::DATA_INTEGER:
			out << node.Integer();
			break;
	}
}

std::string toJsonLine(const JsonNode & node)
{
	std::ostringstream out;
	writeJsonLineValue(out, node);
	return out.str();
}

JsonNode jsonAction(const std::string & tool, const std::string & description)
{
	JsonNode node;
	node["tool"] = JsonNode(tool);
	node["description"] = JsonNode(description);
	return node;
}

}

CMcpPlayerInterface::CMcpPlayerInterface() = default;

void CMcpPlayerInterface::initGameInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CCallback> CB)
{
	env = ENV;
	cb = CB;
	human = false;
	dllName = "McpAI";
	playerID = cb->getPlayerID().value_or(PlayerColor::CANNOT_DETERMINE);
	tracePath = readTracePathFromEnvironment(playerID);
	if(!tracePath.empty())
	{
		JsonNode event;
		event["event"] = JsonNode("mcp_interface_started");
		event["player"] = JsonNode(playerID.toString());
		appendTraceEvent(event);
		logGlobal->info("MCP trace for player %s will be written to %s", playerID.toString(), tracePath);
	}

	configureProtocol();
	startHttpServer();
}

CMcpPlayerInterface::~CMcpPlayerInterface() = default;

void CMcpPlayerInterface::finish()
{
	JsonNode event;
	event["event"] = JsonNode("mcp_interface_finished");
	event["player"] = JsonNode(playerID.toString());
	appendTraceEvent(event);
	httpServer.reset();
}

void CMcpPlayerInterface::configureProtocol()
{
	protocol.registerResource({
		"vcmi://state",
		"Visible VCMI state",
		"Current state visible to the MCP-controlled player.",
		"application/json",
		[this]
		{
			return makeStateText();
		}
	});

	protocol.registerResource({
		"vcmi://action-space",
		"Current VCMI action space",
		"Available MCP actions and current object ids for the controlled player.",
		"application/json",
		[this]
		{
			return makeActionSpaceJson().toCompactString();
		}
	});

	protocol.registerResource({
		"vcmi://battle-state",
		"Current VCMI battle state",
		"Battle state visible to the MCP-controlled player, if any battle is active.",
		"application/json",
		[this]
		{
			return makeBattleStateJson().toCompactString();
		}
	});

	protocol.registerResource({
		"vcmi://agent-guide",
		"VCMI MCP agent guide",
		"Short operating guide for an LLM controlling VCMI through MCP.",
		"text/plain",
		[this]
		{
			return makeAgentGuideText();
		}
	});

	protocol.registerTool({
		"vcmi.get_state",
		"Returns current visible player state as JSON, optionally filtered by selectors.",
		makeObjectSchema({{"select", "array"}, {"since_revision", "integer"}, {"max_updates", "integer"}}, {}),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_state", arguments, makeJsonResult(makeStateJson(arguments)));
		}
	});

	protocol.registerTool({
		"vcmi.get_updates",
		"Returns revisioned visible state updates since a previous revision.",
		makeObjectSchema({{"since_revision", "integer"}, {"max_updates", "integer"}}, {"since_revision"}),
		[this](const JsonNode & arguments)
		{
			const uint64_t sinceRevision = static_cast<uint64_t>(std::max<int32_t>(0, readInteger(arguments, "since_revision")));
			const size_t maxUpdates = static_cast<size_t>(readClampedInteger(arguments, "max_updates", 200, 1, 1000));
			return traceToolResult("vcmi.get_updates", arguments, makeJsonResult(makeUpdatesJson(sinceRevision, maxUpdates)));
		}
	});

	protocol.registerTool({
		"vcmi.get_action_space",
		"Returns current MCP action guidance, usable object ids, and currently relevant action candidates.",
		makeObjectSchema({}, {}),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_action_space", arguments, makeJsonResult(makeActionSpaceJson()));
		}
	});

	protocol.registerTool({
		"vcmi.get_day_context",
		"Returns compact whole-day planning context: selected state, build options, plan schema, and recommended reachable targets.",
		makeDayContextSchema(),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_day_context", arguments, makeJsonResult(makeDayContextJson(arguments)));
		}
	});

	protocol.registerTool({
		"vcmi.get_visible_map",
		"Returns visible adventure-map tiles and visible objects around a center or owned hero.",
		makeObjectSchema({{"hero_id", "integer"}, {"x", "integer"}, {"y", "integer"}, {"z", "integer"}, {"radius", "integer"}, {"full_visible", "boolean"}, {"include_tiles", "boolean"}, {"include_objects", "boolean"}, {"max_tiles", "integer"}}, {}),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_visible_map", arguments, makeJsonResult(makeVisibleMapJson(arguments)));
		}
	});

	protocol.registerTool({
		"vcmi.get_movement_options",
		"Returns pathfinder-derived visible movement destinations and object targets for an owned hero.",
		makeReachableSchema(),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_movement_options", arguments, makeJsonResult(makeMovementOptionsJson(arguments)));
		}
	});

	protocol.registerTool({
		"vcmi.get_reachable",
		"Returns compact this-day reachable objects, recommended targets, optional tiles, and route ids for an owned hero.",
		makeReachableSchema(),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_reachable", arguments, makeJsonResult(makeReachableJson(arguments)));
		}
	});

	protocol.registerTool({
		"vcmi.get_battle_state",
		"Returns visible battle state and legal actions for the currently active stack.",
		makeObjectSchema({}, {}),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_battle_state", arguments, makeJsonResult(makeBattleStateJson()));
		}
	});

	protocol.registerTool({
		"vcmi.end_turn",
		"Ends the current adventure-map turn.",
		makeObjectSchema({}, {}),
		[this](const JsonNode & arguments)
		{
			{
				std::lock_guard lock(interfaceMutex);
				if(!turnActive)
					return traceToolResult("vcmi.end_turn", arguments, makeToolError("No active turn to end"));
				turnActive = false;
			}
			RequestWaitResult request = submitAndWaitForRequest(typeid(EndTurn), [this]
			{
				cb->endTurn();
			});
			JsonNode result = jsonRequestWaitResult(request);
			result["message"] = JsonNode(request.applied ? "turn ended" : "end turn rejected by server");
			if(!request.applied)
			{
				std::lock_guard lock(interfaceMutex);
				turnActive = true;
			}
			return traceToolResult("vcmi.end_turn", arguments, request.applied ? makeJsonResult(result) : makeToolError(result.toCompactString()));
		}
	});

	protocol.registerTool({
		"vcmi.answer_query",
		"Answers the currently pending VCMI query/dialog.",
		makeObjectSchema({{"query_id", "integer"}, {"answer", "integer"}}, {"query_id"}),
		[this](const JsonNode & arguments)
		{
			QueryID queryID(readInteger(arguments, "query_id"));
			std::optional<int32_t> answer;
			if(hasField(arguments, "answer"))
				answer = readInteger(arguments, "answer");

			int requestID = cb->sendQueryReply(answer, queryID);
			clearPendingQuery(queryID);

			JsonNode result;
			result["ok"] = JsonNode(true);
			result["requestId"] = JsonNode(requestID);
			return traceToolResult("vcmi.answer_query", arguments, makeJsonResult(result));
		}
	});

	protocol.registerTool({
		"vcmi.move_hero",
		"Moves an owned hero to an adventure-map tile using a pathfinder-generated route.",
		makeObjectSchema({{"hero_id", "integer"}, {"x", "integer"}, {"y", "integer"}, {"z", "integer"}, {"route_id", "string"}}, {"hero_id", "x", "y"}),
		[this](const JsonNode & arguments)
		{
			std::lock_guard actionLock(actionMutex);
			const CGHeroInstance * hero = cb->getHero(ObjectInstanceID(readInteger(arguments, "hero_id")));
			if(!hero)
				return traceToolResult("vcmi.move_hero", arguments, makeToolError("Unknown hero id"));
			if(hero->tempOwner != playerID)
				return traceToolResult("vcmi.move_hero", arguments, makeToolError("Hero is not owned by this MCP player"));

			int z = hasField(arguments, "z") ? readInteger(arguments, "z") : hero->visitablePos().z;
			RoutePlan route = makeRoutePlan(hero, int3(readInteger(arguments, "x"), readInteger(arguments, "y"), z), readOptionalString(arguments, "route_id"));
			if(!route.ok)
				return traceToolResult("vcmi.move_hero", arguments, makeToolError(route.error));

			RequestWaitResult request = submitAndWaitForRequest(typeid(MoveHero), [this, hero, route]
			{
				cb->moveHero(hero, route.requestPath, route.transit, route.layer);
			});

			JsonNode result = jsonRequestWaitResult(request);
			result["message"] = JsonNode(request.applied ? "move applied" : "move rejected by server");
			result["route_id"] = JsonNode(route.routeID);
			result["destination"] = jsonPosition(route.destination);
			result["submittedPath"] = jsonPositions(route.requestPath);
			if(const CGHeroInstance * updatedHero = cb->getHero(hero->id))
				result["hero"] = jsonHero(updatedHero);

			return traceToolResult("vcmi.move_hero", arguments, request.applied ? makeJsonResult(result) : makeToolError(result.toCompactString()));
		}
	});

	protocol.registerTool({
		"vcmi.move_hero_to_object",
		"Moves an owned hero to a visible reachable object target returned by vcmi.get_reachable.",
		makeObjectSchema({{"hero_id", "integer"}, {"object_id", "integer"}, {"route_id", "string"}}, {"hero_id", "object_id"}),
		[this](const JsonNode & arguments)
		{
			std::lock_guard actionLock(actionMutex);
			const CGHeroInstance * hero = cb->getHero(ObjectInstanceID(readInteger(arguments, "hero_id")));
			if(!hero)
				return traceToolResult("vcmi.move_hero_to_object", arguments, makeToolError("Unknown hero id"));
			if(hero->tempOwner != playerID)
				return traceToolResult("vcmi.move_hero_to_object", arguments, makeToolError("Hero is not owned by this MCP player"));

			const CGObjectInstance * object = cb->getObj(ObjectInstanceID(readInteger(arguments, "object_id")), false);
			if(!object)
				return traceToolResult("vcmi.move_hero_to_object", arguments, makeToolError("Unknown or not visible object id"));

			const int3 destination = object->visitablePos();
			RoutePlan route = makeRoutePlan(hero, destination, readOptionalString(arguments, "route_id"));
			if(!route.ok)
				return traceToolResult("vcmi.move_hero_to_object", arguments, makeToolError(route.error));

			RequestWaitResult request = submitAndWaitForRequest(typeid(MoveHero), [this, hero, route]
			{
				cb->moveHero(hero, route.requestPath, route.transit, route.layer);
			});

			JsonNode result = jsonRequestWaitResult(request);
			result["message"] = JsonNode(request.applied ? "move to object applied" : "move to object rejected by server");
			result["object_id"] = JsonNode(object->id.getNum());
			result["destination"] = jsonPosition(destination);
			result["route_id"] = JsonNode(route.routeID);
			result["layer"] = jsonPathLayer(route.layer);
			result["submittedPath"] = jsonPositions(route.requestPath);
			if(const CGHeroInstance * updatedHero = cb->getHero(hero->id))
				result["hero"] = jsonHero(updatedHero);
			return traceToolResult("vcmi.move_hero_to_object", arguments, request.applied ? makeJsonResult(result) : makeToolError(result.toCompactString()));
		}
	});

	protocol.registerTool({
		"vcmi.build_town_building",
		"Requests construction of a building in an owned town.",
		makeObjectSchema({{"town_id", "integer"}, {"building_id", "integer"}}, {"town_id", "building_id"}),
		[this](const JsonNode & arguments)
		{
			std::lock_guard actionLock(actionMutex);
			const CGTownInstance * town = cb->getTown(ObjectInstanceID(readInteger(arguments, "town_id")));
			if(!town)
				return traceToolResult("vcmi.build_town_building", arguments, makeToolError("Unknown town id"));
			if(town->tempOwner != playerID)
				return traceToolResult("vcmi.build_town_building", arguments, makeToolError("Town is not owned by this MCP player"));

			const BuildingID buildingID(readInteger(arguments, "building_id"));
			bool accepted = false;
			RequestWaitResult request = submitAndWaitForRequest(typeid(BuildStructure), [&]
			{
				accepted = cb->buildBuilding(town, buildingID);
			});
			if(!accepted)
				return traceToolResult("vcmi.build_town_building", arguments, makeToolError("Build request was rejected by the client callback"));

			JsonNode result = jsonRequestWaitResult(request);
			result["message"] = JsonNode(request.applied ? "build applied" : "build rejected by server");
			result["town_id"] = JsonNode(town->id.getNum());
			result["building_id"] = JsonNode(buildingID.getNum());
			return traceToolResult("vcmi.build_town_building", arguments, request.applied ? makeJsonResult(result) : makeToolError(result.toCompactString()));
		}
	});

	protocol.registerTool({
		"vcmi.execute_plan",
		"Executes a sequential batch of day-plan actions until completion or a stop condition. Canonical action types: build, recruit, move_hero, visit_object, answer_query, end_turn.",
		Mcp::makeExecutePlanSchema(),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.execute_plan", arguments, executePlan(arguments));
		}
	});

	protocol.registerTool({
		"vcmi.battle_defend",
		"Defends with the currently active battle stack.",
		makeObjectSchema({}, {}),
		[this](const JsonNode & arguments)
		{
			BattleID battleID = BattleID::NONE;
			const CStack * stack = nullptr;
			{
				std::lock_guard lock(interfaceMutex);
				battleID = activeBattleID;
				stack = activeStackToMove;
				activeBattleID = BattleID::NONE;
				activeStackToMove = nullptr;
			}

			if(!stack)
				return traceToolResult("vcmi.battle_defend", arguments, makeToolError("No active battle stack"));

			cb->battleMakeUnitAction(battleID, BattleAction::makeDefend(stack));
			return traceToolResult("vcmi.battle_defend", arguments, makeOkResult("battle defend requested"));
		}
	});

	protocol.registerTool({
		"vcmi.battle_wait",
		"Waits with the currently active battle stack.",
		makeObjectSchema({}, {}),
		[this](const JsonNode & arguments)
		{
			BattleID battleID = BattleID::NONE;
			const CStack * stack = nullptr;
			{
				std::lock_guard lock(interfaceMutex);
				battleID = activeBattleID;
				stack = activeStackToMove;
				activeBattleID = BattleID::NONE;
				activeStackToMove = nullptr;
			}

			if(!stack)
				return traceToolResult("vcmi.battle_wait", arguments, makeToolError("No active battle stack"));

			cb->battleMakeUnitAction(battleID, BattleAction::makeWait(stack));
			return traceToolResult("vcmi.battle_wait", arguments, makeOkResult("battle wait requested"));
		}
	});

	protocol.registerTool({
		"vcmi.battle_move",
		"Moves the currently active battle stack to a legal battlefield hex.",
		makeObjectSchema({{"hex", "integer"}}, {"hex"}),
		[this](const JsonNode & arguments)
		{
			BattleID battleID = BattleID::NONE;
			const CStack * stack = nullptr;
			{
				std::lock_guard lock(interfaceMutex);
				battleID = activeBattleID;
				stack = activeStackToMove;
			}
			if(!stack)
				return traceToolResult("vcmi.battle_move", arguments, makeToolError("No active battle stack"));

			auto battle = cb->getBattle(battleID);
			const BattleHex destination(static_cast<si16>(readInteger(arguments, "hex")));
			if(!destination.isValid())
				return traceToolResult("vcmi.battle_move", arguments, makeToolError("Invalid battle hex"));
			if(!battle->battleGetAvailableHexes(stack, false).contains(destination))
				return traceToolResult("vcmi.battle_move", arguments, makeToolError("Battle hex is not reachable by active stack"));

			{
				std::lock_guard lock(interfaceMutex);
				if(activeBattleID == battleID && activeStackToMove == stack)
				{
					activeBattleID = BattleID::NONE;
					activeStackToMove = nullptr;
				}
			}
			cb->battleMakeUnitAction(battleID, BattleAction::makeMove(stack, destination));
			return traceToolResult("vcmi.battle_move", arguments, makeOkResult("battle move requested"));
		}
	});

	protocol.registerTool({
		"vcmi.battle_shoot",
		"Shoots a target stack with the currently active battle stack.",
		makeObjectSchema({{"target_stack_id", "integer"}}, {"target_stack_id"}),
		[this](const JsonNode & arguments)
		{
			BattleID battleID = BattleID::NONE;
			const CStack * stack = nullptr;
			{
				std::lock_guard lock(interfaceMutex);
				battleID = activeBattleID;
				stack = activeStackToMove;
			}
			if(!stack)
				return traceToolResult("vcmi.battle_shoot", arguments, makeToolError("No active battle stack"));

			auto battle = cb->getBattle(battleID);
			const CStack * target = battle->battleGetStackByID(readInteger(arguments, "target_stack_id"));
			if(!target)
				return traceToolResult("vcmi.battle_shoot", arguments, makeToolError("Unknown target stack id"));
			if(!battle->battleCanShoot(stack, target->getPosition()))
				return traceToolResult("vcmi.battle_shoot", arguments, makeToolError("Active stack cannot shoot this target"));

			{
				std::lock_guard lock(interfaceMutex);
				if(activeBattleID == battleID && activeStackToMove == stack)
				{
					activeBattleID = BattleID::NONE;
					activeStackToMove = nullptr;
				}
			}
			cb->battleMakeUnitAction(battleID, BattleAction::makeShotAttack(stack, target));
			return traceToolResult("vcmi.battle_shoot", arguments, makeOkResult("battle shot requested"));
		}
	});

	protocol.registerTool({
		"vcmi.battle_melee_attack",
		"Performs a melee attack with the currently active battle stack.",
		makeObjectSchema({{"target_stack_id", "integer"}, {"attack_from_hex", "integer"}}, {"target_stack_id"}),
		[this](const JsonNode & arguments)
		{
			BattleID battleID = BattleID::NONE;
			const CStack * stack = nullptr;
			{
				std::lock_guard lock(interfaceMutex);
				battleID = activeBattleID;
				stack = activeStackToMove;
			}
			if(!stack)
				return traceToolResult("vcmi.battle_melee_attack", arguments, makeToolError("No active battle stack"));

			auto battle = cb->getBattle(battleID);
			const CStack * target = battle->battleGetStackByID(readInteger(arguments, "target_stack_id"));
			if(!target)
				return traceToolResult("vcmi.battle_melee_attack", arguments, makeToolError("Unknown target stack id"));
			if(!battle->battleCanAttackUnit(stack, target))
				return traceToolResult("vcmi.battle_melee_attack", arguments, makeToolError("Active stack cannot attack this target"));

			BattleHex attackFrom;
			const BattleHexArray availableHexes = battle->battleGetAvailableHexes(stack, false);
			auto findLegalAttackFrom = [&](std::optional<BattleHex> requested) -> std::optional<BattleHex>
			{
				for(const BattleHex::EDir direction : BattleHex::hexagonalDirections())
				{
					if(!battle->battleCanAttackHex(availableHexes, stack, target->getPosition(), direction))
						continue;

					const BattleHex candidate = battle->fromWhichHexAttack(stack, target->getPosition(), direction);
					if(!candidate.isValid())
						continue;
					if(!requested || candidate == *requested)
						return candidate;
				}
				return std::nullopt;
			};

			if(hasField(arguments, "attack_from_hex"))
			{
				attackFrom = BattleHex(static_cast<si16>(readInteger(arguments, "attack_from_hex")));
				if(!attackFrom.isValid())
					return traceToolResult("vcmi.battle_melee_attack", arguments, makeToolError("Invalid attack_from_hex"));
				auto legalAttackFrom = findLegalAttackFrom(attackFrom);
				if(!legalAttackFrom)
					return traceToolResult("vcmi.battle_melee_attack", arguments, makeToolError("attack_from_hex is not legal for this target"));
				attackFrom = *legalAttackFrom;
			}
			else
			{
				auto legalAttackFrom = findLegalAttackFrom(std::nullopt);
				if(legalAttackFrom)
					attackFrom = *legalAttackFrom;
			}

			if(!attackFrom.isValid())
				return traceToolResult("vcmi.battle_melee_attack", arguments, makeToolError("Target is not reachable for melee attack"));

			{
				std::lock_guard lock(interfaceMutex);
				if(activeBattleID == battleID && activeStackToMove == stack)
				{
					activeBattleID = BattleID::NONE;
					activeStackToMove = nullptr;
				}
			}
			cb->battleMakeUnitAction(battleID, BattleAction::makeMeleeAttack(stack, target, attackFrom));
			return traceToolResult("vcmi.battle_melee_attack", arguments, makeOkResult("battle melee attack requested"));
		}
	});

	protocol.registerTool({
		"vcmi.battle_end_tactics",
		"Ends the current tactics phase.",
		makeObjectSchema({}, {}),
		[this](const JsonNode & arguments)
		{
			BattleID battleID = BattleID::NONE;
			{
				std::lock_guard lock(interfaceMutex);
				battleID = activeTacticsBattleID;
				activeTacticsBattleID = BattleID::NONE;
			}

			if(battleID == BattleID::NONE)
				return traceToolResult("vcmi.battle_end_tactics", arguments, makeToolError("No active tactics phase"));

			cb->battleMakeTacticAction(battleID, BattleAction::makeEndOFTacticPhase(cb->getBattle(battleID)->battleGetTacticsSide()));
			return traceToolResult("vcmi.battle_end_tactics", arguments, makeOkResult("tactics phase ended"));
		}
	});
}

void CMcpPlayerInterface::startHttpServer()
{
	const auto configuredPort = readPortFromEnvironment();
	const uint16_t basePort = configuredPort.value_or(static_cast<uint16_t>(DEFAULT_MCP_PORT + std::max(0, playerID.getNum())));
	const int attempts = configuredPort ? 1 : PlayerColor::PLAYER_LIMIT_I;
	const std::string authToken = readTokenFromEnvironment();

	for(int attempt = 0; attempt < attempts; ++attempt)
	{
		uint16_t portToTry = static_cast<uint16_t>(basePort + attempt);
		try
		{
			httpServer = std::make_unique<McpHttpServer>(protocol, portToTry, authToken);
			httpServer->start();
			logGlobal->info("MCP interface for player %s listening on http://127.0.0.1:%d/mcp", playerID.toString(), httpServer->getPort());
			JsonNode event;
			event["event"] = JsonNode("mcp_http_started");
			event["player"] = JsonNode(playerID.toString());
			event["url"] = JsonNode("http://127.0.0.1:" + std::to_string(httpServer->getPort()) + "/mcp");
			event["authRequired"] = JsonNode(!authToken.empty());
			appendTraceEvent(event);
			return;
		}
		catch(const std::exception & exception)
		{
			httpServer.reset();
			logGlobal->warn("Failed to start MCP HTTP server for player %s on port %d: %s", playerID.toString(), portToTry, exception.what());
		}
	}
}

CMcpPlayerInterface::RequestWaitResult CMcpPlayerInterface::submitAndWaitForRequest(const std::type_info & requestType, const std::function<void()> & submit)
{
	PendingRequest request;
	request.token = ++nextRequestToken;
	request.typeName = requestType.name();

	{
		std::lock_guard lock(requestMutex);
		pendingRequest = request;
	}

	submit();

	std::unique_lock lock(requestMutex);
	if(!pendingRequest || pendingRequest->token != request.token || pendingRequest->requestID < 0)
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

JsonNode CMcpPlayerInterface::jsonRequestWaitResult(const RequestWaitResult & request) const
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

void CMcpPlayerInterface::appendStateUpdate(const std::string & type, JsonNode data) const
{
	std::lock_guard lock(updatesMutex);
	JsonNode update = Mcp::makeRevisionedUpdate(++stateRevision, type, std::move(data));
	update["time"] = JsonNode(timestampUtc());
	updateJournal.push_back(update);
	while(updateJournal.size() > 2000)
		updateJournal.pop_front();
}

JsonNode CMcpPlayerInterface::makeUpdatesJson(uint64_t sinceRevision, size_t maxUpdates) const
{
	std::lock_guard lock(updatesMutex);
	JsonNode result = Mcp::collectUpdatesSince(updateJournal, sinceRevision, maxUpdates);
	result["currentRevision"] = JsonNode(static_cast<int64_t>(stateRevision));
	return result;
}

CMcpPlayerInterface::RoutePlan CMcpPlayerInterface::makeRoutePlan(const CGHeroInstance * hero, const int3 & destination, std::optional<std::string> expectedRouteID) const
{
	RoutePlan result;
	if(!cb)
	{
		result.error = "MCP player interface is not initialized";
		return result;
	}

	std::shared_lock gameStateLock(CGameState::mutex);
	if(!cb->isInTheMap(destination))
	{
		result.error = "Destination is outside of the map";
		return result;
	}
	if(destination == hero->visitablePos())
	{
		result.error = "Destination is the hero current tile";
		return result;
	}

	CPathsInfo paths(cb->getMapSize(), hero);
	auto config = std::make_shared<SingleHeroPathfinderConfig>(paths, *cb, hero);
	cb->calculatePaths(config);

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
	result.routeID = Mcp::makeRouteId(
		hero->id.getNum(),
		hero->visitablePos().x,
		hero->visitablePos().y,
		hero->visitablePos().z,
		destination.x,
		destination.y,
		destination.z,
		pathNode->layer.getNum(),
		pathNode->moveRemains);
	result.pathPreview = jsonPathPreview(paths, destination, pathNode->layer, 30);

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

		const int3 guardingPosition = cb->guardingCreaturePosition(node.coord);
		if(guardingPosition.isValid())
			break;
		if(!cb->getVisitableObjs(node.coord).empty())
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

JsonNode CMcpPlayerInterface::makeStateJson() const
{
	JsonNode state;
	{
		std::lock_guard lock(updatesMutex);
		state["revision"] = JsonNode(static_cast<int64_t>(stateRevision));
	}

	{
		std::lock_guard lock(interfaceMutex);
		state["turn"]["active"] = JsonNode(turnActive);
		state["pendingQuery"] = makePendingQueryJson();
		state["battle"]["activeTactics"] = JsonNode(activeTacticsBattleID != BattleID::NONE);
		state["battle"]["activeStack"] = JsonNode(activeStackToMove != nullptr);
		if(activeTacticsBattleID != BattleID::NONE)
			state["battle"]["tacticsBattleId"] = JsonNode(activeTacticsBattleID.getNum());
	if(activeStackToMove)
	{
		state["battle"]["battleId"] = JsonNode(activeBattleID.getNum());
		state["battle"]["stack"]["id"] = JsonNode(static_cast<int32_t>(activeStackToMove->unitId()));
		state["battle"]["stack"]["name"] = JsonNode(jsonText(activeStackToMove->getName()));
		state["battle"]["stack"]["side"] = JsonNode(static_cast<int32_t>(activeStackToMove->unitSide()));
		state["battle"]["stack"]["position"] = jsonBattleHex(activeStackToMove->getPosition());
		}
	}

	state["player"]["id"] = JsonNode(playerID.getNum());
	state["player"]["color"] = JsonNode(playerID.toString());
	state["initialized"] = JsonNode(cb != nullptr);
	if(!cb)
		return state;

	std::shared_lock gameStateLock(CGameState::mutex);

	const PlayerState * playerState = cb->getPlayerState(playerID);
	if(playerState)
	{
		state["player"]["team"] = JsonNode(playerState->team.getNum());
		state["player"]["status"] = JsonNode(static_cast<int32_t>(playerState->status));
		state["resources"] = jsonResources(playerState->resources);
	}

	state["heroes"].Vector();
	for(const CGHeroInstance * hero : cb->getHeroesInfo())
		if(hero)
			state["heroes"].Vector().push_back(jsonHero(hero));

	state["towns"].Vector();
	for(const CGTownInstance * town : cb->getTownsInfo(true))
		if(town)
			state["towns"].Vector().push_back(jsonTown(town));

	state["tavern"]["towns"].Vector();
	for(const CGTownInstance * town : cb->getTownsInfo(true))
	{
		if(!town || town->tempOwner != playerID)
			continue;

		JsonNode tavern;
		tavern["town_id"] = JsonNode(town->id.getNum());
		tavern["town"] = JsonNode(jsonText(town->getNameTranslated()));
		tavern["heroes"].Vector();
		for(const CGHeroInstance * hero : cb->getAvailableHeroes(town))
			if(hero)
				tavern["heroes"].Vector().push_back(jsonHero(hero));
		state["tavern"]["towns"].Vector().push_back(tavern);
	}

	return state;
}

JsonNode CMcpPlayerInterface::makeStateJson(const JsonNode & arguments) const
{
	Mcp::StateSelection selection = Mcp::readStateSelection(arguments);
	JsonNode state = makeStateJson();
	if(selection.sinceRevision || selection.sections.count("updates") != 0)
		state["updates"] = makeUpdatesJson(selection.sinceRevision.value_or(0), selection.maxUpdates);
	return Mcp::filterStateBySelection(state, selection);
}

JsonNode CMcpPlayerInterface::makeActionSpaceJson() const
{
	JsonNode actionSpace;
	actionSpace["purpose"] = JsonNode("Use this payload to choose one legal VCMI MCP tool call for the current player.");
	actionSpace["rules"].Vector().push_back(JsonNode("For whole-day play, prefer vcmi.get_day_context followed by one vcmi.execute_plan."));
	actionSpace["rules"].Vector().push_back(JsonNode("Use canonical execute_plan action types: build, recruit, move_hero, visit_object, answer_query, end_turn."));
	actionSpace["rules"].Vector().push_back(JsonNode("Use vcmi.get_reachable before moving heroes; prefer recommendedTargets[*].planAction or route_id-based movement."));
	actionSpace["rules"].Vector().push_back(JsonNode("Use only object ids, route ids, and coordinates returned by MCP payloads."));
	actionSpace["rules"].Vector().push_back(JsonNode("Battle is auto-handled by the MCP interface; focus planning on adventure-map and town actions."));
	actionSpace["rules"].Vector().push_back(JsonNode("If a tool returns isError=true, inspect vcmi.get_state again before retrying."));
	actionSpace["rules"].Vector().push_back(JsonNode("If unsure and turn.active is true, vcmi.end_turn is a valid conservative action."));

	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_state", "Read current visible player state."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_updates", "Read revisioned visible state changes since a previous state revision."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_action_space", "Read current guidance, object ids, and action candidates."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_day_context", "Read compact state, build options, recommended route targets, and a suggested execute_plan skeleton."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_visible_map", "Read visible adventure-map tiles and objects around a center or owned hero."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_movement_options", "Read pathfinder-derived movement and visible object targets for an owned hero."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_reachable", "Read route-id movement targets and reachable objects for an owned hero."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.execute_plan", "Execute a sequential batch of town, recruitment, movement, query, and turn actions."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_battle_state", "Read battle stacks, active stack, and legal battle action candidates."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.end_turn", "End the active adventure-map turn."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.answer_query", "Answer the pending query/dialog using query_id and optional answer."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.move_hero", "Move an owned hero to x, y, optional z."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.move_hero_to_object", "Move an owned hero to a visible object target."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.build_town_building", "Build a currently allowed building in an owned town."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_defend", "Defend with the active battle stack."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_wait", "Wait with the active battle stack."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_move", "Move the active battle stack to a legal hex."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_shoot", "Shoot a legal target stack."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_melee_attack", "Attack a legal target stack in melee."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_end_tactics", "End the current tactics phase."));

	const JsonNode state = makeStateJson();
	const JsonNode battleState = makeBattleStateJson();
	actionSpace["stateSummary"]["turnActive"] = state["turn"]["active"];
	actionSpace["stateSummary"]["pendingQuery"] = state["pendingQuery"];
	actionSpace["stateSummary"]["battle"] = state["battle"];
	actionSpace["stateSummary"]["activeBattles"] = JsonNode(static_cast<int32_t>(battleState["battles"].Vector().size()));
	actionSpace["executePlan"]["acceptedActionTypes"].Vector();
	for(const std::string & type : Mcp::acceptedPlanActionTypes())
		actionSpace["executePlan"]["acceptedActionTypes"].Vector().push_back(JsonNode(type));
	actionSpace["executePlan"]["toolShapeAccepted"] = JsonNode(true);
	actionSpace["executePlan"]["examples"].Vector();
	{
		JsonNode build;
		build["type"] = JsonNode("build");
		build["town_id"] = JsonNode(0);
		build["building_id"] = JsonNode(30);
		actionSpace["executePlan"]["examples"].Vector().push_back(build);

		JsonNode visit;
		visit["type"] = JsonNode("visit_object");
		visit["hero_id"] = JsonNode(990);
		visit["object_id"] = JsonNode(181);
		visit["route_id"] = JsonNode("route_id from vcmi.get_reachable");
		actionSpace["executePlan"]["examples"].Vector().push_back(visit);

		JsonNode move;
		move["type"] = JsonNode("move_hero");
		move["hero_id"] = JsonNode(990);
		move["x"] = JsonNode(20);
		move["y"] = JsonNode(2);
		move["z"] = JsonNode(0);
		move["route_id"] = JsonNode("route_id from vcmi.get_reachable");
		actionSpace["executePlan"]["examples"].Vector().push_back(move);

		JsonNode endTurn;
		endTurn["type"] = JsonNode("end_turn");
		actionSpace["executePlan"]["examples"].Vector().push_back(endTurn);
	}

	actionSpace["objectIds"]["heroes"].Vector();
	for(const JsonNode & hero : state["heroes"].Vector())
	{
		JsonNode heroRef;
		heroRef["hero_id"] = hero["id"];
		heroRef["name"] = hero["name"];
		heroRef["position"] = hero["position"];
		heroRef["movement"] = hero["movement"];
		actionSpace["objectIds"]["heroes"].Vector().push_back(heroRef);
	}

	actionSpace["objectIds"]["towns"].Vector();
	for(const JsonNode & town : state["towns"].Vector())
	{
		JsonNode townRef;
		townRef["town_id"] = town["id"];
		townRef["name"] = town["name"];
		townRef["position"] = town["position"];
		actionSpace["objectIds"]["towns"].Vector().push_back(townRef);
	}

	actionSpace["currentActions"].Vector();
	{
		JsonNode dayContext = jsonAction("vcmi.get_day_context", "Read compact whole-day context and suggested execute_plan skeleton.");
		dayContext["arguments"]["radius"] = JsonNode(20);
		dayContext["arguments"]["max_objects"] = JsonNode(16);
		dayContext["arguments"]["max_recommended"] = JsonNode(10);
		actionSpace["currentActions"].Vector().push_back(dayContext);
	}
	if(state["turn"]["active"].Bool())
	{
		JsonNode action = jsonAction("vcmi.end_turn", "End the current turn.");
		action["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(action);
	}

	if(!state["pendingQuery"].isNull())
	{
		JsonNode action = jsonAction("vcmi.answer_query", "Answer the currently pending VCMI query/dialog.");
		action["arguments"]["query_id"] = state["pendingQuery"]["id"];
		action["arguments"]["answer"] = JsonNode(0);
		action["notes"].Vector().push_back(JsonNode("Use answer=0 for the first/default option unless the query data indicates another choice."));
		actionSpace["currentActions"].Vector().push_back(action);
	}

	if(state["battle"]["activeStack"].Bool())
	{
		JsonNode inspectBattle = jsonAction("vcmi.get_battle_state", "Inspect legal battle actions for the active stack.");
		inspectBattle["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(inspectBattle);

		JsonNode defend = jsonAction("vcmi.battle_defend", "Defend with the active battle stack.");
		defend["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(defend);

		JsonNode wait = jsonAction("vcmi.battle_wait", "Wait with the active battle stack.");
		wait["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(wait);
	}

	for(const JsonNode & battle : battleState["battles"].Vector())
	{
		if(!hasField(battle, "legalActions"))
			continue;

		const JsonNode & legal = battle["legalActions"];
		if(!legal["moveHexes"].Vector().empty())
		{
			JsonNode action = jsonAction("vcmi.battle_move", "Move the active battle stack to this legal hex.");
			action["arguments"]["hex"] = legal["moveHexes"].Vector().front()["id"];
			actionSpace["currentActions"].Vector().push_back(action);
		}

		for(const JsonNode & target : legal["shootTargets"].Vector())
		{
			JsonNode action = jsonAction("vcmi.battle_shoot", "Shoot this legal target stack.");
			action["arguments"]["target_stack_id"] = target["target_stack_id"];
			action["target"] = target["target"];
			actionSpace["currentActions"].Vector().push_back(action);
		}

		for(const JsonNode & target : legal["meleeTargets"].Vector())
		{
			JsonNode action = jsonAction("vcmi.battle_melee_attack", "Melee attack this legal target stack.");
			action["arguments"]["target_stack_id"] = target["target_stack_id"];
			action["arguments"]["attack_from_hex"] = target["attack_from_hex"]["id"];
			action["target"] = target["target"];
			actionSpace["currentActions"].Vector().push_back(action);
		}
	}

	if(state["battle"]["activeTactics"].Bool())
	{
		JsonNode action = jsonAction("vcmi.battle_end_tactics", "End the current tactics phase.");
		action["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(action);
	}

	for(const JsonNode & hero : state["heroes"].Vector())
	{
		JsonNode visibleMap = jsonAction("vcmi.get_visible_map", "Inspect visible map around this owned hero.");
		visibleMap["arguments"]["hero_id"] = hero["id"];
		visibleMap["arguments"]["radius"] = JsonNode(8);
		actionSpace["currentActions"].Vector().push_back(visibleMap);

		JsonNode movementOptions = jsonAction("vcmi.get_movement_options", "Inspect legal movement destinations and visible object targets for this owned hero.");
		movementOptions["arguments"]["hero_id"] = hero["id"];
		movementOptions["arguments"]["radius"] = JsonNode(12);
		movementOptions["arguments"]["max_options"] = JsonNode(80);
		actionSpace["currentActions"].Vector().push_back(movementOptions);

		JsonNode reachable = jsonAction("vcmi.get_reachable", "Inspect this-day reachable route-id targets for this owned hero.");
		reachable["arguments"]["hero_id"] = hero["id"];
		reachable["arguments"]["radius"] = JsonNode(20);
		reachable["arguments"]["max_options"] = JsonNode(80);
		reachable["arguments"]["max_objects"] = JsonNode(16);
		reachable["arguments"]["max_recommended"] = JsonNode(10);
		reachable["arguments"]["compact"] = JsonNode(true);
		actionSpace["currentActions"].Vector().push_back(reachable);
	}

	actionSpace["buildOptions"].Vector();
	if(cb)
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		for(const CGTownInstance * town : cb->getTownsInfo(true))
		{
			if(!town || town->tempOwner != playerID)
				continue;

			for(const auto & buildingEntry : town->getTown()->buildings)
			{
				const CBuilding * building = buildingEntry.second.get();
				if(!building || building->mode != CBuilding::BUILD_NORMAL)
					continue;

				if(cb->canBuildStructure(town, building->bid) == EBuildingState::ALLOWED)
				{
					JsonNode option = jsonBuildOption(town, building);
					actionSpace["buildOptions"].Vector().push_back(option);

					JsonNode action = jsonAction("vcmi.build_town_building", "Build this currently allowed town building.");
					action["arguments"]["town_id"] = option["town_id"];
					action["arguments"]["building_id"] = option["building_id"];
					action["building"] = option["building"];
					action["town"] = option["town"];
					action["cost"] = option["cost"];
					actionSpace["currentActions"].Vector().push_back(action);
				}
			}
		}
	}

	return actionSpace;
}

JsonNode CMcpPlayerInterface::makeDayContextJson(const JsonNode & arguments) const
{
	JsonNode result;
	result["purpose"] = JsonNode("Compact whole-day planning context. Prefer one vcmi.execute_plan using suggestedPlan.actions, edited as needed.");
	result["rules"].Vector().push_back(JsonNode("Use canonical execute_plan action types: build, recruit, move_hero, visit_object, answer_query, end_turn."));
	result["rules"].Vector().push_back(JsonNode("Use recommendedTargets[*].planAction entries directly inside execute_plan actions."));
	result["rules"].Vector().push_back(JsonNode("Do not call get_reachable repeatedly unless new terrain/object discovery changes the plan."));

	JsonNode stateArgs;
	stateArgs["select"].Vector().push_back(JsonNode("summary"));
	stateArgs["select"].Vector().push_back(JsonNode("resources"));
	stateArgs["select"].Vector().push_back(JsonNode("heroes"));
	stateArgs["select"].Vector().push_back(JsonNode("towns"));
	stateArgs["select"].Vector().push_back(JsonNode("tavern"));
	if(hasField(arguments, "since_revision"))
	{
		stateArgs["select"].Vector().push_back(JsonNode("updates"));
		stateArgs["since_revision"] = arguments["since_revision"];
	}
	stateArgs["max_updates"] = JsonNode(readClampedInteger(arguments, "max_updates", 100, 1, 1000));

	JsonNode state = makeStateJson(stateArgs);
	JsonNode actionSpace = makeActionSpaceJson();
	result["state"] = state;
	result["buildOptions"] = actionSpace["buildOptions"];
	result["executePlan"]["schema"] = Mcp::makeExecutePlanSchema();
	result["executePlan"]["acceptedActionTypes"].Vector();
	for(const std::string & type : Mcp::acceptedPlanActionTypes())
		result["executePlan"]["acceptedActionTypes"].Vector().push_back(JsonNode(type));

	const int32_t radius = readClampedInteger(arguments, "radius", 20, 1, 30);
	const int32_t maxOptions = readClampedInteger(arguments, "max_options", 80, 1, 300);
	const int32_t maxObjects = readClampedInteger(arguments, "max_objects", 16, 0, 300);
	const int32_t maxRecommended = readClampedInteger(arguments, "max_recommended", 10, 0, 100);
	const bool includePaths = readBool(arguments, "include_paths", false);

	result["heroReachability"].Vector();
	if(hasField(state, "heroes") && state["heroes"].isVector())
	{
		for(const JsonNode & hero : state["heroes"].Vector())
		{
			if(!hasField(hero, "id") || !hero["id"].isNumber())
				continue;
			if(hasField(hero, "movement") && hero["movement"].isNumber() && hero["movement"].Integer() <= 0)
				continue;

			JsonNode reachableArgs;
			reachableArgs["hero_id"] = hero["id"];
			reachableArgs["radius"] = JsonNode(radius);
			reachableArgs["max_options"] = JsonNode(maxOptions);
			reachableArgs["max_objects"] = JsonNode(maxObjects);
			reachableArgs["max_recommended"] = JsonNode(maxRecommended);
			reachableArgs["compact"] = JsonNode(true);
			reachableArgs["include_tiles"] = JsonNode(false);
			reachableArgs["include_paths"] = JsonNode(includePaths);
			result["heroReachability"].Vector().push_back(makeReachableJson(reachableArgs));
		}
	}

	result["suggestedPlan"]["dry_run"] = JsonNode(false);
	result["suggestedPlan"]["max_updates"] = JsonNode(100);
	result["suggestedPlan"]["return_select"].Vector().push_back(JsonNode("summary"));
	result["suggestedPlan"]["return_select"].Vector().push_back(JsonNode("resources"));
	result["suggestedPlan"]["return_select"].Vector().push_back(JsonNode("heroes"));
	result["suggestedPlan"]["return_select"].Vector().push_back(JsonNode("towns"));
	result["suggestedPlan"]["return_select"].Vector().push_back(JsonNode("updates"));
	result["suggestedPlan"]["actions"].Vector();

	if(hasField(actionSpace, "buildOptions") && actionSpace["buildOptions"].isVector() && !actionSpace["buildOptions"].Vector().empty())
	{
		const JsonNode & build = actionSpace["buildOptions"].Vector().front();
		JsonNode buildAction;
		buildAction["type"] = JsonNode("build");
		buildAction["town_id"] = build["town_id"];
		buildAction["building_id"] = build["building_id"];
		result["suggestedPlan"]["actions"].Vector().push_back(buildAction);
	}

	for(const JsonNode & reachable : result["heroReachability"].Vector())
	{
		if(!hasField(reachable, "recommendedTargets") || !reachable["recommendedTargets"].isVector() || reachable["recommendedTargets"].Vector().empty())
			continue;
		const JsonNode & target = reachable["recommendedTargets"].Vector().front();
		if(hasField(target, "planAction"))
			result["suggestedPlan"]["actions"].Vector().push_back(target["planAction"]);
	}

	JsonNode endTurn;
	endTurn["type"] = JsonNode("end_turn");
	result["suggestedPlan"]["actions"].Vector().push_back(endTurn);
	return result;
}

JsonNode CMcpPlayerInterface::makeVisibleMapJson(const JsonNode & arguments) const
{
	JsonNode result;
	result["ok"] = JsonNode(false);
	result["player"]["id"] = JsonNode(playerID.getNum());
	result["player"]["color"] = JsonNode(playerID.toString());
	if(!cb)
	{
		result["error"] = JsonNode("MCP player interface is not initialized");
		return result;
	}

	const bool fullVisible = readBool(arguments, "full_visible", false);
	const bool includeTiles = readBool(arguments, "include_tiles", true);
	const bool includeObjects = readBool(arguments, "include_objects", true);
	const int32_t radius = readClampedInteger(arguments, "radius", 8, 0, 30);
	const int32_t maxTiles = readClampedInteger(arguments, "max_tiles", fullVisible ? 1500 : 500, 1, 5000);

	std::shared_lock gameStateLock(CGameState::mutex);

	const CGHeroInstance * contextHero = nullptr;
	int3 center(0, 0, 0);
	if(hasField(arguments, "hero_id"))
	{
		contextHero = cb->getHero(ObjectInstanceID(readInteger(arguments, "hero_id")));
		if(!contextHero)
		{
			result["error"] = JsonNode("Unknown hero id");
			return result;
		}
		if(contextHero->tempOwner != playerID)
		{
			result["error"] = JsonNode("Hero is not owned by this MCP player");
			return result;
		}
		center = contextHero->visitablePos();
	}
	else if(hasField(arguments, "x") || hasField(arguments, "y"))
	{
		center = int3(
			readInteger(arguments, "x", 0),
			readInteger(arguments, "y", 0),
			readInteger(arguments, "z", 0));
	}
	else
	{
		const auto heroes = cb->getHeroesInfo();
		if(!heroes.empty() && heroes.front())
		{
			contextHero = heroes.front();
			center = contextHero->visitablePos();
		}
	}

	if(!fullVisible && !cb->isInTheMap(center))
	{
		result["error"] = JsonNode("Center is outside of the map");
		return result;
	}

	result["ok"] = JsonNode(true);
	result["mapSize"] = jsonPosition(cb->getMapSize());
	result["center"] = jsonPosition(center);
	result["radius"] = JsonNode(radius);
	result["fullVisible"] = JsonNode(fullVisible);
	result["visibility"] = JsonNode("revealed tiles and visible/owned objects only");
	result["truncated"] = JsonNode(false);
	result["tiles"].Vector();
	result["objects"].Vector();

	std::set<int32_t> seenObjects;
	int32_t visitedTiles = 0;

	auto emitTile = [&](const int3 & position)
	{
		if(visitedTiles >= maxTiles)
		{
			result["truncated"] = JsonNode(true);
			return;
		}
		if(!cb->isInTheMap(position) || !cb->isVisibleFor(position, playerID))
			return;

		const TerrainTile * tile = cb->getTile(position, false);
		if(!tile)
			return;

		++visitedTiles;
		if(includeTiles)
			result["tiles"].Vector().push_back(jsonTerrainTile(*tile, position, *cb, playerID));

		if(includeObjects)
		{
			for(const ObjectInstanceID & objectID : tile->visitableObjects)
				addVisibleObjectFromId(result["objects"], seenObjects, *cb, objectID, playerID, contextHero);
			for(const ObjectInstanceID & objectID : tile->blockingObjects)
				addVisibleObjectFromId(result["objects"], seenObjects, *cb, objectID, playerID, contextHero);
			const CGObjectInstance * topObject = cb->getTopObj(position);
			if(topObject)
				addVisibleObjectFromId(result["objects"], seenObjects, *cb, topObject->id, playerID, contextHero);
		}
	};

	if(fullVisible)
	{
		const int3 mapSize = cb->getMapSize();
		for(int z = 0; z < mapSize.z && !result["truncated"].Bool(); ++z)
			for(int x = 0; x < mapSize.x && !result["truncated"].Bool(); ++x)
				for(int y = 0; y < mapSize.y && !result["truncated"].Bool(); ++y)
					emitTile(int3(x, y, z));
	}
	else
	{
		FowTilesType tiles;
		cb->getTilesInRange(tiles, center, radius, ETileVisibility::REVEALED, playerID);
		for(const int3 & position : tiles)
		{
			if(result["truncated"].Bool())
				break;
			emitTile(position);
		}
	}

	result["tileCount"] = JsonNode(visitedTiles);
	result["objectCount"] = JsonNode(static_cast<int32_t>(seenObjects.size()));
	return result;
}

JsonNode CMcpPlayerInterface::makeMovementOptionsJson(const JsonNode & arguments) const
{
	JsonNode result;
	result["ok"] = JsonNode(false);
	result["visibility"] = JsonNode("movement options are calculated from player-visible callback data");
	if(!cb)
	{
		result["error"] = JsonNode("MCP player interface is not initialized");
		return result;
	}

	const CGHeroInstance * hero = cb->getHero(ObjectInstanceID(readInteger(arguments, "hero_id")));
	if(!hero)
	{
		result["error"] = JsonNode("Unknown hero id");
		return result;
	}
	if(hero->tempOwner != playerID)
	{
		result["error"] = JsonNode("Hero is not owned by this MCP player");
		return result;
	}

	const int32_t radius = readClampedInteger(arguments, "radius", 12, 1, 30);
	const int32_t maxOptions = readClampedInteger(arguments, "max_options", 80, 1, 300);
	const int32_t maxObjects = readClampedInteger(arguments, "max_objects", maxOptions, 0, 300);
	const int32_t maxRecommended = readClampedInteger(arguments, "max_recommended", 10, 0, 100);
	const bool includeFutureTurns = readBool(arguments, "include_future_turns", false);
	const bool compact = readBool(arguments, "compact", false);
	const bool includeTiles = readBool(arguments, "include_tiles", !compact);
	const bool includePaths = readBool(arguments, "include_paths", !compact);

	struct MovementCandidate
	{
		JsonNode json;
		float cost;
		int32_t turns;
		int32_t x;
		int32_t y;
		int32_t z;
	};

	std::vector<MovementCandidate> candidates;
	std::vector<MovementCandidate> objectCandidates;
	std::set<int32_t> seenTargetObjects;

	std::shared_lock gameStateLock(CGameState::mutex);

	CPathsInfo paths(cb->getMapSize(), hero);
	auto config = std::make_shared<SingleHeroPathfinderConfig>(paths, *cb, hero);
	cb->calculatePaths(config);

	FowTilesType tiles;
	cb->getTilesInRange(tiles, hero->visitablePos(), radius, ETileVisibility::REVEALED, playerID);

	result["ok"] = JsonNode(true);
	result["hero"] = jsonHero(hero);
	result["radius"] = JsonNode(radius);
	result["currentTurnOnly"] = JsonNode(!includeFutureTurns);
	result["compact"] = JsonNode(compact);
	result["includeTiles"] = JsonNode(includeTiles);
	result["includePaths"] = JsonNode(includePaths);
	result["movementOptions"].Vector();
	result["objectTargets"].Vector();
	result["recommendedTargets"].Vector();

	for(const int3 & position : tiles)
	{
		if(position == hero->visitablePos())
			continue;
		if(!cb->isInTheMap(position) || !cb->isVisibleFor(position, playerID))
			continue;

		const CGPathNode * pathNode = paths.getPathInfo(position);
		if(!pathNode || !pathNode->reachable())
			continue;
		if(!includeFutureTurns && pathNode->turns != 0)
			continue;
		if(pathNode->accessible == EPathAccessibility::NOT_SET || pathNode->accessible == EPathAccessibility::BLOCKED)
			continue;

		JsonNode option = compact ? jsonCompactPathNode(*pathNode) : jsonPathNode(*pathNode);
		if(includePaths)
			option["path"] = jsonPathPreview(paths, position, pathNode->layer, 20);
		option["route_id"] = JsonNode(Mcp::makeRouteId(
			hero->id.getNum(),
			hero->visitablePos().x,
			hero->visitablePos().y,
			hero->visitablePos().z,
			position.x,
			position.y,
			position.z,
			pathNode->layer.getNum(),
			pathNode->moveRemains));
		option["suggestedTool"] = JsonNode("vcmi.move_hero");
		option["arguments"]["hero_id"] = JsonNode(hero->id.getNum());
		option["arguments"]["x"] = JsonNode(position.x);
		option["arguments"]["y"] = JsonNode(position.y);
		option["arguments"]["z"] = JsonNode(position.z);
		option["arguments"]["route_id"] = option["route_id"];
		option["planAction"]["type"] = JsonNode("move_hero");
		option["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
		option["planAction"]["x"] = JsonNode(position.x);
		option["planAction"]["y"] = JsonNode(position.y);
		option["planAction"]["z"] = JsonNode(position.z);
		option["planAction"]["route_id"] = option["route_id"];

		const CGObjectInstance * topObject = cb->getTopObj(position);
		if(topObject)
		{
			option["object"] = compact ? jsonCompactMapObject(topObject, playerID, hero) : jsonMapObject(topObject, playerID, hero);
			const bool isObjectAction =
				pathNode->action == EPathNodeAction::BATTLE ||
				pathNode->action == EPathNodeAction::VISIT ||
				pathNode->action == EPathNodeAction::BLOCKING_VISIT ||
				pathNode->action == EPathNodeAction::TELEPORT_BLOCKING_VISIT ||
				pathNode->action == EPathNodeAction::TELEPORT_BATTLE;
			if(isObjectAction && seenTargetObjects.insert(topObject->id.getNum()).second)
			{
				JsonNode target;
				target["object"] = compact ? jsonCompactMapObject(topObject, playerID, hero) : jsonMapObject(topObject, playerID, hero);
				target["path"] = compact ? jsonCompactPathNode(*pathNode) : jsonPathNode(*pathNode);
				target["route_id"] = option["route_id"];
				if(includePaths)
					target["pathPreview"] = jsonPathPreview(paths, position, pathNode->layer, 20);
				target["suggestedTool"] = JsonNode("vcmi.execute_plan");
				target["directTool"] = JsonNode("vcmi.move_hero_to_object");
				target["arguments"]["hero_id"] = JsonNode(hero->id.getNum());
				target["arguments"]["object_id"] = JsonNode(topObject->id.getNum());
				target["arguments"]["route_id"] = option["route_id"];
				target["planAction"]["type"] = JsonNode("visit_object");
				target["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
				target["planAction"]["object_id"] = JsonNode(topObject->id.getNum());
				target["planAction"]["route_id"] = option["route_id"];
				objectCandidates.push_back(MovementCandidate{target, pathNode->cost, static_cast<int32_t>(pathNode->turns), position.x, position.y, position.z});
			}
		}

		candidates.push_back(MovementCandidate{option, pathNode->cost, static_cast<int32_t>(pathNode->turns), position.x, position.y, position.z});
	}

	std::sort(candidates.begin(), candidates.end(), [](const MovementCandidate & left, const MovementCandidate & right)
	{
		return std::tie(left.turns, left.cost, left.z, left.x, left.y) < std::tie(right.turns, right.cost, right.z, right.x, right.y);
	});
	std::sort(objectCandidates.begin(), objectCandidates.end(), [](const MovementCandidate & left, const MovementCandidate & right)
	{
		return std::tie(left.turns, left.cost, left.z, left.x, left.y) < std::tie(right.turns, right.cost, right.z, right.x, right.y);
	});

	result["truncated"] = JsonNode(static_cast<int32_t>(candidates.size()) > maxOptions || static_cast<int32_t>(objectCandidates.size()) > maxObjects);
	result["reachableTileCount"] = JsonNode(static_cast<int32_t>(candidates.size()));
	result["reachableObjectCount"] = JsonNode(static_cast<int32_t>(objectCandidates.size()));
	if(includeTiles)
		for(size_t index = 0; index < candidates.size() && index < static_cast<size_t>(maxOptions); ++index)
			result["movementOptions"].Vector().push_back(candidates[index].json);
	for(size_t index = 0; index < objectCandidates.size() && index < static_cast<size_t>(maxObjects); ++index)
		result["objectTargets"].Vector().push_back(objectCandidates[index].json);
	for(size_t index = 0; index < objectCandidates.size() && index < static_cast<size_t>(maxRecommended); ++index)
		result["recommendedTargets"].Vector().push_back(objectCandidates[index].json);

	result["movementOptionCount"] = JsonNode(static_cast<int32_t>(result["movementOptions"].Vector().size()));
	result["objectTargetCount"] = JsonNode(static_cast<int32_t>(result["objectTargets"].Vector().size()));
	return result;
}

JsonNode CMcpPlayerInterface::makeReachableJson(const JsonNode & arguments) const
{
	JsonNode reachableArguments = arguments;
	if(!hasField(reachableArguments, "compact"))
		reachableArguments["compact"] = JsonNode(true);
	if(!hasField(reachableArguments, "include_tiles"))
		reachableArguments["include_tiles"] = JsonNode(false);
	if(!hasField(reachableArguments, "include_paths"))
		reachableArguments["include_paths"] = JsonNode(false);
	if(!hasField(reachableArguments, "max_objects"))
		reachableArguments["max_objects"] = JsonNode(readClampedInteger(reachableArguments, "max_options", 20, 1, 300));
	if(!hasField(reachableArguments, "max_recommended"))
		reachableArguments["max_recommended"] = JsonNode(12);

	JsonNode result = makeMovementOptionsJson(reachableArguments);
	result["purpose"] = JsonNode("Compact reachable this-day targets. Prefer recommendedTargets planAction entries inside vcmi.execute_plan.");
	result["reachableTiles"] = result["movementOptions"];
	result["reachableObjects"] = result["objectTargets"];
	return result;
}

Mcp::Protocol::ToolResult CMcpPlayerInterface::executePlan(const JsonNode & arguments)
{
	std::lock_guard actionLock(actionMutex);
	if(!hasField(arguments, "actions") || !arguments["actions"].isVector())
		return makeToolError("Missing or non-array argument: actions");

	const bool dryRun = readBool(arguments, "dry_run", false);
	const size_t maxUpdates = static_cast<size_t>(readClampedInteger(arguments, "max_updates", 200, 1, 1000));
	uint64_t fromRevision = 0;
	{
		std::lock_guard lock(updatesMutex);
		fromRevision = stateRevision;
	}

	JsonNode result;
	result["ok"] = JsonNode(true);
	result["status"] = JsonNode("completed");
	result["plan_id"] = hasField(arguments, "plan_id") ? JsonNode(readString(arguments, "plan_id")) : JsonNode();
	result["dryRun"] = JsonNode(dryRun);
	result["fromRevision"] = JsonNode(static_cast<int64_t>(fromRevision));
	result["executed"].Vector();
	result["failed"].Vector();
	result["remaining"].Vector();
	result["acceptedActionTypes"].Vector();
	for(const std::string & type : Mcp::acceptedPlanActionTypes())
		result["acceptedActionTypes"].Vector().push_back(JsonNode(type));
	result["acceptedToolShape"] = JsonNode(true);
	result["acceptedToolShapeDescription"] = JsonNode("Plan actions may be canonical objects, aliased typed objects, or tool-shaped objects with tool and arguments fields.");

	std::vector<JsonNode> actions;
	actions.reserve(arguments["actions"].Vector().size());
	for(size_t index = 0; index < arguments["actions"].Vector().size(); ++index)
	{
		try
		{
			actions.push_back(Mcp::normalizePlanAction(arguments["actions"].Vector()[index]));
		}
		catch(const std::exception & exception)
		{
			JsonNode actionResult;
			actionResult["index"] = JsonNode(static_cast<int32_t>(index));
			actionResult["ok"] = JsonNode(false);
			actionResult["error"] = JsonNode(exception.what());
			actionResult["acceptedActionTypes"] = result["acceptedActionTypes"];
			result["ok"] = JsonNode(false);
			result["status"] = JsonNode("partial");
			result["failed"].Vector().push_back(actionResult);
			for(size_t remainingIndex = index + 1; remainingIndex < arguments["actions"].Vector().size(); ++remainingIndex)
				result["remaining"].Vector().push_back(arguments["actions"].Vector()[remainingIndex]);
			result["toRevision"] = JsonNode(static_cast<int64_t>(fromRevision));
			result["updates"] = makeUpdatesJson(fromRevision, maxUpdates);
			return makeJsonResult(result);
		}
	}

	auto stopWithFailure = [&](JsonNode actionResult, size_t failedIndex)
	{
		result["ok"] = JsonNode(false);
		result["status"] = JsonNode("partial");
		result["failed"].Vector().push_back(actionResult);
		for(size_t index = failedIndex + 1; index < actions.size(); ++index)
			result["remaining"].Vector().push_back(actions[index]);
	};

	for(size_t index = 0; index < actions.size(); ++index)
	{
		const JsonNode & action = actions[index];
		JsonNode actionResult;
		actionResult["index"] = JsonNode(static_cast<int32_t>(index));
		if(hasField(action, "id"))
			actionResult["id"] = action["id"];
		if(!hasField(action, "type"))
		{
			actionResult["ok"] = JsonNode(false);
			actionResult["error"] = JsonNode("Plan action is missing type. Use canonical type values or a tool-shaped action with tool and arguments.");
			actionResult["acceptedActionTypes"] = result["acceptedActionTypes"];
			stopWithFailure(actionResult, index);
			break;
		}

		const std::string type = readString(action, "type");
		actionResult["type"] = JsonNode(type);

		if(!dryRun)
		{
			std::lock_guard lock(interfaceMutex);
			if(pendingQuery && type != "answer_query")
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("Pending query must be answered before continuing plan execution");
				actionResult["pendingQuery"] = makePendingQueryJson();
				stopWithFailure(actionResult, index);
				break;
			}
			if(activeBattleID != BattleID::NONE || activeTacticsBattleID != BattleID::NONE)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("Battle is active; plan execution paused for auto-battle resolution");
				stopWithFailure(actionResult, index);
				break;
			}
		}

		if(type == "build")
		{
			const CGTownInstance * town = cb->getTown(ObjectInstanceID(readInteger(action, "town_id")));
			if(!town || town->tempOwner != playerID)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("Unknown town or town is not owned by this MCP player");
				stopWithFailure(actionResult, index);
				break;
			}
			const BuildingID buildingID(readInteger(action, "building_id"));
			if(cb->canBuildStructure(town, buildingID) != EBuildingState::ALLOWED)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("Building is not currently allowed");
				stopWithFailure(actionResult, index);
				break;
			}
			actionResult["town_id"] = JsonNode(town->id.getNum());
			actionResult["building_id"] = JsonNode(buildingID.getNum());
			if(!dryRun)
			{
				bool accepted = false;
				RequestWaitResult request = submitAndWaitForRequest(typeid(BuildStructure), [&]
				{
					accepted = cb->buildBuilding(town, buildingID);
				});
				actionResult["request"] = jsonRequestWaitResult(request);
				actionResult["ok"] = JsonNode(accepted && request.applied);
				if(!accepted || !request.applied)
				{
					actionResult["error"] = JsonNode("Build action was rejected");
					stopWithFailure(actionResult, index);
					break;
				}
			}
			else
			{
				actionResult["ok"] = JsonNode(true);
			}
			result["executed"].Vector().push_back(actionResult);
			continue;
		}

		if(type == "recruit")
		{
			const int32_t sourceID = hasField(action, "source_id") ? readInteger(action, "source_id") : readInteger(action, "town_id");
			const CGObjectInstance * sourceObject = cb->getObj(ObjectInstanceID(sourceID), false);
			const CGDwelling * dwelling = dynamic_cast<const CGDwelling *>(sourceObject);
			const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(sourceObject);
			const CArmedInstance * destination = town ? town->getUpperArmy() : dynamic_cast<const CArmedInstance *>(sourceObject);
			if(hasField(action, "destination_id"))
				destination = dynamic_cast<const CArmedInstance *>(cb->getObj(ObjectInstanceID(readInteger(action, "destination_id")), false));

			if(!dwelling || !destination || dwelling->tempOwner != playerID)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("Unknown recruitment source, destination, or source is not owned by this MCP player");
				stopWithFailure(actionResult, index);
				break;
			}

			const int32_t level = readInteger(action, "level");
			if(level < 0 || level >= static_cast<int32_t>(dwelling->creatures.size()) || dwelling->creatures[level].second.empty())
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("Recruitment level is not available");
				stopWithFailure(actionResult, index);
				break;
			}

			const CreatureID creatureID = hasField(action, "creature_id") ? CreatureID(readInteger(action, "creature_id")) : dwelling->creatures[level].second.back();
			if(!vstd::contains(dwelling->creatures[level].second, creatureID))
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("creature_id is not available at this recruitment level");
				stopWithFailure(actionResult, index);
				break;
			}

			int32_t amount = readInteger(action, "amount", static_cast<int32_t>(dwelling->creatures[level].first));
			amount = std::clamp<int32_t>(amount, 0, static_cast<int32_t>(dwelling->creatures[level].first));
			if(const CCreature * creature = creatureID.toCreature())
			{
				const int32_t affordable = cb->getResourceAmount() / creature->getFullRecruitCost();
				amount = std::min(amount, affordable);
			}
			if(amount <= 0)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("No recruitable or affordable creatures for this action");
				stopWithFailure(actionResult, index);
				break;
			}

			actionResult["source_id"] = JsonNode(dwelling->id.getNum());
			actionResult["destination_id"] = JsonNode(destination->id.getNum());
			actionResult["level"] = JsonNode(level);
			actionResult["creature_id"] = JsonNode(creatureID.getNum());
			actionResult["amount"] = JsonNode(amount);
			if(!dryRun)
			{
				RequestWaitResult request = submitAndWaitForRequest(typeid(RecruitCreatures), [&]
				{
					cb->recruitCreatures(dwelling, destination, creatureID, amount, level);
				});
				actionResult["request"] = jsonRequestWaitResult(request);
				actionResult["ok"] = JsonNode(request.applied);
				if(!request.applied)
				{
					actionResult["error"] = JsonNode("Recruit action was rejected");
					stopWithFailure(actionResult, index);
					break;
				}
			}
			else
			{
				actionResult["ok"] = JsonNode(true);
			}
			result["executed"].Vector().push_back(actionResult);
			continue;
		}

		if(type == "move_hero" || type == "visit_object")
		{
			const CGHeroInstance * hero = cb->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
			if(!hero || hero->tempOwner != playerID)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode("Unknown hero or hero is not owned by this MCP player");
				stopWithFailure(actionResult, index);
				break;
			}

			int3 destination;
			if(type == "visit_object")
			{
				const CGObjectInstance * object = cb->getObj(ObjectInstanceID(readInteger(action, "object_id")), false);
				if(!object)
				{
					actionResult["ok"] = JsonNode(false);
					actionResult["error"] = JsonNode("Unknown or not visible object_id");
					stopWithFailure(actionResult, index);
					break;
				}
				destination = object->visitablePos();
				actionResult["object_id"] = JsonNode(object->id.getNum());
			}
			else
			{
				const int z = hasField(action, "z") ? readInteger(action, "z") : hero->visitablePos().z;
				destination = int3(readInteger(action, "x"), readInteger(action, "y"), z);
			}

			RoutePlan route = makeRoutePlan(hero, destination, readOptionalString(action, "route_id"));
			if(!route.ok)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode(route.error);
				stopWithFailure(actionResult, index);
				break;
			}

			actionResult["route_id"] = JsonNode(route.routeID);
			actionResult["destination"] = jsonPosition(route.destination);
			actionResult["submittedPath"] = jsonPositions(route.requestPath);
			if(!dryRun)
			{
				RequestWaitResult request = submitAndWaitForRequest(typeid(MoveHero), [this, hero, route]
				{
					cb->moveHero(hero, route.requestPath, route.transit, route.layer);
				});
				actionResult["request"] = jsonRequestWaitResult(request);
				actionResult["ok"] = JsonNode(request.applied);
				if(const CGHeroInstance * updatedHero = cb->getHero(hero->id))
					actionResult["hero"] = jsonHero(updatedHero);
				if(!request.applied)
				{
					actionResult["error"] = JsonNode("Move action was rejected");
					stopWithFailure(actionResult, index);
					break;
				}
			}
			else
			{
				actionResult["ok"] = JsonNode(true);
			}
			result["executed"].Vector().push_back(actionResult);
			continue;
		}

		if(type == "answer_query")
		{
			QueryID queryID(readInteger(action, "query_id"));
			std::optional<int32_t> answer;
			if(hasField(action, "answer"))
				answer = readInteger(action, "answer");
			if(!dryRun)
			{
				int requestID = cb->sendQueryReply(answer, queryID);
				clearPendingQuery(queryID);
				actionResult["requestId"] = JsonNode(requestID);
			}
			actionResult["ok"] = JsonNode(true);
			result["executed"].Vector().push_back(actionResult);
			continue;
		}

		if(type == "end_turn")
		{
			if(!dryRun)
			{
				{
					std::lock_guard lock(interfaceMutex);
					if(!turnActive)
					{
						actionResult["ok"] = JsonNode(false);
						actionResult["error"] = JsonNode("No active turn to end");
						stopWithFailure(actionResult, index);
						break;
					}
					turnActive = false;
				}
				RequestWaitResult request = submitAndWaitForRequest(typeid(EndTurn), [this]
				{
					cb->endTurn();
				});
				actionResult["request"] = jsonRequestWaitResult(request);
				actionResult["ok"] = JsonNode(request.applied);
				if(!request.applied)
				{
					std::lock_guard lock(interfaceMutex);
					turnActive = true;
					actionResult["error"] = JsonNode("End turn action was rejected");
					stopWithFailure(actionResult, index);
					break;
				}
			}
			else
			{
				actionResult["ok"] = JsonNode(true);
			}
			result["executed"].Vector().push_back(actionResult);
			result["status"] = JsonNode("turn_ended");
			break;
		}

		actionResult["ok"] = JsonNode(false);
		actionResult["error"] = JsonNode("Unsupported plan action type: " + type);
		actionResult["acceptedActionTypes"] = result["acceptedActionTypes"];
		stopWithFailure(actionResult, index);
		break;
	}

	{
		std::lock_guard lock(updatesMutex);
		result["toRevision"] = JsonNode(static_cast<int64_t>(stateRevision));
	}
	result["updates"] = makeUpdatesJson(fromRevision, maxUpdates);
	if(hasField(arguments, "return_select"))
	{
		JsonNode stateArgs;
		stateArgs["select"] = arguments["return_select"];
		stateArgs["since_revision"] = JsonNode(static_cast<int64_t>(fromRevision));
		stateArgs["max_updates"] = JsonNode(static_cast<int32_t>(maxUpdates));
		result["state"] = makeStateJson(stateArgs);
	}
	return makeJsonResult(result);
}

JsonNode CMcpPlayerInterface::makeBattleStateJson() const
{
	JsonNode state;
	state["active"] = JsonNode(false);
	state["battles"].Vector();
	if(!cb)
		return state;

	BattleID activeID = BattleID::NONE;
	const CStack * activeStack = nullptr;
	BattleID tacticsID = BattleID::NONE;
	{
		std::lock_guard lock(interfaceMutex);
		activeID = activeBattleID;
		activeStack = activeStackToMove;
		tacticsID = activeTacticsBattleID;
	}

	std::shared_lock gameStateLock(CGameState::mutex);
	const auto activeBattles = cb->getActiveBattles();
	state["active"] = JsonNode(!activeBattles.empty());
	state["activeBattleId"] = activeID == BattleID::NONE ? JsonNode() : JsonNode(activeID.getNum());
	state["activeTacticsBattleId"] = tacticsID == BattleID::NONE ? JsonNode() : JsonNode(tacticsID.getNum());

	for(const auto & [battleID, battle] : activeBattles)
	{
		if(!battle)
			continue;

		const IBattleInfo * info = battle->getBattle();
		JsonNode battleNode;
		battleNode["battle_id"] = JsonNode(battleID.getNum());
		battleNode["round"] = JsonNode(info->getRound());
		battleNode["location"] = jsonPosition(info->getLocation());
		battleNode["mySide"] = JsonNode(battleSideName(battle->battleGetMySide()));
		battleNode["activeStackId"] = JsonNode(info->getActiveStackID());
		battleNode["tactics"]["distance"] = JsonNode(static_cast<int32_t>(battle->battleTacticDist()));
		battleNode["tactics"]["side"] = JsonNode(battleSideName(battle->battleGetTacticsSide()));
		battleNode["canFlee"] = JsonNode(battle->battleCanFlee());
		battleNode["surrenderCost"] = JsonNode(battle->battleGetSurrenderCost());
		battleNode["sides"]["attacker"]["player"] = JsonNode(info->getSidePlayer(BattleSide::ATTACKER).toString());
		battleNode["sides"]["defender"]["player"] = JsonNode(info->getSidePlayer(BattleSide::DEFENDER).toString());

		const CGHeroInstance * myHero = battle->battleGetMyHero();
		if(myHero)
			battleNode["myHero"] = jsonHero(myHero);

		battleNode["stacks"].Vector();
		for(const CStack * stack : battle->battleGetStacks(CBattleInfoEssentials::MINE_AND_ENEMY, false))
		{
			if(stack)
				battleNode["stacks"].Vector().push_back(jsonBattleStack(*battle, stack));
		}

		const CStack * legalActionStack = nullptr;
		if(activeID == battleID && activeStack)
			legalActionStack = activeStack;
		else if(info->getActiveStackID() >= 0)
			legalActionStack = battle->battleGetStackByID(info->getActiveStackID(), false);

		if(legalActionStack && battle->battleGetOwner(legalActionStack) == playerID)
			battleNode["legalActions"] = jsonBattleLegalActions(*battle, legalActionStack);

		state["battles"].Vector().push_back(battleNode);
	}

	return state;
}

void CMcpPlayerInterface::performAutoBattleAction(const BattleID & battleID, const CStack * stack)
{
	if(!cb || !stack)
		return;

	auto battle = cb->getBattle(battleID);
	if(!battle || battle->battleIsFinished())
		return;
	if(battle->battleGetOwner(stack) != playerID)
		return;

	for(const CStack * target : battle->battleGetStacks(CBattleInfoEssentials::ONLY_ENEMY, false))
	{
		if(target && battle->battleCanShoot(stack, target->getPosition()))
		{
			cb->battleMakeUnitAction(battleID, BattleAction::makeShotAttack(stack, target));
			return;
		}
	}

	const BattleHexArray availableHexes = battle->battleGetAvailableHexes(stack, false);
	for(const CStack * target : battle->battleGetStacks(CBattleInfoEssentials::ONLY_ENEMY, false))
	{
		if(!target || !battle->battleCanAttackUnit(stack, target))
			continue;

		for(const BattleHex::EDir direction : BattleHex::hexagonalDirections())
		{
			if(!battle->battleCanAttackHex(availableHexes, stack, target->getPosition(), direction))
				continue;

			const BattleHex attackFrom = battle->fromWhichHexAttack(stack, target->getPosition(), direction);
			if(attackFrom.isValid())
			{
				cb->battleMakeUnitAction(battleID, BattleAction::makeMeleeAttack(stack, target, attackFrom));
				return;
			}
		}
	}

	if(!stack->waitedThisTurn)
		cb->battleMakeUnitAction(battleID, BattleAction::makeWait(stack));
	else
		cb->battleMakeUnitAction(battleID, BattleAction::makeDefend(stack));
}

std::string CMcpPlayerInterface::makeAgentGuideText() const
{
	return
		"VCMI MCP agent guide\n"
		"1. Call vcmi.get_state with selectors and vcmi.get_reachable for route-id movement planning.\n"
		"2. Prefer vcmi.execute_plan to submit a day plan with builds, recruitment, movement, queries, and end_turn.\n"
		"3. Use only ids, route_id values, and coordinates returned by MCP payloads.\n"
		"4. Movement results mean the server applied or rejected the request; inspect structured failures before retrying.\n"
		"5. Battle is auto-handled by the MCP interface. Focus on adventure-map and town planning.\n"
		"6. Use vcmi.get_updates with the last revision instead of rereading full state after every small action.\n";
}

std::string CMcpPlayerInterface::makeStateText() const
{
	return makeStateJson().toCompactString();
}

JsonNode CMcpPlayerInterface::makePendingQueryJson() const
{
	JsonNode node;
	if(!pendingQuery)
		return node;

	node = pendingQuery->data;
	node["id"] = JsonNode(pendingQuery->id.getNum());
	node["type"] = JsonNode(pendingQuery->type);
	return node;
}

Mcp::Protocol::ToolResult CMcpPlayerInterface::makeOkResult(const std::string & message) const
{
	JsonNode result;
	result["ok"] = JsonNode(true);
	result["message"] = JsonNode(message);
	return makeJsonResult(result);
}

Mcp::Protocol::ToolResult CMcpPlayerInterface::makeJsonResult(const JsonNode & node) const
{
	return Mcp::Protocol::makeTextResult(node.toCompactString());
}

Mcp::Protocol::ToolResult CMcpPlayerInterface::makeToolError(const std::string & message) const
{
	return Mcp::Protocol::makeTextResult(message, true);
}

Mcp::Protocol::ToolResult CMcpPlayerInterface::traceToolResult(const std::string & toolName, const JsonNode & arguments, Mcp::Protocol::ToolResult result) const
{
	JsonNode event;
	event["event"] = JsonNode("tool_call");
	event["player"] = JsonNode(playerID.toString());
	event["tool"] = JsonNode(toolName);
	event["arguments"] = arguments;
	event["result"] = result.result;
	appendTraceEvent(event);
	return result;
}

void CMcpPlayerInterface::appendTraceEvent(JsonNode event) const
{
	if(tracePath.empty())
		return;

	std::lock_guard lock(traceMutex);
	event["sequence"] = JsonNode(static_cast<int64_t>(++traceSequence));
	event["time"] = JsonNode(timestampUtc());

	std::ofstream traceFile(tracePath, std::ios::app);
	if(traceFile)
		traceFile << toJsonLine(event) << '\n';
}

void CMcpPlayerInterface::setPendingQuery(QueryID queryID, const std::string & type, JsonNode data)
{
	std::lock_guard lock(interfaceMutex);
	pendingQuery = PendingQuery{queryID, type, std::move(data)};

	JsonNode event;
	event["event"] = JsonNode("pending_query_set");
	event["player"] = JsonNode(playerID.toString());
	event["query_id"] = JsonNode(queryID.getNum());
	event["type"] = JsonNode(type);
	event["data"] = pendingQuery->data;
	appendTraceEvent(event);

	JsonNode update;
	update["query_id"] = JsonNode(queryID.getNum());
	update["queryType"] = JsonNode(type);
	update["data"] = pendingQuery->data;
	appendStateUpdate("query.pending", update);
}

void CMcpPlayerInterface::clearPendingQuery(QueryID queryID)
{
	std::lock_guard lock(interfaceMutex);
	if(pendingQuery && pendingQuery->id == queryID)
		pendingQuery.reset();

	JsonNode event;
	event["event"] = JsonNode("pending_query_cleared");
	event["player"] = JsonNode(playerID.toString());
	event["query_id"] = JsonNode(queryID.getNum());
	appendTraceEvent(event);

	JsonNode update;
	update["query_id"] = JsonNode(queryID.getNum());
	appendStateUpdate("query.cleared", update);
}

void CMcpPlayerInterface::yourTurn(QueryID queryID)
{
	{
		std::lock_guard lock(interfaceMutex);
		turnActive = true;
	}

	JsonNode event;
	event["event"] = JsonNode("turn_started");
	event["player"] = JsonNode(playerID.toString());
	if(queryID.hasValue())
		event["query_id"] = JsonNode(queryID.getNum());
	appendTraceEvent(event);

	JsonNode update;
	update["player"] = JsonNode(playerID.toString());
	if(queryID.hasValue())
		update["query_id"] = JsonNode(queryID.getNum());
	appendStateUpdate("turn.started", update);

	if(queryID.hasValue())
		cb->selectionMade(0, queryID);
}

void CMcpPlayerInterface::yourTacticPhase(const BattleID & battleID, int distance)
{
	JsonNode event;
	event["event"] = JsonNode("tactics_phase_started");
	event["player"] = JsonNode(playerID.toString());
	event["battle_id"] = JsonNode(battleID.getNum());
	event["distance"] = JsonNode(distance);
	appendTraceEvent(event);

	JsonNode update;
	update["battle_id"] = JsonNode(battleID.getNum());
	update["distance"] = JsonNode(distance);
	appendStateUpdate("battle.tacticsStarted", update);

	cb->battleMakeTacticAction(battleID, BattleAction::makeEndOFTacticPhase(cb->getBattle(battleID)->battleGetTacticsSide()));
}

void CMcpPlayerInterface::activeStack(const BattleID & battleID, const CStack * stack)
{
	JsonNode event;
	event["event"] = JsonNode("battle_stack_active");
	event["player"] = JsonNode(playerID.toString());
	event["battle_id"] = JsonNode(battleID.getNum());
	if(stack)
	{
		event["stack"]["id"] = JsonNode(static_cast<int32_t>(stack->unitId()));
		event["stack"]["name"] = JsonNode(jsonText(stack->getName()));
	}
	appendTraceEvent(event);

	JsonNode update;
	update["battle_id"] = JsonNode(battleID.getNum());
	if(stack)
		update["stack"] = jsonBattleStack(*cb->getBattle(battleID), stack);
	appendStateUpdate("battle.stackAutoAction", update);
	performAutoBattleAction(battleID, stack);
}

void CMcpPlayerInterface::battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed)
{
	JsonNode event;
	event["event"] = JsonNode("battle_started");
	event["player"] = JsonNode(playerID.toString());
	event["battle_id"] = JsonNode(battleID.getNum());
	event["tile"] = jsonPosition(tile);
	event["side"] = JsonNode(battleSideName(side));
	event["replayAllowed"] = JsonNode(replayAllowed);
	event["attackerHeroId"] = jsonObjectId(hero1);
	event["defenderHeroId"] = jsonObjectId(hero2);
	event["attackerArmySlots"] = JsonNode(army1 ? static_cast<int32_t>(army1->Slots().size()) : 0);
	event["defenderArmySlots"] = JsonNode(army2 ? static_cast<int32_t>(army2->Slots().size()) : 0);
	appendTraceEvent(event);
	appendStateUpdate("battle.started", event);
}

void CMcpPlayerInterface::battleEnd(const BattleID & battleID, const BattleResult * br, QueryID queryID)
{
	{
		std::lock_guard lock(interfaceMutex);
		if(activeBattleID == battleID)
		{
			activeBattleID = BattleID::NONE;
			activeStackToMove = nullptr;
		}
		if(activeTacticsBattleID == battleID)
			activeTacticsBattleID = BattleID::NONE;
	}

	JsonNode event;
	event["event"] = JsonNode("battle_ended");
	event["player"] = JsonNode(playerID.toString());
	event["battle_id"] = JsonNode(battleID.getNum());
	if(queryID.hasValue())
		event["query_id"] = JsonNode(queryID.getNum());
	if(br)
	{
		event["result"] = JsonNode(static_cast<int32_t>(br->result));
		event["winner"] = JsonNode(battleSideName(br->winner));
		event["attacker"] = JsonNode(br->attacker.toString());
	}
	appendTraceEvent(event);
	appendStateUpdate("battle.ended", event);
}

void CMcpPlayerInterface::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID)
{
	JsonNode data;
	data["heroId"] = JsonNode(hero ? hero->id.getNum() : ObjectInstanceID::NONE.getNum());
	data["primarySkill"] = JsonNode(pskill.getNum());
	data["skills"].Vector();
	for(const SecondarySkill & skill : skills)
		data["skills"].Vector().push_back(JsonNode(skill.getNum()));

	setPendingQuery(queryID, "hero_level_up", data);
}

void CMcpPlayerInterface::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID)
{
	JsonNode data;
	data["skills"].Vector();
	for(ui32 skill : skills)
		data["skills"].Vector().push_back(JsonNode(static_cast<int32_t>(skill)));

	setPendingQuery(queryID, "commander_level_up", data);
}

void CMcpPlayerInterface::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	JsonNode data;
	data["text"] = JsonNode(jsonText(text));
	data["components"] = JsonNode(static_cast<int32_t>(components.size()));
	data["selection"] = JsonNode(selection);
	data["cancel"] = JsonNode(cancel);
	data["safeToAutoaccept"] = JsonNode(safeToAutoaccept);
	setPendingQuery(askID, "blocking_dialog", data);
}

void CMcpPlayerInterface::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	JsonNode data;
	data["heroId"] = JsonNode(hero ? hero->id.getNum() : ObjectInstanceID::NONE.getNum());
	data["channel"] = JsonNode(channel.getNum());
	data["impassable"] = JsonNode(impassable);
	data["exits"].Vector();
	for(const auto & exit : exits)
	{
		JsonNode exitNode;
		exitNode["objectId"] = JsonNode(exit.first.getNum());
		exitNode["position"] = jsonPosition(exit.second);
		data["exits"].Vector().push_back(exitNode);
	}
	setPendingQuery(askID, "teleport_dialog", data);
}

void CMcpPlayerInterface::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	JsonNode data;
	data["upperArmyId"] = jsonObjectId(up);
	data["lowerHeroId"] = jsonObjectId(down);
	data["removableUnits"] = JsonNode(removableUnits);
	data["title"] = JsonNode(jsonText(customTitle.toString()));
	setPendingQuery(queryID, "garrison_dialog", data);
}

void CMcpPlayerInterface::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	JsonNode data;
	data["title"] = JsonNode(jsonText(title.toString()));
	data["description"] = JsonNode(jsonText(description.toString()));
	data["objects"].Vector();
	for(const ObjectInstanceID & object : objects)
		data["objects"].Vector().push_back(JsonNode(object.getNum()));
	setPendingQuery(askID, "map_object_select_dialog", data);
}

std::optional<BattleAction> CMcpPlayerInterface::makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState)
{
	return std::nullopt;
}

void CMcpPlayerInterface::buildChanged(const CGTownInstance * town, BuildingID buildingID, int what)
{
	JsonNode data;
	data["town_id"] = JsonNode(town ? town->id.getNum() : ObjectInstanceID::NONE.getNum());
	if(town)
		data["town"] = jsonTown(town);
	data["building_id"] = JsonNode(buildingID.getNum());
	data["change"] = JsonNode(what == 1 ? "built" : what == 2 ? "demolished" : "changed");
	appendStateUpdate("town.buildingChanged", data);
}

void CMcpPlayerInterface::heroMoved(const TryMoveHero & details, bool verbose)
{
	JsonNode data;
	data["hero_id"] = JsonNode(details.id.getNum());
	data["start"] = jsonPosition(details.start);
	data["end"] = jsonPosition(details.end);
	data["result"] = JsonNode(static_cast<int32_t>(details.result));
	if(const CGHeroInstance * hero = cb ? cb->getHero(details.id) : nullptr)
		data["hero"] = jsonHero(hero);
	appendStateUpdate("hero.moved", data);
}

void CMcpPlayerInterface::heroCreated(const CGHeroInstance * hero)
{
	JsonNode data;
	if(hero)
		data["hero"] = jsonHero(hero);
	appendStateUpdate("hero.created", data);
}

void CMcpPlayerInterface::heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start)
{
	JsonNode data;
	if(visitor)
		data["hero"] = jsonHero(visitor);
	if(visitedObj)
		data["object"] = jsonMapObject(visitedObj, playerID, visitor);
	data["start"] = JsonNode(start);
	appendStateUpdate("hero.visit", data);
}

void CMcpPlayerInterface::heroManaPointsChanged(const CGHeroInstance * hero)
{
	JsonNode data;
	if(hero)
		data["hero"] = jsonHero(hero);
	appendStateUpdate("hero.manaChanged", data);
}

void CMcpPlayerInterface::heroMovePointsChanged(const CGHeroInstance * hero)
{
	JsonNode data;
	if(hero)
		data["hero"] = jsonHero(hero);
	appendStateUpdate("hero.movementChanged", data);
}

void CMcpPlayerInterface::tileHidden(const FowTilesType & pos)
{
	JsonNode data;
	data["tiles"].Vector();
	for(const int3 & tile : pos)
		data["tiles"].Vector().push_back(jsonPosition(tile));
	appendStateUpdate("visibility.hidden", data);
}

void CMcpPlayerInterface::tileRevealed(const FowTilesType & pos)
{
	JsonNode data;
	data["tiles"].Vector();
	for(const int3 & tile : pos)
		data["tiles"].Vector().push_back(jsonPosition(tile));
	appendStateUpdate("visibility.revealed", data);
}

void CMcpPlayerInterface::availableCreaturesChanged(const CGDwelling * town)
{
	JsonNode data;
	if(town)
	{
		data["source_id"] = JsonNode(town->id.getNum());
		data["recruitOptions"].Vector();
		for(int32_t level = 0; level < static_cast<int32_t>(town->creatures.size()); ++level)
		{
			JsonNode option = jsonRecruitOption(town, town, level);
			if(option.isStruct())
				data["recruitOptions"].Vector().push_back(option);
		}
	}
	appendStateUpdate("recruitment.availableChanged", data);
}

void CMcpPlayerInterface::beforeObjectPropertyChanged(const SetObjectProperty * sop)
{
}

void CMcpPlayerInterface::objectPropertyChanged(const SetObjectProperty * sop)
{
	JsonNode data;
	if(sop)
	{
		data["object_id"] = JsonNode(sop->id.getNum());
		data["property"] = JsonNode(static_cast<int32_t>(sop->what));
		data["identifier"] = JsonNode(sop->identifier.getNum());
		if(cb)
		{
			const CGObjectInstance * object = cb->getObj(sop->id, false);
			if(object)
				data["object"] = jsonMapObject(object, playerID, nullptr);
		}
	}
	appendStateUpdate("object.changed", data);
}

void CMcpPlayerInterface::objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator)
{
	JsonNode data;
	if(obj)
		data["object"] = jsonMapObject(obj, playerID, nullptr);
	data["initiator"] = JsonNode(initiator.toString());
	appendStateUpdate("object.removed", data);
}

void CMcpPlayerInterface::playerStartsTurn(PlayerColor player)
{
	JsonNode data;
	data["player"] = JsonNode(player.toString());
	appendStateUpdate("player.turnStarted", data);
}

void CMcpPlayerInterface::playerEndsTurn(PlayerColor player)
{
	JsonNode data;
	data["player"] = JsonNode(player.toString());
	appendStateUpdate("player.turnEnded", data);
}

void CMcpPlayerInterface::requestSent(const CPackForServer * pack, int requestID)
{
	JsonNode data;
	data["requestId"] = JsonNode(requestID);
	if(pack)
		data["type"] = JsonNode(typeid(*pack).name());
	appendStateUpdate("request.sent", data);

	std::lock_guard lock(requestMutex);
	if(pendingRequest && pack && pendingRequest->requestID < 0 && pendingRequest->typeName == typeid(*pack).name())
	{
		pendingRequest->requestID = requestID;
		requestCv.notify_all();
	}
}

void CMcpPlayerInterface::requestRealized(PackageApplied * pa)
{
	JsonNode data;
	if(pa)
	{
		data["requestId"] = JsonNode(static_cast<int32_t>(pa->requestID));
		data["packType"] = JsonNode(static_cast<int32_t>(pa->packType));
		data["result"] = JsonNode(pa->result);
	}
	appendStateUpdate("request.realized", data);

	std::lock_guard lock(requestMutex);
	if(pendingRequest && pa && pendingRequest->requestID == static_cast<int>(pa->requestID))
	{
		pendingRequest->packType = pa->packType;
		pendingRequest->realized = true;
		pendingRequest->applied = pa->result;
		requestCv.notify_all();
	}
}
