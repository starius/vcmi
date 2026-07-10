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
#include "../../lib/UnlockGuard.h"
#include "../../lib/VCMIDirs.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CTown.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/gameState/UpgradeInfo.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/mapObjects/CQuest.h"
#include "../../lib/mapObjects/CGDwelling.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGObjectInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/IMarket.h"
#include "../../lib/mapObjects/IObjectInterface.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/entities/artifact/ArtifactUtils.h"
#include "../../lib/entities/artifact/CArtifact.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/mapping/TerrainTile.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../lib/networkPacks/SaveLocalState.h"
#include "../../lib/pathfinder/CGPathNode.h"
#include "../../lib/pathfinder/PathfinderOptions.h"
#include "../../lib/serializer/CTypeList.h"
#include "../../lib/spells/CSpell.h"
#include "../../lib/spells/CSpellHandler.h"
#include "../../lib/spells/Problem.h"
#include "../../lib/spells/adventure/AdventureSpellMechanics.h"
#include "../../luascript/LuaAdventureScriptRunner.h"
#include "../Nullkiller2/Analyzers/DangerHitMapAnalyzer.h"
#include "../Nullkiller2/Analyzers/ObjectClusterizer.h"
#include "../Nullkiller2/Goals/AdventureSpellCast.h"
#include "../Nullkiller2/Goals/BuildBoat.h"
#include "../Nullkiller2/Goals/BuildThis.h"
#include "../Nullkiller2/Goals/Composition.h"
#include "../Nullkiller2/Goals/ExecuteHeroChain.h"
#include "../Nullkiller2/Goals/StayAtTown.h"
#include "../Nullkiller2/Markers/ArmyUpgrade.h"
#include "../Nullkiller2/Markers/DefendTown.h"
#include "../Nullkiller2/Markers/HeroExchange.h"
#include "../Nullkiller2/Markers/UnlockCluster.h"

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

enum class ScriptBuildingKind : int32_t
{
	UNKNOWN = 0,
	MAGE_GUILD = 1,
	TAVERN = 2,
	SHIPYARD = 3,
	FORTIFICATION = 4,
	HALL = 5,
	MARKET = 6,
	RESOURCE_SILO = 7,
	BLACKSMITH = 8,
	SPECIAL = 9,
	HORDE = 10,
	DWELLING = 11,
	GRAIL = 12,
	SHIP = 13
};

enum class ScriptObjectKind : int32_t
{
	UNKNOWN = 0,
	TREASURE = 1,
	RESOURCE = 2,
	MINE = 3,
	ARTIFACT = 4,
	TOWN = 5,
	HERO = 6,
	CREATURE_BANK = 7,
	DWELLING = 8,
	MONSTER = 9,
	TELEPORT = 10,
	SHRINE = 11,
	VISIT_BONUS = 12,
	MARKET = 13,
	QUEST = 14
};

enum class ScriptArmyTransferKind : int32_t
{
	UNKNOWN = 0,
	GATHER_TO_HERO = 1,
	REINFORCE_TOWN = 2
};

enum class ScriptThreatLevel : int32_t
{
	UNKNOWN = 0,
	WATCH = 1,
	HIGH = 2,
	CRITICAL = 3
};

enum class ScriptRiskLevel : int32_t
{
	NONE = 0,
	ACCEPTABLE = 1,
	RISKY = 2,
	HIGH = 3,
	CRITICAL = 4
};

const char * scriptBuildingKindName(ScriptBuildingKind kind)
{
	switch(kind)
	{
	case ScriptBuildingKind::MAGE_GUILD:
		return "mage_guild";
	case ScriptBuildingKind::TAVERN:
		return "tavern";
	case ScriptBuildingKind::SHIPYARD:
		return "shipyard";
	case ScriptBuildingKind::FORTIFICATION:
		return "fortification";
	case ScriptBuildingKind::HALL:
		return "hall";
	case ScriptBuildingKind::MARKET:
		return "market";
	case ScriptBuildingKind::RESOURCE_SILO:
		return "resource_silo";
	case ScriptBuildingKind::BLACKSMITH:
		return "blacksmith";
	case ScriptBuildingKind::SPECIAL:
		return "special";
	case ScriptBuildingKind::HORDE:
		return "horde";
	case ScriptBuildingKind::DWELLING:
		return "dwelling";
	case ScriptBuildingKind::GRAIL:
		return "grail";
	case ScriptBuildingKind::SHIP:
		return "ship";
	default:
		return "unknown";
	}
}

const char * scriptObjectKindName(ScriptObjectKind kind)
{
	switch(kind)
	{
	case ScriptObjectKind::TREASURE:
		return "treasure";
	case ScriptObjectKind::RESOURCE:
		return "resource";
	case ScriptObjectKind::MINE:
		return "mine";
	case ScriptObjectKind::ARTIFACT:
		return "artifact";
	case ScriptObjectKind::TOWN:
		return "town";
	case ScriptObjectKind::HERO:
		return "hero";
	case ScriptObjectKind::CREATURE_BANK:
		return "creature_bank";
	case ScriptObjectKind::DWELLING:
		return "dwelling";
	case ScriptObjectKind::MONSTER:
		return "monster";
	case ScriptObjectKind::TELEPORT:
		return "teleport";
	case ScriptObjectKind::SHRINE:
		return "shrine";
	case ScriptObjectKind::VISIT_BONUS:
		return "visit_bonus";
	case ScriptObjectKind::MARKET:
		return "market";
	case ScriptObjectKind::QUEST:
		return "quest";
	default:
		return "unknown";
	}
}

const char * scriptArmyTransferKindName(ScriptArmyTransferKind kind)
{
	switch(kind)
	{
	case ScriptArmyTransferKind::GATHER_TO_HERO:
		return "gather_to_hero";
	case ScriptArmyTransferKind::REINFORCE_TOWN:
		return "reinforce_town";
	default:
		return "unknown";
	}
}

const char * scriptThreatLevelName(ScriptThreatLevel level)
{
	switch(level)
	{
	case ScriptThreatLevel::WATCH:
		return "watch";
	case ScriptThreatLevel::HIGH:
		return "high";
	case ScriptThreatLevel::CRITICAL:
		return "critical";
	default:
		return "unknown";
	}
}

const char * scriptRiskLevelName(ScriptRiskLevel level)
{
	switch(level)
	{
	case ScriptRiskLevel::NONE:
		return "none";
	case ScriptRiskLevel::ACCEPTABLE:
		return "acceptable";
	case ScriptRiskLevel::RISKY:
		return "risky";
	case ScriptRiskLevel::HIGH:
		return "high";
	case ScriptRiskLevel::CRITICAL:
		return "critical";
	default:
		return "unknown";
	}
}

ScriptBuildingKind scriptBuildingKind(BuildingID buildingID)
{
	if(buildingID.isDwelling())
		return ScriptBuildingKind::DWELLING;

	switch(buildingID.toEnum())
	{
	case BuildingID::MAGES_GUILD_1:
	case BuildingID::MAGES_GUILD_2:
	case BuildingID::MAGES_GUILD_3:
	case BuildingID::MAGES_GUILD_4:
	case BuildingID::MAGES_GUILD_5:
		return ScriptBuildingKind::MAGE_GUILD;
	case BuildingID::TAVERN:
		return ScriptBuildingKind::TAVERN;
	case BuildingID::SHIPYARD:
		return ScriptBuildingKind::SHIPYARD;
	case BuildingID::FORT:
	case BuildingID::CITADEL:
	case BuildingID::CASTLE:
		return ScriptBuildingKind::FORTIFICATION;
	case BuildingID::VILLAGE_HALL:
	case BuildingID::TOWN_HALL:
	case BuildingID::CITY_HALL:
	case BuildingID::CAPITOL:
	case BuildingID::EXTRA_TOWN_HALL:
	case BuildingID::EXTRA_CITY_HALL:
	case BuildingID::EXTRA_CAPITOL:
		return ScriptBuildingKind::HALL;
	case BuildingID::MARKETPLACE:
		return ScriptBuildingKind::MARKET;
	case BuildingID::RESOURCE_SILO:
		return ScriptBuildingKind::RESOURCE_SILO;
	case BuildingID::BLACKSMITH:
		return ScriptBuildingKind::BLACKSMITH;
	case BuildingID::SPECIAL_1:
	case BuildingID::SPECIAL_2:
	case BuildingID::SPECIAL_3:
	case BuildingID::SPECIAL_4:
		return ScriptBuildingKind::SPECIAL;
	case BuildingID::HORDE_1:
	case BuildingID::HORDE_1_UPGR:
	case BuildingID::HORDE_2:
	case BuildingID::HORDE_2_UPGR:
		return ScriptBuildingKind::HORDE;
	case BuildingID::GRAIL:
		return ScriptBuildingKind::GRAIL;
	case BuildingID::SHIP:
		return ScriptBuildingKind::SHIP;
	default:
		return ScriptBuildingKind::UNKNOWN;
	}
}

int32_t scriptBuildingLevel(BuildingID buildingID)
{
	if(buildingID.isDwelling())
		return BuildingID::getLevelIndexFromDwelling(buildingID) + 1;

	switch(buildingID.toEnum())
	{
	case BuildingID::MAGES_GUILD_1:
	case BuildingID::MAGES_GUILD_2:
	case BuildingID::MAGES_GUILD_3:
	case BuildingID::MAGES_GUILD_4:
	case BuildingID::MAGES_GUILD_5:
		return buildingID.getMagesGuildLevel();
	case BuildingID::VILLAGE_HALL:
		return 1;
	case BuildingID::TOWN_HALL:
	case BuildingID::EXTRA_TOWN_HALL:
		return 2;
	case BuildingID::CITY_HALL:
	case BuildingID::EXTRA_CITY_HALL:
		return 3;
	case BuildingID::CAPITOL:
	case BuildingID::EXTRA_CAPITOL:
		return 4;
	case BuildingID::FORT:
		return 1;
	case BuildingID::CITADEL:
		return 2;
	case BuildingID::CASTLE:
		return 3;
	default:
		return 0;
	}
}

int32_t scriptBuildingUpgrade(BuildingID buildingID)
{
	if(buildingID.isDwelling())
		return BuildingID::getUpgradeNoFromDwelling(buildingID);
	return 0;
}

ScriptObjectKind scriptObjectKind(MapObjectID objectID)
{
	switch(objectID)
	{
	case Obj::RESOURCE:
	case Obj::RANDOM_RESOURCE:
		return ScriptObjectKind::RESOURCE;
	case Obj::TREASURE_CHEST:
	case Obj::SEA_CHEST:
	case Obj::CAMPFIRE:
	case Obj::FLOTSAM:
	case Obj::SHIPWRECK_SURVIVOR:
	case Obj::WAGON:
	case Obj::LEAN_TO:
	case Obj::CORPSE:
		return ScriptObjectKind::TREASURE;
	case Obj::MINE:
	case Obj::ABANDONED_MINE:
		return ScriptObjectKind::MINE;
	case Obj::ARTIFACT:
	case Obj::SPELL_SCROLL:
	case Obj::RANDOM_ART:
	case Obj::RANDOM_TREASURE_ART:
	case Obj::RANDOM_MINOR_ART:
	case Obj::RANDOM_MAJOR_ART:
	case Obj::RANDOM_RELIC_ART:
		return ScriptObjectKind::ARTIFACT;
	case Obj::TOWN:
	case Obj::RANDOM_TOWN:
		return ScriptObjectKind::TOWN;
	case Obj::HERO:
	case Obj::HERO_PLACEHOLDER:
	case Obj::RANDOM_HERO:
	case Obj::PRISON:
		return ScriptObjectKind::HERO;
	case Obj::CREATURE_BANK:
	case Obj::CRYPT:
	case Obj::DERELICT_SHIP:
	case Obj::DRAGON_UTOPIA:
	case Obj::PYRAMID:
	case Obj::SHIPWRECK:
		return ScriptObjectKind::CREATURE_BANK;
	case Obj::CREATURE_GENERATOR1:
	case Obj::CREATURE_GENERATOR2:
	case Obj::CREATURE_GENERATOR3:
	case Obj::CREATURE_GENERATOR4:
	case Obj::RANDOM_DWELLING:
	case Obj::RANDOM_DWELLING_LVL:
	case Obj::RANDOM_DWELLING_FACTION:
	case Obj::REFUGEE_CAMP:
		return ScriptObjectKind::DWELLING;
	case Obj::MONSTER:
	case Obj::RANDOM_MONSTER:
	case Obj::RANDOM_MONSTER_L1:
	case Obj::RANDOM_MONSTER_L2:
	case Obj::RANDOM_MONSTER_L3:
	case Obj::RANDOM_MONSTER_L4:
	case Obj::RANDOM_MONSTER_L5:
	case Obj::RANDOM_MONSTER_L6:
	case Obj::RANDOM_MONSTER_L7:
		return ScriptObjectKind::MONSTER;
	case Obj::MONOLITH_ONE_WAY_ENTRANCE:
	case Obj::MONOLITH_ONE_WAY_EXIT:
	case Obj::MONOLITH_TWO_WAY:
	case Obj::SUBTERRANEAN_GATE:
	case Obj::WHIRLPOOL:
		return ScriptObjectKind::TELEPORT;
	case Obj::SHRINE_OF_MAGIC_INCANTATION:
	case Obj::SHRINE_OF_MAGIC_GESTURE:
	case Obj::SHRINE_OF_MAGIC_THOUGHT:
		return ScriptObjectKind::SHRINE;
	case Obj::ARENA:
	case Obj::MARLETTO_TOWER:
	case Obj::MERCENARY_CAMP:
	case Obj::SCHOOL_OF_MAGIC:
	case Obj::SCHOOL_OF_WAR:
	case Obj::STAR_AXIS:
	case Obj::GARDEN_OF_REVELATION:
	case Obj::LEARNING_STONE:
	case Obj::TREE_OF_KNOWLEDGE:
	case Obj::LIBRARY_OF_ENLIGHTENMENT:
	case Obj::STABLES:
	case Obj::MAGIC_WELL:
	case Obj::MAGIC_SPRING:
	case Obj::FOUNTAIN_OF_FORTUNE:
	case Obj::FOUNTAIN_OF_YOUTH:
	case Obj::OASIS:
	case Obj::BUOY:
	case Obj::IDOL_OF_FORTUNE:
	case Obj::RALLY_FLAG:
	case Obj::SWAN_POND:
	case Obj::FAERIE_RING:
	case Obj::MERMAID:
	case Obj::MYSTICAL_GARDEN:
	case Obj::WATER_WHEEL:
	case Obj::WATERING_HOLE:
	case Obj::WINDMILL:
	case Obj::WITCH_HUT:
		return ScriptObjectKind::VISIT_BONUS;
	case Obj::ALTAR_OF_SACRIFICE:
	case Obj::BLACK_MARKET:
	case Obj::FREELANCERS_GUILD:
	case Obj::HILL_FORT:
	case Obj::MARKET_OF_TIME:
	case Obj::SHIPYARD:
	case Obj::TAVERN:
	case Obj::TRADING_POST:
	case Obj::TRADING_POST_SNOW:
	case Obj::UNIVERSITY:
	case Obj::WAR_MACHINE_FACTORY:
		return ScriptObjectKind::MARKET;
	case Obj::BORDERGUARD:
	case Obj::BORDER_GATE:
	case Obj::HUT_OF_MAGI:
	case Obj::KEYMASTER:
	case Obj::OBELISK:
	case Obj::QUEST_GUARD:
	case Obj::SEER_HUT:
		return ScriptObjectKind::QUEST;
	default:
		return ScriptObjectKind::UNKNOWN;
	}
}

std::string mapObjectIdentifier(MapObjectID objectID)
{
	try
	{
		return MapObjectID::encode(objectID.getNum());
	}
	catch(const std::exception &)
	{
		return std::to_string(objectID.getNum());
	}
}

std::string mapObjectSubtypeIdentifier(MapObjectID objectID, MapObjectSubID subtypeID)
{
	try
	{
		return MapObjectSubID::encode(objectID, subtypeID.getNum());
	}
	catch(const std::exception &)
	{
		return std::to_string(subtypeID.getNum());
	}
}

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

ArtifactLocation readArtifactLocation(const JsonNode & node, const std::string & field)
{
	const JsonNode & location = hasField(node, field) ? node[field] : node;
	if(!location.isStruct())
		throw std::invalid_argument("Artifact location must be an object: " + field);

	ArtifactLocation result(
		ObjectInstanceID(readInteger(location, "holder_id")),
		ArtifactPosition(readInteger(location, "slot"))
	);
	if(hasField(location, "creature_slot"))
		result.creature = SlotID(readInteger(location, "creature_slot"));
	return result;
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

NK2AI::ScriptTaskSearchMode readNullkillerTaskSearchMode(const JsonNode & node, const NK2AI::ScriptTaskSearchMode defaultValue = NK2AI::ScriptTaskSearchMode::ALL)
{
	if(!hasField(node, "mode"))
		return defaultValue;

	if(node["mode"].isNumber())
	{
		switch(readInteger(node, "mode"))
		{
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::PRIORITY):
			return NK2AI::ScriptTaskSearchMode::PRIORITY;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::ADVENTURE):
			return NK2AI::ScriptTaskSearchMode::ADVENTURE;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::ALL):
			return NK2AI::ScriptTaskSearchMode::ALL;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::RECRUIT_HERO):
			return NK2AI::ScriptTaskSearchMode::RECRUIT_HERO;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::BUY_ARMY):
			return NK2AI::ScriptTaskSearchMode::BUY_ARMY;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::BUILDING):
			return NK2AI::ScriptTaskSearchMode::BUILDING;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::CAPTURE):
			return NK2AI::ScriptTaskSearchMode::CAPTURE;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::CLUSTER):
			return NK2AI::ScriptTaskSearchMode::CLUSTER;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::DEFENSE):
			return NK2AI::ScriptTaskSearchMode::DEFENSE;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::ESCAPE):
			return NK2AI::ScriptTaskSearchMode::ESCAPE;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::GATHER_ARMY):
			return NK2AI::ScriptTaskSearchMode::GATHER_ARMY;
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::EXPLORATION):
			return NK2AI::ScriptTaskSearchMode::EXPLORATION;
		default:
			throw std::invalid_argument("Unsupported Nullkiller task search mode id");
		}
	}

	if(!node["mode"].isString())
		throw std::invalid_argument("Nullkiller task search mode must be a string or integer");

	const std::string mode = toLowerAscii(node["mode"].String());
	if(mode == "priority" || mode == "support")
		return NK2AI::ScriptTaskSearchMode::PRIORITY;
	if(mode == "adventure" || mode == "map")
		return NK2AI::ScriptTaskSearchMode::ADVENTURE;
	if(mode == "all")
		return NK2AI::ScriptTaskSearchMode::ALL;
	if(mode == "recruit_hero" || mode == "recruit-hero" || mode == "recruithero")
		return NK2AI::ScriptTaskSearchMode::RECRUIT_HERO;
	if(mode == "buy_army" || mode == "buy-army" || mode == "buyarmy")
		return NK2AI::ScriptTaskSearchMode::BUY_ARMY;
	if(mode == "building" || mode == "buildings" || mode == "build")
		return NK2AI::ScriptTaskSearchMode::BUILDING;
	if(mode == "capture" || mode == "capture_objects" || mode == "capture-objects")
		return NK2AI::ScriptTaskSearchMode::CAPTURE;
	if(mode == "cluster" || mode == "clusters")
		return NK2AI::ScriptTaskSearchMode::CLUSTER;
	if(mode == "defense" || mode == "defence")
		return NK2AI::ScriptTaskSearchMode::DEFENSE;
	if(mode == "escape")
		return NK2AI::ScriptTaskSearchMode::ESCAPE;
	if(mode == "gather_army" || mode == "gather-army" || mode == "gatherarmy")
		return NK2AI::ScriptTaskSearchMode::GATHER_ARMY;
	if(mode == "exploration" || mode == "explore")
		return NK2AI::ScriptTaskSearchMode::EXPLORATION;

	throw std::invalid_argument("Unsupported Nullkiller task search mode: " + mode);
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

bool canExposeMarketDetails(const CGObjectInstance * object, PlayerColor player)
{
	return object->tempOwner == player
		|| object->tempOwner == PlayerColor::NEUTRAL
		|| object->tempOwner == PlayerColor::UNFLAGGABLE;
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

std::string componentTypeName(ComponentType type);
std::string marketModeName(EMarketMode mode);

JsonNode jsonPosition(const int3 & position)
{
	JsonNode node;
	node["x"] = JsonNode(position.x);
	node["y"] = JsonNode(position.y);
	node["z"] = JsonNode(position.z);
	return node;
}

JsonNode jsonComponent(const Component & component)
{
	JsonNode node;
	node["typeId"] = JsonNode(static_cast<int32_t>(component.type));
	node["type"] = JsonNode(componentTypeName(component.type));
	node["subtypeId"] = JsonNode(component.subType.getNum());
	if(component.value)
		node["value"] = JsonNode(*component.value);
	return node;
}

JsonNode jsonComponents(const std::vector<Component> & components)
{
	JsonNode node;
	node.Vector();
	for(const Component & component : components)
		node.Vector().push_back(jsonComponent(component));
	return node;
}

JsonNode jsonTradeItemBuy(const TradeItemBuy & item)
{
	JsonNode node;
	node["id"] = JsonNode(item.getNum());

	const GameResID resource = item.as<GameResID>();
	if(resource.hasValue() && resource.getNum() >= 0 && resource.getNum() < static_cast<int32_t>(GameConstants::RESOURCE_QUANTITY))
	{
		node["kindId"] = JsonNode(0);
		node["kind"] = JsonNode("resource");
		node["resource_id"] = JsonNode(resource.getNum());
		return node;
	}

	const PlayerColor player = item.as<PlayerColor>();
	if(player.isValidPlayer())
	{
		node["kindId"] = JsonNode(1);
		node["kind"] = JsonNode("player");
		node["player_id"] = JsonNode(player.getNum());
		return node;
	}

	const ArtifactID artifact = item.as<ArtifactID>();
	if(artifact.hasValue())
	{
		node["kindId"] = JsonNode(2);
		node["kind"] = JsonNode("artifact");
		node["artifact_id"] = JsonNode(artifact.getNum());
		return node;
	}

	const SecondarySkill skill = item.as<SecondarySkill>();
	if(skill.hasValue())
	{
		node["kindId"] = JsonNode(3);
		node["kind"] = JsonNode("secondary_skill");
		node["skill_id"] = JsonNode(skill.getNum());
		return node;
	}

	node["kindId"] = JsonNode(-1);
	node["kind"] = JsonNode("unknown");
	return node;
}

JsonNode jsonResourceMarketRates(const IMarket * market)
{
	JsonNode node;
	node.Vector();
	if(!market || !market->allowsTrade(EMarketMode::RESOURCE_RESOURCE))
		return node;

	for(size_t sell = 0; sell < GameConstants::RESOURCE_QUANTITY; ++sell)
	{
		for(size_t buy = 0; buy < GameConstants::RESOURCE_QUANTITY; ++buy)
		{
			if(sell == buy)
				continue;

			int give = 0;
			int receive = 0;
			if(!market->getOffer(static_cast<int>(sell), static_cast<int>(buy), give, receive, EMarketMode::RESOURCE_RESOURCE))
				continue;

			JsonNode offer;
			offer["sell_resource_id"] = JsonNode(static_cast<int32_t>(sell));
			offer["buy_resource_id"] = JsonNode(static_cast<int32_t>(buy));
			offer["give"] = JsonNode(give);
			offer["receive"] = JsonNode(receive);
			node.Vector().push_back(offer);
		}
	}
	return node;
}

JsonNode jsonMarketModeDetails(const IMarket * market, EMarketMode mode)
{
	JsonNode node;
	node["modeId"] = JsonNode(static_cast<int32_t>(mode));
	node["mode"] = JsonNode(marketModeName(mode));
	node["items"].Vector();

	for(const TradeItemBuy & item : market->availableItemsIds(mode))
	{
		JsonNode itemNode = jsonTradeItemBuy(item);
		itemNode["availableUnits"] = JsonNode(market->availableUnits(mode, item.getNum()));
		node["items"].Vector().push_back(itemNode);
	}

	if(mode == EMarketMode::RESOURCE_RESOURCE)
		node["resourceRates"] = jsonResourceMarketRates(market);

	return node;
}

JsonNode jsonVisibleTile(const int3 & position, const TerrainTile & tile)
{
	JsonNode node;
	node["position"] = jsonPosition(position);
	node["terrainId"] = JsonNode(tile.getTerrainID().getNum());
	node["riverId"] = JsonNode(tile.getRiverID().getNum());
	node["roadId"] = JsonNode(tile.getRoadID().getNum());
	node["blocked"] = JsonNode(tile.blocked());
	node["visitable"] = JsonNode(tile.visitable());
	node["water"] = JsonNode(tile.isWater());
	node["land"] = JsonNode(tile.isLand());
	node["favorableWinds"] = JsonNode(tile.hasFavorableWinds());
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

JsonNode jsonArtifactPosition(ArtifactPosition position)
{
	JsonNode node;
	node["id"] = JsonNode(position.getNum());
	try
	{
		node["identifier"] = JsonNode(ArtifactPosition::encode(position.getNum()));
	}
	catch(const std::exception &)
	{
		node["identifier"] = JsonNode(std::to_string(position.getNum()));
	}
	return node;
}

JsonNode jsonArtifactLocation(const ArtifactLocation & location)
{
	JsonNode node;
	node["holder_id"] = JsonNode(location.artHolder.getNum());
	node["slot"] = JsonNode(location.slot.getNum());
	node["slotInfo"] = jsonArtifactPosition(location.slot);
	if(location.creature)
		node["creature_slot"] = JsonNode(location.creature->getNum());
	return node;
}

JsonNode jsonArtifactSlot(ObjectInstanceID holderID, ArtifactPosition position, const ArtSlotInfo & slotInfo, bool backpack)
{
	JsonNode node;
	node["holder_id"] = JsonNode(holderID.getNum());
	node["slot"] = JsonNode(position.getNum());
	node["slotInfo"] = jsonArtifactPosition(position);
	node["backpack"] = JsonNode(backpack);
	node["locked"] = JsonNode(slotInfo.locked);

	if(const CArtifactInstance * artifact = slotInfo.getArt())
	{
		node["artifactInstanceId"] = JsonNode(artifact->getId().getNum());
		node["artifactTypeId"] = JsonNode(artifact->getTypeId().getNum());
		if(const CArtifact * artifactType = artifact->getType())
		{
			node["artifactIdentifier"] = JsonNode(artifactType->getJsonKey());
			node["artifactName"] = JsonNode(jsonText(artifactType->getNameTranslated()));
			node["combined"] = JsonNode(artifact->isCombined());
			node["scroll"] = JsonNode(artifact->isScroll());
			node["possibleSlots"].Vector();
			if(const auto possibleSlots = artifactType->getPossibleSlots().find(ArtBearer::HERO); possibleSlots != artifactType->getPossibleSlots().end())
			{
				for(const ArtifactPosition possibleSlot : possibleSlots->second)
					node["possibleSlots"].Vector().push_back(jsonArtifactPosition(possibleSlot));
			}
		}
	}

	return node;
}

JsonNode jsonArtifactAssemblyPrompt(QueryID decisionID, const CGHeroInstance * hero, const ArtifactLocation & destination)
{
	JsonNode data;
	data["scriptDecision"] = JsonNode(true);
	data["answerableByQueryReply"] = JsonNode(false);
	data["location"] = jsonArtifactLocation(destination);
	data["holder_id"] = JsonNode(destination.artHolder.getNum());
	data["hero_id"] = JsonNode(hero->id.getNum());
	data["slot"] = JsonNode(destination.slot.getNum());
	data["assemblyOptions"].Vector();
	data["ignoreAction"]["type"] = JsonNode("ignore_script_query");
	data["ignoreAction"]["query_id"] = JsonNode(decisionID.getNum());

	const CArtifactInstance * artifact = hero->getArt(destination.slot);
	if(artifact)
	{
		data["artifactInstanceId"] = JsonNode(artifact->getId().getNum());
		data["artifactTypeId"] = JsonNode(artifact->getTypeId().getNum());
		if(const CArtifact * artifactType = artifact->getType())
		{
			data["artifactIdentifier"] = JsonNode(artifactType->getJsonKey());
			data["artifactName"] = JsonNode(jsonText(artifactType->getNameTranslated()));
		}

		for(const CArtifact * combinedArtifact : ArtifactUtils::assemblyPossibilities(hero, artifact->getTypeId()))
		{
			if(!combinedArtifact)
				continue;

			JsonNode option;
			option["artifact_id"] = JsonNode(combinedArtifact->getId().getNum());
			option["artifactIdentifier"] = JsonNode(combinedArtifact->getJsonKey());
			option["artifactName"] = JsonNode(jsonText(combinedArtifact->getNameTranslated()));
			option["fused"] = JsonNode(combinedArtifact->isFused());
			option["planAction"]["type"] = JsonNode("assemble_artifacts");
			option["planAction"]["hero_id"] = data["hero_id"];
			option["planAction"]["slot"] = data["slot"];
			option["planAction"]["assemble"] = JsonNode(true);
			option["planAction"]["artifact_id"] = option["artifact_id"];
			data["assemblyOptions"].Vector().push_back(option);
		}
	}

	return data;
}

JsonNode jsonArtifacts(const CGHeroInstance * hero)
{
	JsonNode node;
	node["worn"].Vector();
	node["backpack"].Vector();

	for(const auto & [position, slotInfo] : hero->artifactsWorn)
	{
		if(slotInfo.getArt())
			node["worn"].Vector().push_back(jsonArtifactSlot(hero->id, position, slotInfo, false));
	}

	for(size_t index = 0; index < hero->artifactsInBackpack.size(); ++index)
	{
		const ArtSlotInfo & slotInfo = hero->artifactsInBackpack[index];
		if(slotInfo.getArt())
			node["backpack"].Vector().push_back(jsonArtifactSlot(hero->id, ArtifactPosition::BACKPACK_START + static_cast<int>(index), slotInfo, true));
	}

	return node;
}

JsonNode jsonKnownSpell(const CGHeroInstance * hero, const CSpell * spell)
{
	JsonNode node;
	node["spell_id"] = JsonNode(spell->getId().getNum());
	node["spellIdentifier"] = JsonNode(spell->getJsonKey());
	node["spellName"] = JsonNode(jsonText(spell->getNameTranslated()));
	node["level"] = JsonNode(spell->getLevel());
	node["adventure"] = JsonNode(spell->isAdventure());
	node["combat"] = JsonNode(spell->isCombat());
	node["schoolLevel"] = JsonNode(hero->getSpellSchoolLevel(spell));
	node["cost"] = JsonNode(hero->getSpellCost(spell));
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

int32_t scriptPathActionId(EPathNodeAction action)
{
	return static_cast<int32_t>(action);
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

std::string marketModeName(EMarketMode mode)
{
	switch(mode)
	{
	case EMarketMode::RESOURCE_RESOURCE:
		return "resource_resource";
	case EMarketMode::RESOURCE_PLAYER:
		return "resource_player";
	case EMarketMode::CREATURE_RESOURCE:
		return "creature_resource";
	case EMarketMode::RESOURCE_ARTIFACT:
		return "resource_artifact";
	case EMarketMode::ARTIFACT_RESOURCE:
		return "artifact_resource";
	case EMarketMode::ARTIFACT_EXP:
		return "artifact_experience";
	case EMarketMode::CREATURE_EXP:
		return "creature_experience";
	case EMarketMode::CREATURE_UNDEAD:
		return "creature_undead";
	case EMarketMode::RESOURCE_SKILL:
		return "resource_skill";
	case EMarketMode::MARKET_AFTER_LAST_PLACEHOLDER:
		break;
	}
	return "unknown";
}

std::string diggingStatusName(EDiggingStatus status)
{
	switch(status)
	{
	case EDiggingStatus::UNKNOWN:
		return "unknown";
	case EDiggingStatus::CAN_DIG:
		return "can_dig";
	case EDiggingStatus::LACK_OF_MOVEMENT:
		return "lack_of_movement";
	case EDiggingStatus::WRONG_TERRAIN:
		return "wrong_terrain";
	case EDiggingStatus::TILE_OCCUPIED:
		return "tile_occupied";
	case EDiggingStatus::BACKPACK_IS_FULL:
		return "backpack_is_full";
	}
	return "unknown";
}

std::string shipyardStatusName(IBoatGenerator::EGeneratorState status)
{
	switch(status)
	{
	case IBoatGenerator::GOOD:
		return "good";
	case IBoatGenerator::BOAT_ALREADY_BUILT:
		return "boat_already_built";
	case IBoatGenerator::TILE_BLOCKED:
		return "tile_blocked";
	case IBoatGenerator::NO_WATER:
		return "no_water";
	case IBoatGenerator::UNKNOWN:
		return "unknown";
	}
	return "unknown";
}

std::string armyFormationName(EArmyFormation formation)
{
	switch(formation)
	{
	case EArmyFormation::LOOSE:
		return "loose";
	case EArmyFormation::TIGHT:
		return "tight";
	}
	return "unknown";
}

std::string componentTypeName(ComponentType type)
{
	switch(type)
	{
	case ComponentType::NONE:
		return "none";
	case ComponentType::PRIM_SKILL:
		return "primary_skill";
	case ComponentType::SEC_SKILL:
		return "secondary_skill";
	case ComponentType::RESOURCE:
		return "resource";
	case ComponentType::RESOURCE_PER_DAY:
		return "resource_per_day";
	case ComponentType::CREATURE:
		return "creature";
	case ComponentType::ARTIFACT:
		return "artifact";
	case ComponentType::SPELL_SCROLL:
		return "spell_scroll";
	case ComponentType::MANA:
		return "mana";
	case ComponentType::EXPERIENCE:
		return "experience";
	case ComponentType::LEVEL:
		return "level";
	case ComponentType::SPELL:
		return "spell";
	case ComponentType::MORALE:
		return "morale";
	case ComponentType::LUCK:
		return "luck";
	case ComponentType::BUILDING:
		return "building";
	case ComponentType::HERO_PORTRAIT:
		return "hero_portrait";
	case ComponentType::FLAG:
		return "flag";
	}
	return "unknown";
}

std::string nullkillerTaskSearchModeName(NK2AI::ScriptTaskSearchMode mode)
{
	switch(mode)
	{
	case NK2AI::ScriptTaskSearchMode::PRIORITY:
		return "priority";
	case NK2AI::ScriptTaskSearchMode::ADVENTURE:
		return "adventure";
	case NK2AI::ScriptTaskSearchMode::ALL:
		return "all";
	case NK2AI::ScriptTaskSearchMode::RECRUIT_HERO:
		return "recruit_hero";
	case NK2AI::ScriptTaskSearchMode::BUY_ARMY:
		return "buy_army";
	case NK2AI::ScriptTaskSearchMode::BUILDING:
		return "building";
	case NK2AI::ScriptTaskSearchMode::CAPTURE:
		return "capture";
	case NK2AI::ScriptTaskSearchMode::CLUSTER:
		return "cluster";
	case NK2AI::ScriptTaskSearchMode::DEFENSE:
		return "defense";
	case NK2AI::ScriptTaskSearchMode::ESCAPE:
		return "escape";
	case NK2AI::ScriptTaskSearchMode::GATHER_ARMY:
		return "gather_army";
	case NK2AI::ScriptTaskSearchMode::EXPLORATION:
		return "exploration";
	}
	return "unknown";
}

std::string nullkillerTaskFailureActionName(NK2AI::TaskFailureAction action)
{
	switch(action)
	{
	case NK2AI::TaskFailureAction::TRY_NEXT_TASK:
		return "try_next_task";
	case NK2AI::TaskFailureAction::REPLAN:
		return "replan";
	case NK2AI::TaskFailureAction::STOP_TURN:
		return "stop_turn";
	}
	return "unknown";
}

std::string nullkillerHeroRoleName(NK2AI::HeroRole role)
{
	switch(role)
	{
	case NK2AI::SCOUT:
		return "scout";
	case NK2AI::MAIN:
		return "main";
	}
	return "unknown";
}

std::string nullkillerGoalName(NK2AI::Goals::EGoals goal)
{
	switch(goal)
	{
	case NK2AI::Goals::INVALID:
		return "invalid";
	case NK2AI::Goals::WIN:
		return "win";
	case NK2AI::Goals::CONQUER:
		return "conquer";
	case NK2AI::Goals::BUILD:
		return "build";
	case NK2AI::Goals::EXPLORE:
		return "explore";
	case NK2AI::Goals::GATHER_ARMY:
		return "gather_army";
	case NK2AI::Goals::BOOST_HERO:
		return "boost_hero";
	case NK2AI::Goals::RECRUIT_HERO:
		return "recruit_hero";
	case NK2AI::Goals::RECRUIT_HERO_BEHAVIOR:
		return "recruit_hero_behavior";
	case NK2AI::Goals::BUILD_STRUCTURE:
		return "build_structure";
	case NK2AI::Goals::COLLECT_RES:
		return "collect_res";
	case NK2AI::Goals::GATHER_TROOPS:
		return "gather_troops";
	case NK2AI::Goals::CAPTURE_OBJECTS:
		return "capture_objects";
	case NK2AI::Goals::GET_ART_TYPE:
		return "get_art_type";
	case NK2AI::Goals::DEFENCE:
		return "defence";
	case NK2AI::Goals::STARTUP:
		return "startup";
	case NK2AI::Goals::DIG_AT_TILE:
		return "dig_at_tile";
	case NK2AI::Goals::BUY_ARMY:
		return "buy_army";
	case NK2AI::Goals::TRADE:
		return "trade";
	case NK2AI::Goals::BUILD_BOAT:
		return "build_boat";
	case NK2AI::Goals::COMPLETE_QUEST:
		return "complete_quest";
	case NK2AI::Goals::ADVENTURE_SPELL_CAST:
		return "adventure_spell_cast";
	case NK2AI::Goals::EXECUTE_HERO_CHAIN:
		return "execute_hero_chain";
	case NK2AI::Goals::EXCHANGE_SWAP_TOWN_HEROES:
		return "exchange_swap_town_heroes";
	case NK2AI::Goals::DISMISS_HERO:
		return "dismiss_hero";
	case NK2AI::Goals::COMPOSITION:
		return "composition";
	case NK2AI::Goals::CLUSTER_BEHAVIOR:
		return "cluster_behavior";
	case NK2AI::Goals::UNLOCK_CLUSTER:
		return "unlock_cluster";
	case NK2AI::Goals::HERO_EXCHANGE:
		return "hero_exchange";
	case NK2AI::Goals::ARMY_UPGRADE:
		return "army_upgrade";
	case NK2AI::Goals::DEFEND_TOWN:
		return "defend_town";
	case NK2AI::Goals::CAPTURE_OBJECT:
		return "capture_object";
	case NK2AI::Goals::SAVE_RESOURCES:
		return "save_resources";
	case NK2AI::Goals::STAY_AT_TOWN_BEHAVIOR:
		return "stay_at_town_behavior";
	case NK2AI::Goals::STAY_AT_TOWN:
		return "stay_at_town";
	case NK2AI::Goals::EXPLORATION_BEHAVIOR:
		return "exploration_behavior";
	case NK2AI::Goals::ESCAPE_BEHAVIOR:
		return "escape_behavior";
	case NK2AI::Goals::EXPLORATION_POINT:
		return "exploration_point";
	case NK2AI::Goals::EXPLORE_NEIGHBOUR_TILE:
		return "explore_neighbour_tile";
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

std::vector<int3> visibleMapTiles(const std::shared_ptr<CCallback> & cc, PlayerColor playerID)
{
	std::vector<int3> result;
	if(!cc)
		return result;

	const int3 mapSize = cc->getMapSize();
	result.reserve(static_cast<size_t>(mapSize.x) * static_cast<size_t>(mapSize.y) * static_cast<size_t>(mapSize.z) / 4);
	for(int32_t z = 0; z < mapSize.z; ++z)
	{
		for(int32_t x = 0; x < mapSize.x; ++x)
		{
			for(int32_t y = 0; y < mapSize.y; ++y)
			{
				const int3 position(x, y, z);
				if(cc->isVisibleFor(position, playerID))
					result.push_back(position);
			}
		}
	}
	return result;
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
	node["pathActionId"] = JsonNode(scriptPathActionId(pathNode.action));
	node["pathAction"] = JsonNode(pathActionName(pathNode.action));
	node["isTeleportAction"] = JsonNode(pathNode.isTeleportAction());
	return node;
}

ScriptRiskLevel scriptRiskLevel(uint64_t danger, double dangerRatio, bool safe)
{
	if(danger == 0)
		return ScriptRiskLevel::NONE;
	if(safe)
		return ScriptRiskLevel::ACCEPTABLE;
	if(dangerRatio >= 1.5)
		return ScriptRiskLevel::CRITICAL;
	if(dangerRatio >= 1.0)
		return ScriptRiskLevel::HIGH;
	return ScriptRiskLevel::RISKY;
}

ScriptThreatLevel scriptThreatLevel(double strengthRatio)
{
	if(strengthRatio >= 1.5)
		return ScriptThreatLevel::CRITICAL;
	if(strengthRatio >= 1.0)
		return ScriptThreatLevel::HIGH;
	return ScriptThreatLevel::WATCH;
}

JsonNode jsonRisk(const CGHeroInstance * hero, uint64_t danger, bool safe)
{
	const int64_t heroStrength = hero ? static_cast<int64_t>(hero->getArmyStrength()) : 0;
	const double dangerRatio = static_cast<double>(danger) / std::max(1.0, static_cast<double>(heroStrength));
	const ScriptRiskLevel riskLevel = scriptRiskLevel(danger, dangerRatio, safe);

	JsonNode node;
	node["danger"] = JsonNode(static_cast<int64_t>(danger));
	node["heroStrength"] = JsonNode(heroStrength);
	node["dangerRatio"] = JsonNode(dangerRatio);
	node["safe"] = JsonNode(safe);
	node["riskId"] = JsonNode(static_cast<int32_t>(riskLevel));
	node["risk"] = JsonNode(scriptRiskLevelName(riskLevel));
	node["estimatedLoss"] = JsonNode(safe ? 0 : static_cast<int64_t>(danger));
	return node;
}

bool hasVisibleThreatHero(const NK2AI::HitMapInfo & info, const std::shared_ptr<CCallback> & cc, PlayerColor player)
{
	if(!cc || !info.heroPtr.isVerified(false))
		return false;

	const CGHeroInstance * hero = info.heroPtr.getUnverified();
	return hero && cc->isVisibleFor(hero, player);
}

JsonNode jsonVisibleHitMapInfo(const NK2AI::HitMapInfo & info, const std::shared_ptr<CCallback> & cc, PlayerColor player)
{
	JsonNode node;
	const bool visible = hasVisibleThreatHero(info, cc, player);
	node["hasVisibleThreat"] = JsonNode(visible);
	if(!visible)
		return node;

	const CGHeroInstance * hero = info.heroPtr.getUnverified();
	node["danger"] = JsonNode(static_cast<int64_t>(info.danger));
	node["turn"] = JsonNode(static_cast<int32_t>(info.turn));
	node["threat"] = JsonNode(static_cast<double>(info.threat));
	node["value"] = JsonNode(info.value());
	node["enemyHeroId"] = JsonNode(hero->id.getNum());
	node["enemyHero"] = JsonNode(jsonText(hero->getNameTranslated()));
	node["enemyPosition"] = jsonPosition(hero->visitablePos());
	node["enemyStrength"] = JsonNode(static_cast<int64_t>(hero->getArmyStrength()));
	return node;
}

bool hasVisibleThreat(const NK2AI::HitMapNode & node, const std::shared_ptr<CCallback> & cc, PlayerColor player)
{
	return hasVisibleThreatHero(node.fastestDanger, cc, player) || hasVisibleThreatHero(node.maximumDanger, cc, player);
}

JsonNode jsonNullkillerPath(const NK2AI::AIPath & path, size_t maxNodes = 16)
{
	JsonNode node;
	node["nodeLimit"] = JsonNode(static_cast<int32_t>(maxNodes));
	node["nodeCount"] = JsonNode(static_cast<int32_t>(path.nodes.size()));
	node["nodesTruncated"] = JsonNode(path.nodes.size() > maxNodes);
	node["nodes"].Vector();
	node["exchangeCount"] = JsonNode(static_cast<int32_t>(path.exchangeCount));
	node["chainMask"] = JsonNode(static_cast<int64_t>(path.chainMask));
	node["targetObjectDanger"] = JsonNode(static_cast<int64_t>(path.targetObjectDanger));
	node["armyLoss"] = JsonNode(static_cast<int64_t>(path.armyLoss));
	node["targetObjectArmyLoss"] = JsonNode(static_cast<int64_t>(path.targetObjectArmyLoss));
	if(path.targetHero)
	{
		node["hero_id"] = JsonNode(path.targetHero->id.getNum());
		node["heroStrength"] = JsonNode(static_cast<int64_t>(path.getHeroStrength()));
	}
	if(!path.nodes.empty())
	{
		node["firstTile"] = jsonPosition(path.firstTileToGet());
		node["targetTile"] = jsonPosition(path.targetTile());
		node["movementCost"] = JsonNode(static_cast<double>(path.movementCost()));
		node["turn"] = JsonNode(static_cast<int32_t>(path.turn()));
		node["pathDanger"] = JsonNode(static_cast<int64_t>(path.getPathDanger()));
		node["totalDanger"] = JsonNode(static_cast<int64_t>(path.getTotalDanger()));
		node["totalArmyLoss"] = JsonNode(static_cast<int64_t>(path.getTotalArmyLoss()));
	}

	size_t count = 0;
	for(const NK2AI::AIPathNodeInfo & pathNode : path.nodes)
	{
		if(count++ >= maxNodes)
			break;

		JsonNode pathNodeJson;
		pathNodeJson["position"] = jsonPosition(pathNode.coord);
		pathNodeJson["cost"] = JsonNode(static_cast<double>(pathNode.cost));
		pathNodeJson["turn"] = JsonNode(static_cast<int32_t>(pathNode.turns));
		pathNodeJson["layer_id"] = JsonNode(static_cast<int32_t>(pathNode.layer));
		pathNodeJson["danger"] = JsonNode(static_cast<int64_t>(pathNode.danger));
		pathNodeJson["parentIndex"] = JsonNode(pathNode.parentIndex);
		pathNodeJson["chainMask"] = JsonNode(static_cast<int64_t>(pathNode.chainMask));
		pathNodeJson["actionIsBlocked"] = JsonNode(pathNode.actionIsBlocked);
		pathNodeJson["hasSpecialAction"] = JsonNode(static_cast<bool>(pathNode.specialAction));
		if(pathNode.targetHero)
			pathNodeJson["hero_id"] = JsonNode(pathNode.targetHero->id.getNum());
		if(pathNode.specialAction)
		{
			if(const CGObjectInstance * target = pathNode.specialAction->targetObject())
			{
				pathNodeJson["specialActionTargetObjectId"] = JsonNode(target->id.getNum());
				pathNodeJson["specialActionTargetPosition"] = jsonPosition(target->visitablePos());
			}
		}
		node["nodes"].Vector().push_back(pathNodeJson);
	}
	return node;
}

JsonNode jsonNullkillerGoalSummary(const NK2AI::Goals::AbstractGoal & goal, const std::shared_ptr<CCallback> & cc, PlayerColor playerID, int depth);

JsonNode jsonNullkillerGoalDetails(const NK2AI::Goals::AbstractGoal & goal, const std::shared_ptr<CCallback> & cc, PlayerColor playerID, int depth)
{
	JsonNode details;

	if(const auto * spellCast = dynamic_cast<const NK2AI::Goals::AdventureSpellCast *>(&goal))
	{
		if(const CSpell * spell = spellCast->getSpell())
		{
			details["spell_id"] = JsonNode(spell->getId().getNum());
			details["spell_identifier"] = JsonNode(spell->getJsonKey());
		}
	}

	if(const auto * buildThis = dynamic_cast<const NK2AI::Goals::BuildThis *>(&goal))
	{
		const NK2AI::BuildingInfo & building = buildThis->buildingInfo;
		details["buildCost"] = jsonResources(building.buildCost);
		details["buildCostWithPrerequisites"] = jsonResources(building.buildCostWithPrerequisites);
		details["dailyIncome"] = jsonResources(building.dailyIncome);
		details["armyCost"] = jsonResources(building.armyCost);
		details["creatureGrowth"] = JsonNode(building.creatureGrowth);
		details["creatureLevel"] = JsonNode(static_cast<int32_t>(building.creatureLevel));
		details["creature_id"] = JsonNode(building.creatureID.getNum());
		details["base_creature_id"] = JsonNode(building.baseCreatureID.getNum());
		details["prerequisitesCount"] = JsonNode(static_cast<int32_t>(building.prerequisitesCount));
		details["armyStrength"] = JsonNode(static_cast<int64_t>(building.armyStrength));
		details["isBuilt"] = JsonNode(building.isBuilt);
		details["isBuildable"] = JsonNode(building.isBuildable);
		details["isMissingResources"] = JsonNode(building.isMissingResources);
	}

	if(const auto * buildBoat = dynamic_cast<const NK2AI::Goals::BuildBoat *>(&goal))
	{
		if(const IShipyard * shipyard = buildBoat->getShipyard())
		{
			ResourceSet boatCost;
			shipyard->getBoatCost(boatCost);
			details["boatCost"] = jsonResources(boatCost);
			details["boatTypeId"] = JsonNode(shipyard->getBoatType().getNum());
			details["shipyardStatusId"] = JsonNode(static_cast<int32_t>(shipyard->shipyardStatus()));
			details["bestBoatLocation"] = jsonPosition(shipyard->bestLocation());
			if(const auto * shipyardObject = dynamic_cast<const CGObjectInstance *>(shipyard->getObject()))
			{
				if(!cc || cc->isVisibleFor(shipyardObject, playerID))
				{
					details["shipyard_id"] = JsonNode(shipyardObject->id.getNum());
					details["shipyardPosition"] = jsonPosition(shipyardObject->visitablePos());
				}
			}
		}
	}

	if(const auto * executeChain = dynamic_cast<const NK2AI::Goals::ExecuteHeroChain *>(&goal))
		details["path"] = jsonNullkillerPath(executeChain->getPath());

	if(const auto * heroExchange = dynamic_cast<const NK2AI::Goals::HeroExchange *>(&goal))
		details["exchangePath"] = jsonNullkillerPath(heroExchange->exchangePath);

	if(const auto * unlockCluster = dynamic_cast<const NK2AI::Goals::UnlockCluster *>(&goal))
	{
		if(const std::shared_ptr<NK2AI::ObjectCluster> & cluster = unlockCluster->getCluster())
		{
			if(cluster->blocker && (!cc || cc->isVisibleFor(cluster->blocker, playerID)))
			{
				details["blocker_id"] = JsonNode(cluster->blocker->id.getNum());
				details["blockerPosition"] = jsonPosition(cluster->blocker->visitablePos());
				details["blockerObjectTypeId"] = JsonNode(cluster->blocker->ID.getNum());
			}
		}
		details["pathToCenter"] = jsonNullkillerPath(unlockCluster->getPathToCenter());
	}

	if(const auto * defendTown = dynamic_cast<const NK2AI::Goals::DefendTown *>(&goal))
	{
		details["defenceStrength"] = JsonNode(static_cast<int64_t>(defendTown->getDefenceStrength()));
		details["defenceTurn"] = JsonNode(static_cast<int32_t>(defendTown->getTurn()));
		details["counterattack"] = JsonNode(defendTown->isCounterAttack());
		details["visibleThreat"] = jsonVisibleHitMapInfo(defendTown->getThreat(), cc, playerID);
	}

	if(const auto * armyUpgrade = dynamic_cast<const NK2AI::Goals::ArmyUpgrade *>(&goal))
	{
		details["initialArmyValue"] = JsonNode(static_cast<int64_t>(armyUpgrade->getInitialArmyValue()));
		details["upgradeValue"] = JsonNode(static_cast<int64_t>(armyUpgrade->getUpgradeValue()));
		details["goldCost"] = JsonNode(static_cast<int64_t>(armyUpgrade->getGoldCost()));
		if(const CGObjectInstance * upgrader = armyUpgrade->getUpgrader())
		{
			if(!cc || cc->isVisibleFor(upgrader, playerID))
			{
				details["upgrader_id"] = JsonNode(upgrader->id.getNum());
				details["upgraderPosition"] = jsonPosition(upgrader->visitablePos());
			}
		}
	}

	if(const auto * stayAtTown = dynamic_cast<const NK2AI::Goals::StayAtTown *>(&goal))
		details["movementWasted"] = JsonNode(static_cast<double>(stayAtTown->getMovementWasted()));

	if(const auto * composition = dynamic_cast<const NK2AI::Goals::Composition *>(&goal))
	{
		constexpr size_t maxSequences = 8;
		constexpr size_t maxGoalsPerSequence = 8;
		const auto & sequences = composition->getSubtasks();
		details["subtaskSequenceLimit"] = JsonNode(static_cast<int32_t>(maxSequences));
		details["subtaskSequenceCount"] = JsonNode(static_cast<int32_t>(sequences.size()));
		details["subtaskSequencesTruncated"] = JsonNode(sequences.size() > maxSequences);
		details["subtaskSequences"].Vector();
		if(depth > 0)
		{
			size_t sequenceCount = 0;
			for(const NK2AI::Goals::TGoalVec & sequence : sequences)
			{
				if(sequenceCount++ >= maxSequences)
					break;

				JsonNode sequenceJson;
				sequenceJson["goalLimit"] = JsonNode(static_cast<int32_t>(maxGoalsPerSequence));
				sequenceJson["goalCount"] = JsonNode(static_cast<int32_t>(sequence.size()));
				sequenceJson["goalsTruncated"] = JsonNode(sequence.size() > maxGoalsPerSequence);
				sequenceJson["goals"].Vector();

				size_t goalCount = 0;
				for(const NK2AI::Goals::TSubgoal & subgoal : sequence)
				{
					if(goalCount++ >= maxGoalsPerSequence)
						break;
					if(subgoal)
						sequenceJson["goals"].Vector().push_back(jsonNullkillerGoalSummary(*subgoal, cc, playerID, depth - 1));
				}
				details["subtaskSequences"].Vector().push_back(sequenceJson);
			}
		}
	}

	return details;
}

JsonNode jsonNullkillerGoalSummary(const NK2AI::Goals::AbstractGoal & goal, const std::shared_ptr<CCallback> & cc, PlayerColor playerID, int depth)
{
	JsonNode node;
	node["goalTypeId"] = JsonNode(static_cast<int32_t>(goal.goalType));
	node["goalType"] = JsonNode(nullkillerGoalName(goal.goalType));
	node["isElementar"] = JsonNode(goal.isElementar());
	node["value"] = JsonNode(goal.value);
	if(goal.hero)
		node["hero_id"] = JsonNode(goal.hero->id.getNum());
	if(goal.town)
		node["town_id"] = JsonNode(goal.town->id.getNum());
	if(goal.objid >= 0)
		node["object_id"] = JsonNode(goal.objid);
	if(goal.bid >= 0)
		node["building_id"] = JsonNode(goal.bid);
	if(goal.aid >= 0)
		node["artifact_id"] = JsonNode(goal.aid);
	if(goal.resID >= 0)
		node["resource_id"] = JsonNode(goal.resID);
	if(goal.tile.x >= 0 && goal.tile.y >= 0 && goal.tile.z >= 0)
		node["tile"] = jsonPosition(goal.tile);
	if(goal.goldCost > 0)
		node["goldCost"] = JsonNode(static_cast<int64_t>(goal.goldCost));
	node["buildingCost"] = jsonResources(goal.buildingCost);

	if(const auto * task = dynamic_cast<const NK2AI::Goals::ITask *>(&goal))
	{
		node["priority"].Float() = task->priority;
		node["affectedObjectIds"].Vector();
		for(const ObjectInstanceID objectID : task->getAffectedObjects())
			node["affectedObjectIds"].Vector().push_back(JsonNode(objectID.getNum()));
		node["heroExchangeCount"] = JsonNode(task->getHeroExchangeCount());
	}

	node["details"] = jsonNullkillerGoalDetails(goal, cc, playerID, depth);
	return node;
}

int32_t questMissionId(const CQuest & quest)
{
	for(int32_t index = static_cast<int32_t>(EQuestMission::NONE); index <= static_cast<int32_t>(EQuestMission::HOTA_SCRIPTED); ++index)
	{
		const auto mission = static_cast<EQuestMission>(index);
		if(quest.questName == CQuest::missionName(mission))
			return index;
	}
	return static_cast<int32_t>(EQuestMission::NONE);
}

JsonNode jsonCreatureRequirement(const CStackBasicDescriptor & creature)
{
	JsonNode node;
	node["creature_id"] = JsonNode(creature.getId().getNum());
	node["amount"] = JsonNode(static_cast<int32_t>(creature.getCount()));
	return node;
}

JsonNode jsonQuestLimiter(const Rewardable::Limiter & limiter, int32_t depth = 0)
{
	JsonNode node;
	node["heroLevel"] = JsonNode(limiter.heroLevel);
	node["heroExperience"] = JsonNode(limiter.heroExperience);
	node["manaPoints"] = JsonNode(limiter.manaPoints);
	node["manaPercentage"] = JsonNode(limiter.manaPercentage);
	node["movePoints"] = JsonNode(limiter.movePoints);
	node["movePercentage"] = JsonNode(limiter.movePercentage);
	node["canLearnSkills"] = JsonNode(limiter.canLearnSkills);
	node["commanderAlive"] = JsonNode(limiter.commanderAlive);
	node["hasExtraCreatures"] = JsonNode(limiter.hasExtraCreatures);
	node["resources"] = jsonResources(limiter.resources);

	node["primarySkills"].Vector();
	for(size_t index = 0; index < limiter.primary.size(); ++index)
	{
		if(limiter.primary[index] <= 0)
			continue;
		JsonNode skill;
		skill["skill_id"] = JsonNode(static_cast<int32_t>(index));
		skill["amount"] = JsonNode(limiter.primary[index]);
		node["primarySkills"].Vector().push_back(skill);
	}

	node["secondarySkills"].Vector();
	for(const auto & [skillID, level] : limiter.secondary)
	{
		JsonNode skill;
		skill["skill_id"] = JsonNode(skillID.getNum());
		skill["level"] = JsonNode(level);
		node["secondarySkills"].Vector().push_back(skill);
	}

	node["artifacts"].Vector();
	for(const ArtifactID & artifactID : limiter.artifacts)
		node["artifacts"].Vector().push_back(JsonNode(artifactID.getNum()));

	node["availableSlots"].Vector();
	for(const ArtifactPosition & slot : limiter.availableSlots)
		node["availableSlots"].Vector().push_back(JsonNode(slot.getNum()));

	node["scrolls"].Vector();
	for(const SpellID & spellID : limiter.scrolls)
		node["scrolls"].Vector().push_back(JsonNode(spellID.getNum()));

	node["spells"].Vector();
	for(const SpellID & spellID : limiter.spells)
		node["spells"].Vector().push_back(JsonNode(spellID.getNum()));

	node["canLearnSpells"].Vector();
	for(const SpellID & spellID : limiter.canLearnSpells)
		node["canLearnSpells"].Vector().push_back(JsonNode(spellID.getNum()));

	node["creatures"].Vector();
	for(const CStackBasicDescriptor & creature : limiter.creatures)
		node["creatures"].Vector().push_back(jsonCreatureRequirement(creature));

	node["canReceiveCreatures"].Vector();
	for(const CStackBasicDescriptor & creature : limiter.canReceiveCreatures)
		node["canReceiveCreatures"].Vector().push_back(jsonCreatureRequirement(creature));

	node["heroes"].Vector();
	for(const HeroTypeID & heroID : limiter.heroes)
		node["heroes"].Vector().push_back(JsonNode(heroID.getNum()));

	node["heroClasses"].Vector();
	for(const HeroClassID & heroClassID : limiter.heroClasses)
		node["heroClasses"].Vector().push_back(JsonNode(heroClassID.getNum()));

	node["players"].Vector();
	for(const PlayerColor & player : limiter.players)
		node["players"].Vector().push_back(JsonNode(player.getNum()));

	node["allOfCount"] = JsonNode(static_cast<int32_t>(limiter.allOf.size()));
	node["anyOfCount"] = JsonNode(static_cast<int32_t>(limiter.anyOf.size()));
	node["noneOfCount"] = JsonNode(static_cast<int32_t>(limiter.noneOf.size()));
	if(depth < 2)
	{
		node["allOf"].Vector();
		for(const auto & child : limiter.allOf)
			if(child)
				node["allOf"].Vector().push_back(jsonQuestLimiter(*child, depth + 1));

		node["anyOf"].Vector();
		for(const auto & child : limiter.anyOf)
			if(child)
				node["anyOf"].Vector().push_back(jsonQuestLimiter(*child, depth + 1));

		node["noneOf"].Vector();
		for(const auto & child : limiter.noneOf)
			if(child)
				node["noneOf"].Vector().push_back(jsonQuestLimiter(*child, depth + 1));
	}

	return node;
}

JsonNode jsonQuestObject(const IQuestObject * questObject, PlayerColor player, const CGHeroInstance * contextHero)
{
	JsonNode node;
	const CQuest & quest = questObject->getQuest();
	const bool active = quest.activeForPlayers.count(player) != 0;
	node["activeForPlayer"] = JsonNode(active);
	node["completed"] = JsonNode(quest.isCompleted);
	if(contextHero)
		node["canCompleteWithContextHero"] = JsonNode(active && questObject->checkQuest(contextHero));
	if(!active)
		return node;

	node["quest_id"] = JsonNode(quest.qid.getNum());
	node["missionId"] = JsonNode(questMissionId(quest));
	node["mission"] = JsonNode(quest.questName);
	node["lastDay"] = JsonNode(quest.lastDay);
	node["repeated"] = JsonNode(quest.repeatedQuest);
	if(quest.killTarget != ObjectInstanceID::NONE)
		node["killTargetObjectId"] = JsonNode(quest.killTarget.getNum());
	if(quest.stackToKill != CreatureID::NONE)
		node["stackToKillCreatureId"] = JsonNode(quest.stackToKill.getNum());
	if(quest.heroPortrait != HeroTypeID::NONE)
		node["killHeroTypeId"] = JsonNode(quest.heroPortrait.getNum());
	node["requirements"] = jsonQuestLimiter(quest.mission);
	return node;
}

JsonNode jsonMapObject(const CGObjectInstance * object, PlayerColor player, const CGHeroInstance * contextHero)
{
	const ScriptObjectKind kind = scriptObjectKind(object->ID);
	JsonNode node;
	node["id"] = JsonNode(object->id.getNum());
	node["typeId"] = JsonNode(object->ID.getNum());
	node["subtypeId"] = JsonNode(object->subID.getNum());
	node["kindId"] = JsonNode(static_cast<int32_t>(kind));
	node["kind"] = JsonNode(scriptObjectKindName(kind));
	node["typeIdentifier"] = JsonNode(mapObjectIdentifier(object->ID));
	node["subtypeIdentifier"] = JsonNode(mapObjectSubtypeIdentifier(object->ID, object->subID));
	node["type"] = JsonNode(jsonText(object->getTypeName()));
	node["subtype"] = JsonNode(jsonText(object->getSubtypeName()));
	node["name"] = JsonNode(jsonText(object->getObjectName()));
	node["hoverText"] = JsonNode(jsonText(contextHero ? object->getHoverText(contextHero) : object->getHoverText(player)));
	node["owner"] = JsonNode(jsonPlayerColor(object->tempOwner));
	node["position"] = jsonPosition(object->visitablePos());
	node["passableForPlayer"] = JsonNode(object->passableFor(player));
	if(const auto * questObject = dynamic_cast<const IQuestObject *>(object))
		node["quest"] = jsonQuestObject(questObject, player, contextHero);
	if(const auto * market = dynamic_cast<const IMarket *>(object))
	{
		node["market"]["modes"].Vector();
		for(EMarketMode mode : market->availableModes())
		{
			JsonNode modeNode;
			modeNode["modeId"] = JsonNode(static_cast<int32_t>(mode));
			modeNode["mode"] = JsonNode(marketModeName(mode));
			node["market"]["modes"].Vector().push_back(modeNode);
		}
		if(canExposeMarketDetails(object, player))
		{
			node["market"]["efficiency"] = JsonNode(market->getMarketEfficiency());
			node["market"]["modeDetails"].Vector();
			for(EMarketMode mode : market->availableModes())
				node["market"]["modeDetails"].Vector().push_back(jsonMarketModeDetails(market, mode));
		}
	}
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
	node["formationId"] = JsonNode(static_cast<int32_t>(hero->formation));
	node["formation"] = JsonNode(armyFormationName(hero->formation));
	node["tacticsEnabled"] = JsonNode(hero->tacticFormationEnabled);
	node["primarySkills"]["attack"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::ATTACK));
	node["primarySkills"]["defense"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::DEFENSE));
	node["primarySkills"]["spellPower"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER));
	node["primarySkills"]["knowledge"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::KNOWLEDGE));
	node["armyStrength"] = JsonNode(static_cast<int64_t>(hero->getArmyStrength()));
	node["army"] = jsonArmy(*hero);
	node["artifacts"] = jsonArtifacts(hero);
	node["hasSpellbook"] = JsonNode(hero->hasSpellbook());
	node["spells"].Vector();
	for(const SpellID & spellID : hero->getSpellsInSpellbook())
	{
		if(!spellID.hasValue())
			continue;
		if(const CSpell * spell = spellID.toSpell())
			node["spells"].Vector().push_back(jsonKnownSpell(hero, spell));
	}
	return node;
}

JsonNode jsonNullkillerTaskCandidate(
	int32_t taskID,
	const NK2AI::ScriptTaskCandidate & candidate,
	const std::shared_ptr<CCallback> & cc,
	PlayerColor playerID)
{
	JsonNode node;
	node["task_id"] = JsonNode(taskID);
	node["modeId"] = JsonNode(static_cast<int32_t>(candidate.mode));
	node["mode"] = JsonNode(nullkillerTaskSearchModeName(candidate.mode));
	node["priority"].Float() = candidate.task ? candidate.task->priority : 0.0f;
	node["priorityTier"] = JsonNode(candidate.priorityTier);
	node["heroRoleId"] = JsonNode(static_cast<int32_t>(candidate.heroRole));
	node["heroRole"] = JsonNode(nullkillerHeroRoleName(candidate.heroRole));
	node["affectedObjectIds"].Vector();

	if(candidate.task)
	{
		if(const CGHeroInstance * hero = candidate.task->getHero())
			node["hero_id"] = JsonNode(hero->id.getNum());

		for(const ObjectInstanceID objectID : candidate.task->getAffectedObjects())
			node["affectedObjectIds"].Vector().push_back(JsonNode(objectID.getNum()));

		if(const auto * goal = dynamic_cast<const NK2AI::Goals::AbstractGoal *>(candidate.task.get()))
		{
			node["goal"] = jsonNullkillerGoalSummary(*goal, cc, playerID, 2);
			node["goalTypeId"] = JsonNode(static_cast<int32_t>(goal->goalType));
			node["goalType"] = JsonNode(nullkillerGoalName(goal->goalType));
			if(goal->town)
				node["town_id"] = JsonNode(goal->town->id.getNum());
			if(goal->objid >= 0)
				node["object_id"] = JsonNode(goal->objid);
			if(goal->bid >= 0)
				node["building_id"] = JsonNode(goal->bid);
			if(goal->aid >= 0)
				node["artifact_id"] = JsonNode(goal->aid);
			if(goal->resID >= 0)
				node["resource_id"] = JsonNode(goal->resID);
			if(goal->tile.x >= 0 && goal->tile.y >= 0 && goal->tile.z >= 0)
				node["tile"] = jsonPosition(goal->tile);
			if(goal->goldCost > 0)
				node["goldCost"] = JsonNode(static_cast<int64_t>(goal->goldCost));
			node["buildingCost"] = jsonResources(goal->buildingCost);
		}

		node["debugDescription"] = JsonNode(jsonText(candidate.task->toString()));
	}

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

std::optional<SlotID> weakestTransferReserveSlot(const CArmedInstance * source)
{
	if(!source || source->Slots().empty())
		return std::nullopt;

	std::optional<SlotID> result;
	uint64_t bestPower = std::numeric_limits<uint64_t>::max();
	for(const auto & slot : source->Slots())
	{
		if(!slot.second || slot.second->getCount() <= 0)
			continue;

		const uint64_t power = slot.second->getPower();
		if(!result || power < bestPower)
		{
			result = slot.first;
			bestPower = power;
		}
	}
	return result;
}

bool bulkTransferWouldMoveAnything(const CArmedInstance * source, const CArmedInstance * destination, SlotID reserveSlot)
{
	if(!source || !destination || source == destination || !reserveSlot.validSlot())
		return false;
	if(!source->hasStackAtSlot(reserveSlot))
		return false;
	if(source->stacksCount() == 1 && source->getStack(reserveSlot).getCount() <= 1)
		return false;

	auto freeSlots = destination->getFreeSlots();
	for(const auto & slot : source->Slots())
	{
		if(!slot.second || slot.second->getCount() <= 0)
			continue;

		SlotID targetSlot = destination->getSlotFor(slot.second->getCreature());
		if(destination->slotEmpty(targetSlot))
		{
			if(freeSlots.empty())
				continue;
			freeSlots.erase(freeSlots.begin());
		}

		if(slot.first != reserveSlot || slot.second->getCount() > 1 || source->stacksCount() > 1)
			return true;
	}
	return false;
}

JsonNode jsonAvailableHeroOption(const CGTownInstance * town, const CGHeroInstance * hero)
{
	JsonNode node;
	node["town_id"] = JsonNode(town->id.getNum());
	node["town"] = JsonNode(jsonText(town->getNameTranslated()));
	node["hero_type_id"] = JsonNode(hero->getHeroTypeID().getNum());
	node["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
	node["heroStrength"] = JsonNode(static_cast<int64_t>(hero->getHeroStrength()));
	node["armyStrength"] = JsonNode(static_cast<int64_t>(hero->getArmyStrength()));
	node["totalStrength"] = JsonNode(static_cast<int64_t>(hero->getTotalStrength()));
	node["cost"]["gold"] = JsonNode(GameConstants::HERO_GOLD_COST);
	node["army"] = jsonArmy(*hero);
	node["planAction"]["type"] = JsonNode("hire_hero");
	node["planAction"]["town_id"] = node["town_id"];
	node["planAction"]["hero_type_id"] = node["hero_type_id"];
	return node;
}

JsonNode jsonArmyTransferOption(
	const CArmedInstance * source,
	const CArmedInstance * destination,
	SlotID sourceSlot,
	ScriptArmyTransferKind kind,
	int32_t townID)
{
	JsonNode node;
	node["source_id"] = JsonNode(source->id.getNum());
	node["destination_id"] = JsonNode(destination->id.getNum());
	node["source_slot"] = JsonNode(sourceSlot.getNum());
	node["town_id"] = JsonNode(townID);
	node["transferKindId"] = JsonNode(static_cast<int32_t>(kind));
	node["transferKind"] = JsonNode(scriptArmyTransferKindName(kind));
	node["sourceArmyStrength"] = JsonNode(static_cast<int64_t>(source->getArmyStrength()));
	node["destinationArmyStrength"] = JsonNode(static_cast<int64_t>(destination->getArmyStrength()));
	node["value"] = JsonNode(static_cast<int64_t>(source->getArmyStrength()));
	node["planAction"]["type"] = JsonNode("transfer_army");
	node["planAction"]["source_id"] = node["source_id"];
	node["planAction"]["destination_id"] = node["destination_id"];
	node["planAction"]["source_slot"] = node["source_slot"];
	return node;
}

JsonNode jsonUpgradeCreatureOption(
	const CArmedInstance * army,
	SlotID slot,
	const CStackInstance * stack,
	CreatureID upgradeID,
	const ResourceSet & unitCost,
	const ResourceSet & totalCost,
	const ResourceSet & resources)
{
	JsonNode node;
	const CCreature * current = stack ? stack->getCreature() : nullptr;
	const CCreature * upgraded = upgradeID.toCreature();
	const int64_t count = stack ? static_cast<int64_t>(stack->getCount()) : 0;
	const int64_t currentValue = current ? static_cast<int64_t>(current->getAIValue()) * count : 0;
	const int64_t upgradedValue = upgraded ? static_cast<int64_t>(upgraded->getAIValue()) * count : 0;

	node["army_id"] = JsonNode(army->id.getNum());
	node["slot"] = JsonNode(slot.getNum());
	node["creature_id"] = JsonNode(current ? current->getId().getNum() : CreatureID(CreatureID::NONE).getNum());
	node["upgrade_creature_id"] = JsonNode(upgradeID.getNum());
	node["count"] = JsonNode(count);
	node["creature"] = JsonNode(current ? jsonText(current->getNamePluralTranslated()) : "");
	node["upgradeCreature"] = JsonNode(upgraded ? jsonText(upgraded->getNamePluralTranslated()) : "");
	node["unitCost"] = jsonResources(unitCost);
	node["totalCost"] = jsonResources(totalCost);
	node["affordable"] = JsonNode(resources.canAfford(totalCost));
	node["currentValue"] = JsonNode(currentValue);
	node["upgradedValue"] = JsonNode(upgradedValue);
	node["value"] = JsonNode(upgradedValue - currentValue);
	node["planAction"]["type"] = JsonNode("upgrade_creature");
	node["planAction"]["army_id"] = node["army_id"];
	node["planAction"]["slot"] = node["slot"];
	node["planAction"]["creature_id"] = node["upgrade_creature_id"];
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
	const ScriptBuildingKind kind = scriptBuildingKind(building->bid);
	JsonNode node;
	node["town_id"] = JsonNode(town->id.getNum());
	node["town"] = JsonNode(jsonText(town->getNameTranslated()));
	node["building_id"] = JsonNode(building->bid.getNum());
	node["buildingKindId"] = JsonNode(static_cast<int32_t>(kind));
	node["buildingKind"] = JsonNode(scriptBuildingKindName(kind));
	node["buildingLevel"] = JsonNode(scriptBuildingLevel(building->bid));
	node["buildingUpgrade"] = JsonNode(scriptBuildingUpgrade(building->bid));
	node["building"] = JsonNode(jsonText(building->getNameTranslated()));
	node["cost"] = jsonResources(building->resources);
	node["income"] = jsonResources(building->produce);
	node["planAction"]["type"] = JsonNode("build");
	node["planAction"]["town_id"] = node["town_id"];
	node["planAction"]["building_id"] = node["building_id"];
	return node;
}

JsonNode jsonDigOption(const CGHeroInstance * hero)
{
	const EDiggingStatus status = hero->diggingStatus();
	JsonNode node;
	node["hero_id"] = JsonNode(hero->id.getNum());
	node["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
	node["position"] = jsonPosition(hero->visitablePos());
	node["statusId"] = JsonNode(static_cast<int32_t>(status));
	node["status"] = JsonNode(diggingStatusName(status));
	node["canDig"] = JsonNode(status == EDiggingStatus::CAN_DIG);
	node["planAction"]["type"] = JsonNode("dig");
	node["planAction"]["hero_id"] = node["hero_id"];
	return node;
}

JsonNode jsonShipyardOption(const CGObjectInstance * object, const IShipyard * shipyard, const ResourceSet & resources, bool enemy)
{
	ResourceSet cost;
	shipyard->getBoatCost(cost);
	const auto status = shipyard->shipyardStatus();
	const bool affordable = resources.canAfford(cost);
	const bool buildable = status == IBoatGenerator::GOOD && affordable && !enemy;

	JsonNode node;
	node["shipyard_id"] = JsonNode(object->id.getNum());
	node["position"] = jsonPosition(object->visitablePos());
	node["owner"] = JsonNode(jsonPlayerColor(object->tempOwner));
	node["statusId"] = JsonNode(static_cast<int32_t>(status));
	node["status"] = JsonNode(shipyardStatusName(status));
	node["bestLocation"] = jsonPosition(shipyard->bestLocation());
	node["boatTypeId"] = JsonNode(shipyard->getBoatType().getNum());
	node["boatLayerId"] = JsonNode(static_cast<int32_t>(shipyard->getBoatLayer()));
	node["cost"] = jsonResources(cost);
	node["affordable"] = JsonNode(affordable);
	node["enemy"] = JsonNode(enemy);
	node["buildable"] = JsonNode(buildable);
	node["planAction"]["type"] = JsonNode("build_boat");
	node["planAction"]["shipyard_id"] = node["shipyard_id"];
	return node;
}

JsonNode jsonAdventureSpellOption(const CGHeroInstance * hero, const CSpell * spell, const std::optional<int3> & target, int32_t targetKindID, const std::string & targetKind)
{
	JsonNode node;
	node["hero_id"] = JsonNode(hero->id.getNum());
	node["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
	node["spell_id"] = JsonNode(spell->getId().getNum());
	node["spellIdentifier"] = JsonNode(spell->getJsonKey());
	node["spellName"] = JsonNode(jsonText(spell->getNameTranslated()));
	node["level"] = JsonNode(spell->getLevel());
	node["schoolLevel"] = JsonNode(hero->getSpellSchoolLevel(spell));
	node["cost"] = JsonNode(hero->getSpellCost(spell));
	node["mana"] = JsonNode(hero->mana);
	node["targetKindId"] = JsonNode(targetKindID);
	node["targetKind"] = JsonNode(targetKind);
	node["planAction"]["type"] = JsonNode("cast_spell");
	node["planAction"]["hero_id"] = node["hero_id"];
	node["planAction"]["spell_id"] = node["spell_id"];
	if(target)
	{
		node["target"] = jsonPosition(*target);
		node["planAction"]["x"] = JsonNode(target->x);
		node["planAction"]["y"] = JsonNode(target->y);
		node["planAction"]["z"] = JsonNode(target->z);
	}
	return node;
}

JsonNode jsonBuyArtifactOption(const CGHeroInstance * hero, ArtifactID artifactID, const ResourceSet & resources)
{
	JsonNode node;
	const CArtifact * artifact = artifactID.toArtifact();
	const bool spellbook = artifactID == ArtifactID::SPELLBOOK;
	const int32_t price = spellbook ? GameConstants::SPELLBOOK_GOLD_COST : (artifact ? static_cast<int32_t>(artifact->getPrice()) : 0);

	node["hero_id"] = JsonNode(hero->id.getNum());
	node["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
	if(const CGTownInstance * town = hero->getVisitedTown())
	{
		node["town_id"] = JsonNode(town->id.getNum());
		node["town"] = JsonNode(jsonText(town->getNameTranslated()));
	}
	node["artifact_id"] = JsonNode(artifactID.getNum());
	node["kindId"] = JsonNode(spellbook ? 1 : 2);
	node["kind"] = JsonNode(spellbook ? "spellbook" : "war_machine");
	node["price"] = JsonNode(price);
	node["affordable"] = JsonNode(resources[EGameResID::GOLD] >= price);
	if(artifact)
	{
		node["artifactIdentifier"] = JsonNode(artifact->getJsonKey());
		node["artifactName"] = JsonNode(jsonText(artifact->getNameTranslated()));
		if(artifact->getWarMachine() != CreatureID::NONE)
			node["warMachineCreatureId"] = JsonNode(artifact->getWarMachine().getNum());
	}
	node["planAction"]["type"] = JsonNode("buy_artifact");
	node["planAction"]["hero_id"] = node["hero_id"];
	node["planAction"]["artifact_id"] = node["artifact_id"];
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
	{
		std::lock_guard guard(autoAnswerMutex);
		pendingAutoAnswers[queryID] = selection;
	}

	executeActionAsync(description, [this, queryID]()
	{
		std::optional<int> selection;
		{
			std::lock_guard guard(autoAnswerMutex);
			auto iter = pendingAutoAnswers.find(queryID);
			if(iter == pendingAutoAnswers.end())
				return;

			selection = iter->second;
			pendingAutoAnswers.erase(iter);
		}

		answerQuery(queryID, *selection);
	});
}

void CScriptedAdventureAI::pauseForScriptActionQuery(const std::string & queryDescription, QueryID queryID)
{
	status.addQuery(queryID, queryDescription);
}

size_t CScriptedAdventureAI::answerPendingAutoQueries()
{
	std::vector<std::pair<QueryID, int>> answers;
	{
		std::lock_guard guard(autoAnswerMutex);
		answers.assign(pendingAutoAnswers.begin(), pendingAutoAnswers.end());
		pendingAutoAnswers.clear();
	}

	for(const auto & answer : answers)
	{
		removeScriptQuery(answer.first);
		answerQuery(answer.first, answer.second);
	}

	return answers.size();
}

void CScriptedAdventureAI::setScriptActionAutoAnswerMode(bool active)
{
	std::lock_guard guard(autoAnswerMutex);
	scriptActionAutoAnswerMode = active;
}

bool CScriptedAdventureAI::isScriptActionAutoAnswerMode()
{
	std::lock_guard guard(autoAnswerMutex);
	return scriptActionAutoAnswerMode;
}

void CScriptedAdventureAI::recordScriptQuery(QueryID queryID, const std::string & type, JsonNode data)
{
	if(queryID == QueryID(-1))
		return;

	data["query_id"] = JsonNode(queryID.getNum());
	data["type"] = JsonNode(type);

	std::lock_guard guard(scriptQueryMutex);
	scriptQueries[queryID] = data;
}

void CScriptedAdventureAI::removeArtifactAssemblyPrompts(ObjectInstanceID heroID, ArtifactPosition slot)
{
	std::lock_guard guard(scriptQueryMutex);
	for(auto iter = scriptQueries.begin(); iter != scriptQueries.end();)
	{
		const JsonNode & query = iter->second;
		if(hasField(query, "type")
			&& query["type"].String() == "artifact_assembly_prompt"
			&& hasField(query, "hero_id")
			&& hasField(query, "slot")
			&& query["hero_id"].Integer() == heroID.getNum()
			&& query["slot"].Integer() == slot.getNum())
		{
			iter = scriptQueries.erase(iter);
		}
		else
		{
			++iter;
		}
	}
}

void CScriptedAdventureAI::removeScriptQuery(QueryID queryID)
{
	std::lock_guard guard(scriptQueryMutex);
	scriptQueries.erase(queryID);
}

JsonNode CScriptedAdventureAI::makeScriptQueries() const
{
	JsonNode node;
	node.Vector();

	std::lock_guard guard(scriptQueryMutex);
	for(const auto & entry : scriptQueries)
		node.Vector().push_back(entry.second);

	return node;
}

void CScriptedAdventureAI::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID)
{
	JsonNode data;
	if(hero)
		data["hero_id"] = JsonNode(hero->id.getNum());
	data["primary_skill_id"] = JsonNode(pskill.getNum());
	data["skill_options"].Vector();
	for(size_t index = 0; index < skills.size(); ++index)
	{
		JsonNode option;
		option["answer"] = JsonNode(static_cast<int32_t>(index));
		option["skill_id"] = JsonNode(skills[index].getNum());
		data["skill_options"].Vector().push_back(option);
	}
	recordScriptQuery(queryID, "hero_level_up", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI hero level-up dialog", queryID);
		return;
	}
	AIGateway::heroGotLevel(hero, pskill, skills, queryID);
}

void CScriptedAdventureAI::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID)
{
	JsonNode data;
	data["skill_options"].Vector();
	for(size_t index = 0; index < skills.size(); ++index)
	{
		JsonNode option;
		option["answer"] = JsonNode(static_cast<int32_t>(index));
		option["skill_id"] = JsonNode(static_cast<int32_t>(skills[index]));
		data["skill_options"].Vector().push_back(option);
	}
	recordScriptQuery(queryID, "commander_level_up", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI commander level-up dialog", queryID);
		return;
	}
	AIGateway::commanderGotLevel(commander, skills, queryID);
}

void CScriptedAdventureAI::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	JsonNode data;
	data["text"] = JsonNode(text);
	data["sound_id"] = JsonNode(soundID);
	data["selection"] = JsonNode(selection);
	data["cancel"] = JsonNode(cancel);
	data["safe_to_autoaccept"] = JsonNode(safeToAutoaccept);
	data["components"] = jsonComponents(components);
	for(size_t index = 0; index < data["components"].Vector().size(); ++index)
		data["components"].Vector()[index]["answer"] = JsonNode(static_cast<int32_t>(index + 1));
	recordScriptQuery(askID, "blocking_dialog", data);

	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI blocking dialog", askID);
		return;
	}

	AIGateway::showBlockingDialog(text, components, askID, soundID, selection, cancel, safeToAutoaccept);
}

void CScriptedAdventureAI::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	JsonNode data;
	if(hero)
		data["hero_id"] = JsonNode(hero->id.getNum());
	data["channel_id"] = JsonNode(channel.getNum());
	data["impassable"] = JsonNode(impassable);
	data["exits"].Vector();
	for(size_t index = 0; index < exits.size(); ++index)
	{
		JsonNode exit;
		exit["answer"] = JsonNode(static_cast<int32_t>(index));
		exit["object_id"] = JsonNode(exits[index].first.getNum());
		exit["position"] = jsonPosition(exits[index].second);
		data["exits"].Vector().push_back(exit);
	}
	recordScriptQuery(askID, "teleport_dialog", data);

	if(!isScriptActionAutoAnswerMode())
	{
		AIGateway::showTeleportDialog(hero, channel, exits, impassable, askID);
		return;
	}

	pauseForScriptActionQuery("ScriptedAdventureAI teleport dialog", askID);
}

void CScriptedAdventureAI::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	JsonNode data;
	data["icon"] = jsonComponent(icon);
	data["title"] = JsonNode(title.toString());
	data["description"] = JsonNode(description.toString());
	data["objects"].Vector();
	for(const ObjectInstanceID & objectID : objects)
	{
		JsonNode option;
		option["answer"] = JsonNode(objectID.getNum());
		option["object_id"] = JsonNode(objectID.getNum());
		data["objects"].Vector().push_back(option);
	}
	recordScriptQuery(askID, "map_object_select", data);

	if(!isScriptActionAutoAnswerMode())
	{
		AIGateway::showMapObjectSelectDialog(askID, icon, title, description, objects);
		return;
	}

	pauseForScriptActionQuery("ScriptedAdventureAI map object select dialog", askID);
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
	JsonNode data;
	if(object)
		data["object_id"] = JsonNode(object->id.getNum());
	if(visitor)
		data["visitor_hero_id"] = JsonNode(visitor->id.getNum());
	recordScriptQuery(queryID, "tavern_window", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI tavern window", queryID);
		return;
	}
	AIGateway::showTavernWindow(object, visitor, queryID);
}

void CScriptedAdventureAI::heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query)
{
	JsonNode data;
	data["hero1_id"] = JsonNode(hero1.getNum());
	data["hero2_id"] = JsonNode(hero2.getNum());
	recordScriptQuery(query, "hero_exchange", data);

	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI hero exchange dialog", query);
		return;
	}

	AIGateway::heroExchangeStarted(hero1, hero2, query);
}

void CScriptedAdventureAI::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	JsonNode data;
	if(up)
		data["upper_army_id"] = JsonNode(up->id.getNum());
	if(down)
		data["lower_hero_id"] = JsonNode(down->id.getNum());
	data["removable_units"] = JsonNode(removableUnits);
	data["custom_title"] = JsonNode(customTitle.toString());
	recordScriptQuery(queryID, "garrison_dialog", data);

	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI garrison dialog", queryID);
		return;
	}

	AIGateway::showGarrisonDialog(up, down, removableUnits, queryID, customTitle);
}

void CScriptedAdventureAI::showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QueryID queryID)
{
	JsonNode data;
	if(dwelling)
		data["dwelling_id"] = JsonNode(dwelling->id.getNum());
	if(dst)
		data["destination_id"] = JsonNode(dst->id.getNum());
	data["level"] = JsonNode(level);
	recordScriptQuery(queryID, "recruitment_dialog", data);

	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI recruitment dialog", queryID);
		return;
	}

	AIGateway::showRecruitmentDialog(dwelling, dst, level, queryID);
}

void CScriptedAdventureAI::showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	JsonNode data;
	if(market)
		data["market_id"] = JsonNode(market->getObjInstanceID().getNum());
	if(visitor)
		data["visitor_hero_id"] = JsonNode(visitor->id.getNum());
	recordScriptQuery(queryID, "university_window", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI university window", queryID);
		return;
	}
	AIGateway::showUniversityWindow(market, visitor, queryID);
}

void CScriptedAdventureAI::showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	JsonNode data;
	if(market)
		data["market_id"] = JsonNode(market->getObjInstanceID().getNum());
	if(visitor)
		data["visitor_hero_id"] = JsonNode(visitor->id.getNum());
	recordScriptQuery(queryID, "market_window", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI market window", queryID);
		return;
	}
	AIGateway::showMarketWindow(market, visitor, queryID);
}

void CScriptedAdventureAI::askToAssembleArtifact(const ArtifactLocation & destination)
{
	const CGHeroInstance * hero = cc ? cc->getHero(destination.artHolder) : nullptr;
	if(hero && hero->tempOwner == playerID)
	{
		const QueryID decisionID(nextScriptDecisionID--);
		recordScriptQuery(decisionID, "artifact_assembly_prompt", jsonArtifactAssemblyPrompt(decisionID, hero, destination));
	}

	AIGateway::askToAssembleArtifact(destination);
}

void CScriptedAdventureAI::artifactAssembled(const ArtifactLocation & location)
{
	removeArtifactAssemblyPrompts(location.artHolder, location.slot);

	JsonNode data;
	data["location"] = jsonArtifactLocation(location);
	data["holder_id"] = JsonNode(location.artHolder.getNum());
	data["slot"] = JsonNode(location.slot.getNum());
	appendScriptUpdate("artifact_assembled", data, false);

	AIGateway::artifactAssembled(location);
}

void CScriptedAdventureAI::artifactDisassembled(const ArtifactLocation & location)
{
	removeArtifactAssemblyPrompts(location.artHolder, location.slot);

	JsonNode data;
	data["location"] = jsonArtifactLocation(location);
	data["holder_id"] = JsonNode(location.artHolder.getNum());
	data["slot"] = JsonNode(location.slot.getNum());
	appendScriptUpdate("artifact_disassembled", data, false);

	AIGateway::artifactDisassembled(location);
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
			if(*earlyResult)
				removeScriptQuery(reply->qid);
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
		{
			status.receivedAnswerConfirmation(static_cast<int>(pa->requestID), pa->result);
			if(pa->result)
				removeScriptQuery(*queryID);
		}
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

bool CScriptedAdventureAI::waitTillFreeForScriptAction(JsonNode & actionResult, const std::string & actionType)
{
	static constexpr auto SCRIPT_ACTION_STATUS_TIMEOUT = std::chrono::seconds(30);
	static constexpr auto SCRIPT_ACTION_BATTLE_STATUS_TIMEOUT = std::chrono::minutes(30);
	static constexpr auto SCRIPT_ACTION_STATUS_POLL = std::chrono::milliseconds(100);
	const auto started = std::chrono::steady_clock::now();

	while(true)
	{
		const auto timeout = status.getBattle() == NK2AI::NO_BATTLE ? SCRIPT_ACTION_STATUS_TIMEOUT : SCRIPT_ACTION_BATTLE_STATUS_TIMEOUT;
		if(std::chrono::steady_clock::now() - started >= timeout)
			break;

		if(status.waitTillFreeFor(SCRIPT_ACTION_STATUS_POLL))
			return true;
		if(status.getQueriesCount() > 0)
		{
			if(answerPendingAutoQueries() > 0)
				continue;

			JsonNode queries = makeScriptQueries();
			if(!queries.Vector().empty())
			{
				actionResult["ok"] = JsonNode(true);
				actionResult["pending_query"] = JsonNode(true);
				actionResult["queries"] = queries;
				actionResult["message"] = JsonNode("Action paused for script query");
				return false;
			}
		}
	}

	const std::string blockers = status.describeBlockers();
	logAi->warn("ScriptedAdventureAI timed out waiting after %s action. Blockers: %s", actionType.c_str(), blockers.c_str());
	actionResult["ok"] = JsonNode(false);
	actionResult["error"] = JsonNode("Timed out waiting for action side effects to finish: " + blockers);
	return false;
}

JsonNode CScriptedAdventureAI::makeNullkillerTaskCandidates(const JsonNode & action)
{
	const NK2AI::ScriptTaskSearchMode mode = readNullkillerTaskSearchMode(action);
	const int32_t requestedMax = readInteger(action, "max_candidates", 16);
	const size_t maxCandidates = static_cast<size_t>(std::clamp<int32_t>(requestedMax, 1, 64));

	JsonNode result;
	result["modeId"] = JsonNode(static_cast<int32_t>(mode));
	result["mode"] = JsonNode(nullkillerTaskSearchModeName(mode));
	result["tasks"].Vector();

	{
		std::shared_lock gameStateLock(CGameState::mutex);
		AIGateway::cheatMapReveal(nullkiller);
		AIGateway::memorizeVisitableObjs(nullkiller->memory, nullkiller->dangerHitMap, playerID, cc);
		AIGateway::memorizeRevisitableObjs(nullkiller->memory, playerID, cc);

		const auto candidates = nullkiller->getScriptTaskCandidates(mode, maxCandidates);

		nullkillerTaskHandles.clear();
		for(const NK2AI::ScriptTaskCandidate & candidate : candidates)
		{
			const int32_t taskID = nextNullkillerTaskHandle++;
			nullkillerTaskHandles.emplace_back(taskID, candidate.task);
			result["tasks"].Vector().push_back(jsonNullkillerTaskCandidate(taskID, candidate, cc, playerID));
		}
	}

	result["count"] = JsonNode(static_cast<int32_t>(result["tasks"].Vector().size()));
	return result;
}

bool CScriptedAdventureAI::executeNullkillerTaskAction(const JsonNode & action, JsonNode & actionResult)
{
	const int32_t taskID = readInteger(action, "task_id");
	const auto taskIter = std::ranges::find_if(nullkillerTaskHandles, [taskID](const auto & entry)
	{
		return entry.first == taskID;
	});
	if(taskIter == nullkillerTaskHandles.end())
		throw std::invalid_argument("Unknown or expired Nullkiller task handle");

	const NK2AI::Goals::TTask task = taskIter->second;
	actionResult["task_id"] = JsonNode(taskID);
	if(task)
		actionResult["debugDescription"] = JsonNode(jsonText(task->toString()));

	bool executed = false;
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		executed = nullkiller->executeScriptTask(task);
	}

	for(const auto * heroInfo : cc->getHeroesInfo())
		AIGateway::pickBestArtifacts(cc, heroInfo);

	if(!waitTillFreeForScriptAction(actionResult, "nullkiller_task"))
		return false;

	actionResult["ok"] = JsonNode(executed);
	if(!executed)
		actionResult["error"] = JsonNode("Nullkiller task failed to execute");
	return executed;
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
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		nullkiller->resetScriptTaskState();
	}

	if(runner->hasRunDay())
		return tryMakeImperativeScriptedTurn(*runner);

	JsonNode progress;

	for(size_t callIndex = 0; callIndex < maxScriptCallsPerTurn && status.haveTurn(); ++callIndex)
	{
		AI::AdventureScriptInput input = makeAdventureScriptInput(progress);

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
			fallbackToNullkiller("script requested fallback", false);
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

bool CScriptedAdventureAI::tryMakeImperativeScriptedTurn(scripting::LuaAdventureScriptRunner & runner)
{
	JsonNode executed;
	JsonNode failed;
	JsonNode remaining;
	executed.Vector();
	failed.Vector();
	remaining.Vector();
	nullkillerTaskHandles.clear();

	JsonNode progress = makeProgressJson(executed, failed, remaining);
	AI::AdventureScriptInput input = makeAdventureScriptInput(progress);

	if(scriptConfig.trace)
	{
		JsonNode trace;
		trace["input"] = input.toJson();
		writeTraceEvent("imperative-input", trace);
	}

	size_t commandIndex = 0;
	const auto commandHandler = [&](const JsonNode & command) -> JsonNode
	{
		JsonNode response;
		response["ok"] = JsonNode(true);

		try
		{
			const std::string kind = readString(command, "kind");
			if(kind == "refresh")
			{
				nullkillerTaskHandles.clear();
				response["input"] = makeAdventureScriptInput(progress).toJson();
				return response;
			}

			if(kind != "execute")
				throw std::invalid_argument("Unsupported imperative command kind: " + kind);

			if(commandIndex >= limits.maxActions)
				throw std::invalid_argument("Script exceeded configured command limit");

			const JsonNode & action = command["payload"];
			JsonNode actionResult;
			actionResult["index"] = JsonNode(static_cast<int32_t>(commandIndex));
			if(hasField(action, "id"))
				actionResult["id"] = action["id"];

			bool continueAfterAction = false;
			try
			{
				continueAfterAction = executeScriptAction(action, actionResult);
			}
			catch(const std::exception & e)
			{
				actionResult["ok"] = JsonNode(false);
				actionResult["error"] = JsonNode(e.what());
			}

			++commandIndex;
			actionResult["stop"] = JsonNode(!continueAfterAction || !status.haveTurn());

			if(actionResult["ok"].isBool() && !actionResult["ok"].Bool())
			{
				failed.Vector().push_back(actionResult);
				progress = makeProgressJson(executed, failed, remaining);
				response["ok"] = JsonNode(false);
				response["error"] = actionResult["error"].isString()
					? actionResult["error"]
					: JsonNode("Script action failed");
				response["result"] = actionResult;
			}
			else
			{
				executed.Vector().push_back(actionResult);
				progress = makeProgressJson(executed, failed, remaining);
				response["result"] = actionResult;
			}

			if(scriptConfig.trace)
			{
				JsonNode trace;
				trace["commandIndex"] = JsonNode(static_cast<int32_t>(commandIndex - 1));
				trace["command"] = command;
				trace["response"] = response;
				trace["progress"] = progress;
				writeTraceEvent("imperative-command", trace);
			}

			return response;
		}
		catch(const std::exception & e)
		{
			response["ok"] = JsonNode(false);
			response["error"] = JsonNode(e.what());
			return response;
		}
	};

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, commandHandler);
	scriptMemory = output.memory;
	saveScriptMemoryToLocalState();

	if(scriptConfig.trace)
	{
		JsonNode trace;
		trace["output"] = AI::makeAdventureScriptOutputJson(output);
		trace["progress"] = progress;
		writeTraceEvent("imperative-output", trace);
	}

	if(output.status == AI::AdventureScriptStatus::FALLBACK)
	{
		fallbackToNullkiller("script requested fallback", false);
		return false;
	}

	if(!status.haveTurn())
		return true;

	if(output.status == AI::AdventureScriptStatus::END_TURN)
	{
		endTurn();
		return true;
	}

	fallbackToNullkiller("imperative script returned without ending turn");
	return false;
}

bool CScriptedAdventureAI::executeScriptAction(const JsonNode & action, JsonNode & actionResult)
{
	ScopedCallbackWaitMode nonBlockingCallback(cc, false);
	const std::string type = readString(action, "type");
	actionResult["type"] = JsonNode(type);
	struct AutoAnswerModeGuard
	{
		CScriptedAdventureAI & owner;
		explicit AutoAnswerModeGuard(CScriptedAdventureAI & owner_)
			: owner(owner_)
		{
			owner.setScriptActionAutoAnswerMode(true);
		}
		~AutoAnswerModeGuard()
		{
			owner.setScriptActionAutoAnswerMode(false);
		}
	};

	std::optional<AutoAnswerModeGuard> autoAnswerModeGuard;
	if(type != "nullkiller_trade" && type != "nullkiller_tasks" && type != "nullkiller_task" && type != "nullkiller_step")
		autoAnswerModeGuard.emplace(*this);

	auto readOwnedArmy = [&](const std::string & field, const std::string & label) -> const CArmedInstance *
	{
		const auto * army = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(readInteger(action, field)), false));
		if(!army || army->tempOwner != playerID)
			throw std::invalid_argument("Unknown " + label + " army holder or holder is not owned by scripted AI");
		return army;
	};
	auto readValidSlot = [&](const std::string & field) -> SlotID
	{
		const SlotID slot(readInteger(action, field));
		if(!slot.validSlot())
			throw std::invalid_argument("Invalid army slot id in " + field);
		return slot;
	};
	auto requireStack = [](const CArmedInstance * army, SlotID slot, const std::string & label)
	{
		if(!army->hasStackAtSlot(slot))
			throw std::invalid_argument("No creature stack at " + label + " slot");
	};

	if(type == "nullkiller_trade")
	{
		bool traded = false;
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			traded = nullkiller->executeScriptResourceTrade();
		}
		actionResult["ok"] = JsonNode(true);
		actionResult["didTrade"] = JsonNode(traded);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		return true;
	}

	if(type == "trade_resources")
	{
		const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(action, "market_id")), false);
		const IMarket * market = dynamic_cast<const IMarket *>(object);
		if(!object || !market)
			throw std::invalid_argument("Unknown visible market object");
		if(!market->allowsTrade(EMarketMode::RESOURCE_RESOURCE))
			throw std::invalid_argument("Market does not allow resource-resource trading");

		const int32_t sellResource = readInteger(action, "sell_resource_id");
		const int32_t buyResource = readInteger(action, "buy_resource_id");
		if(sellResource < 0 || sellResource >= static_cast<int32_t>(GameConstants::RESOURCE_QUANTITY)
			|| buyResource < 0 || buyResource >= static_cast<int32_t>(GameConstants::RESOURCE_QUANTITY))
			throw std::invalid_argument("Resource trade ids must be valid resource ids");
		if(sellResource == buyResource)
			throw std::invalid_argument("Resource trade must buy a different resource than it sells");

		const int32_t amount = readInteger(action, "amount");
		if(amount <= 0)
			throw std::invalid_argument("Resource trade amount must be positive");

		const CGHeroInstance * hero = nullptr;
		if(hasField(action, "hero_id"))
		{
			hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
			if(!hero || hero->tempOwner != playerID)
				throw std::invalid_argument("Unknown trade hero or hero is not owned by scripted AI");
		}

		const RequestWaitResult request = submitAndWaitForRequest(typeid(TradeOnMarketplace), CTypeList::getInstance().getTypeID<TradeOnMarketplace>(nullptr), [&]
		{
			cc->trade(object->id, EMarketMode::RESOURCE_RESOURCE, GameResID(sellResource), GameResID(buyResource), static_cast<ui32>(amount), hero);
		});
		actionResult["market_id"] = JsonNode(object->id.getNum());
		if(hero)
			actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["sell_resource_id"] = JsonNode(sellResource);
		actionResult["buy_resource_id"] = JsonNode(buyResource);
		actionResult["amount"] = JsonNode(amount);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Resource trade request was rejected by server" : "Resource trade request was not realized by server");
		return true;
	}

	if(type == "market_trade")
	{
		const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(action, "market_id")), false);
		const IMarket * market = dynamic_cast<const IMarket *>(object);
		if(!object || !market)
			throw std::invalid_argument("Unknown visible market object");

		const int32_t modeID = readInteger(action, "mode_id");
		if(modeID < 0 || modeID >= static_cast<int32_t>(EMarketMode::MARKET_AFTER_LAST_PLACEHOLDER))
			throw std::invalid_argument("Invalid market mode id");
		const EMarketMode mode = static_cast<EMarketMode>(modeID);
		if(!market->allowsTrade(mode))
			throw std::invalid_argument("Market does not allow requested trade mode");

		const CGHeroInstance * hero = nullptr;
		if(hasField(action, "hero_id"))
		{
			hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
			if(!hero || hero->tempOwner != playerID)
				throw std::invalid_argument("Unknown market trade hero or hero is not owned by scripted AI");
		}

		auto readResourceID = [&](const std::string & field) -> GameResID
		{
			const int32_t resourceID = readInteger(action, field);
			if(resourceID < 0 || resourceID >= static_cast<int32_t>(GameConstants::RESOURCE_QUANTITY))
				throw std::invalid_argument("Invalid resource id in " + field);
			return GameResID(resourceID);
		};
		auto requireHero = [&]()
		{
			if(!hero)
				throw std::invalid_argument("Requested market trade mode requires an owned hero_id");
		};
		auto readPositiveAmount = [&](const std::string & field) -> ui32
		{
			const int32_t amount = readInteger(action, field);
			if(amount <= 0)
				throw std::invalid_argument("Market trade amount must be positive");
			return static_cast<ui32>(amount);
		};

		TradeItemSell sell;
		TradeItemBuy buy;
		ui32 amount = 1;
		switch(mode)
		{
		case EMarketMode::RESOURCE_RESOURCE:
		{
			const GameResID sellResource = readResourceID("sell_resource_id");
			const GameResID buyResource = readResourceID("buy_resource_id");
			if(sellResource == buyResource)
				throw std::invalid_argument("Resource trade must buy a different resource than it sells");
			sell = sellResource;
			buy = buyResource;
			amount = readPositiveAmount("amount");
			break;
		}
		case EMarketMode::RESOURCE_PLAYER:
		{
			const PlayerColor targetPlayer(readInteger(action, "target_player_id"));
			if(!targetPlayer.isValidPlayer())
				throw std::invalid_argument("Invalid target player id for resource transfer");
			sell = readResourceID("sell_resource_id");
			buy = targetPlayer;
			amount = readPositiveAmount("amount");
			break;
		}
		case EMarketMode::CREATURE_RESOURCE:
			requireHero();
			sell = readValidSlot("slot");
			buy = readResourceID("buy_resource_id");
			amount = readPositiveAmount("amount");
			break;
		case EMarketMode::RESOURCE_ARTIFACT:
			requireHero();
			sell = readResourceID("sell_resource_id");
			buy = ArtifactID(readInteger(action, "artifact_id"));
			break;
		case EMarketMode::ARTIFACT_RESOURCE:
			requireHero();
			sell = ArtifactInstanceID(readInteger(action, "artifact_instance_id"));
			buy = readResourceID("buy_resource_id");
			break;
		case EMarketMode::ARTIFACT_EXP:
			requireHero();
			sell = ArtifactInstanceID(readInteger(action, "artifact_instance_id"));
			buy = GameResID(EGameResID::GOLD);
			break;
		case EMarketMode::CREATURE_EXP:
			requireHero();
			sell = readValidSlot("slot");
			buy = GameResID(EGameResID::GOLD);
			amount = readPositiveAmount("amount");
			break;
		case EMarketMode::CREATURE_UNDEAD:
			sell = readValidSlot("slot");
			buy = GameResID(EGameResID::GOLD);
			break;
		case EMarketMode::RESOURCE_SKILL:
			requireHero();
			sell = GameResID(EGameResID::GOLD);
			buy = SecondarySkill(readInteger(action, "skill_id"));
			break;
		case EMarketMode::MARKET_AFTER_LAST_PLACEHOLDER:
			throw std::invalid_argument("Invalid market mode id");
		}

		const RequestWaitResult request = submitAndWaitForRequest(typeid(TradeOnMarketplace), CTypeList::getInstance().getTypeID<TradeOnMarketplace>(nullptr), [&]
		{
			cc->trade(object->id, mode, sell, buy, amount, hero);
		});
		actionResult["market_id"] = JsonNode(object->id.getNum());
		actionResult["mode_id"] = JsonNode(modeID);
		actionResult["mode"] = JsonNode(marketModeName(mode));
		if(hero)
			actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["amount"] = JsonNode(static_cast<int32_t>(amount));
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Market trade request was rejected by server" : "Market trade request was not realized by server");
		return true;
	}

	if(type == "nullkiller_tasks")
	{
		const JsonNode candidates = makeNullkillerTaskCandidates(action);
		actionResult["ok"] = JsonNode(true);
		actionResult["nullkiller"] = candidates;
		return true;
	}

	if(type == "nullkiller_task")
		return executeNullkillerTaskAction(action, actionResult);

	if(type == "nullkiller_step")
	{
		const JsonNode candidates = makeNullkillerTaskCandidates(action);
		actionResult["nullkiller"] = candidates;
		const auto & tasks = candidates["tasks"].Vector();
		if(tasks.empty())
		{
			actionResult["ok"] = JsonNode(true);
			actionResult["didExecute"] = JsonNode(false);
			return true;
		}

		const int32_t requestedMaxAttempts = readInteger(action, "max_attempts", static_cast<int32_t>(tasks.size()));
		const size_t maxAttempts = static_cast<size_t>(std::clamp<int32_t>(requestedMaxAttempts, 1, 64));
		NK2AI::Goals::TTaskVec nativeTasks;
		nativeTasks.reserve(tasks.size());
		for(const JsonNode & task : tasks)
		{
			const int32_t taskID = static_cast<int32_t>(task["task_id"].Integer());
			const auto taskIter = std::ranges::find_if(nullkillerTaskHandles, [taskID](const auto & entry)
			{
				return entry.first == taskID;
			});
			if(taskIter != nullkillerTaskHandles.end())
				nativeTasks.push_back(taskIter->second);
		}

		NK2AI::ScriptTaskExecutionResult result;
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			result = nullkiller->executeScriptTaskSequence(nativeTasks, maxAttempts);
		}

		actionResult["ok"] = JsonNode(true);
		actionResult["didExecute"] = JsonNode(result.executed);
		actionResult["attempted"] = JsonNode(result.attempted);
		actionResult["attempts"] = JsonNode(static_cast<int32_t>(result.attempts));
		actionResult["maxAttempts"] = JsonNode(static_cast<int32_t>(maxAttempts));
		actionResult["selectedTaskIndex"] = JsonNode(static_cast<int32_t>(result.selectedTaskIndex));
		actionResult["failureActionId"] = JsonNode(static_cast<int32_t>(result.failureAction));
		actionResult["failureAction"] = JsonNode(nullkillerTaskFailureActionName(result.failureAction));
		actionResult["shouldReplan"] = JsonNode(result.shouldReplan);
		actionResult["shouldStopTurn"] = JsonNode(result.stopTurn);
		actionResult["exhaustedCandidates"] = JsonNode(result.exhaustedCandidates);
		int32_t outcomeID = 0;
		std::string outcome = "failed";
		if(result.executed)
		{
			outcomeID = 1;
			outcome = "executed";
		}
		else if(result.shouldReplan)
		{
			outcomeID = 2;
			outcome = "replan";
		}
		else if(result.stopTurn)
		{
			outcomeID = 3;
			outcome = "stop_turn";
		}
		else if(result.exhaustedCandidates)
		{
			outcomeID = 4;
			outcome = "exhausted_candidates";
		}
		actionResult["outcomeId"] = JsonNode(outcomeID);
		actionResult["outcome"] = JsonNode(outcome);
		if(!tasks.empty() && result.selectedTaskIndex < tasks.size())
		{
			const JsonNode selectedTask = tasks[result.selectedTaskIndex];
			actionResult["selectedTask"] = selectedTask;
			actionResult["task_id"] = selectedTask["task_id"];
			if(hasField(selectedTask, "debugDescription"))
				actionResult["debugDescription"] = selectedTask["debugDescription"];
		}
		if(!result.error.empty())
			actionResult["error"] = JsonNode(result.error);

		if(result.executed)
		{
			for(const auto * heroInfo : cc->getHeroesInfo())
				AIGateway::pickBestArtifacts(cc, heroInfo);

			if(!waitTillFreeForScriptAction(actionResult, "nullkiller_step"))
				return false;

			return true;
		}

		actionResult["ok"] = JsonNode(true);
		actionResult["didExecute"] = JsonNode(false);
		return !result.stopTurn;
	}

	if(type == "dismiss_hero")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(DismissHero), CTypeList::getInstance().getTypeID<DismissHero>(nullptr), [&]
		{
			cc->dismissHero(hero);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Dismiss hero request was rejected by server" : "Dismiss hero request was not realized by server");
		return true;
	}

	if(type == "build_boat")
	{
		const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(action, "shipyard_id")), false);
		const IShipyard * shipyard = dynamic_cast<const IShipyard *>(object);
		if(!object || !shipyard)
			throw std::invalid_argument("Unknown visible shipyard object");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(BuildBoat), CTypeList::getInstance().getTypeID<BuildBoat>(nullptr), [&]
		{
			cc->buildBoat(shipyard);
		});
		actionResult["shipyard_id"] = JsonNode(object->id.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Build boat request was rejected by server" : "Build boat request was not realized by server");
		return true;
	}

	if(type == "dig")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(DigWithHero), CTypeList::getInstance().getTypeID<DigWithHero>(nullptr), [&]
		{
			cc->dig(hero);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Dig request was rejected by server" : "Dig request was not realized by server");
		return true;
	}

	if(type == "cast_spell")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const SpellID spellID(readInteger(action, "spell_id"));
		int3 destination(-1, -1, -1);
		if(hasField(action, "x") || hasField(action, "y") || hasField(action, "z"))
		{
			const int z = hasField(action, "z") ? readInteger(action, "z") : hero->visitablePos().z;
			destination = int3(readInteger(action, "x"), readInteger(action, "y"), z);
			if(!cc->isInTheMap(destination) || !cc->isVisibleFor(destination, playerID))
				throw std::invalid_argument("Adventure spell target tile is not visible to scripted AI");
		}

		const RequestWaitResult request = submitAndWaitForRequest(typeid(CastAdvSpell), CTypeList::getInstance().getTypeID<CastAdvSpell>(nullptr), [&]
		{
			cc->castSpell(hero, spellID, destination);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["spell_id"] = JsonNode(spellID.getNum());
		actionResult["target"] = jsonPosition(destination);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Cast spell request was rejected by server" : "Cast spell request was not realized by server");
		return true;
	}

	if(type == "buy_artifact")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");
		const CGTownInstance * town = hero->getVisitedTown();
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Hero must be visiting an owned town to buy this artifact");

		const ArtifactID artifactID(readInteger(action, "artifact_id"));
		const CArtifact * artifact = artifactID.toArtifact();
		if(!artifact)
			throw std::invalid_argument("Unknown artifact_id");

		if(artifactID == ArtifactID::SPELLBOOK)
		{
			if(!town->hasBuilt(BuildingID::MAGES_GUILD_1))
				throw std::invalid_argument("Cannot buy a spellbook without Mage Guild 1");
			if(hero->hasSpellbook())
				throw std::invalid_argument("Hero already has a spellbook");
			if(cc->getResourceAmount()[EGameResID::GOLD] < GameConstants::SPELLBOOK_GOLD_COST)
				throw std::invalid_argument("Not enough gold to buy a spellbook");
		}
		else
		{
			if(artifact->getWarMachine() == CreatureID::NONE)
				throw std::invalid_argument("buy_artifact supports spellbooks and war machines only");
			if(hero->hasArt(artifactID))
				throw std::invalid_argument("Hero already has this war machine");
			if(!town->isWarMachineAvailable(artifactID))
				throw std::invalid_argument("Requested war machine is not available in this town");
			if(cc->getResourceAmount()[EGameResID::GOLD] < static_cast<int64_t>(artifact->getPrice()))
				throw std::invalid_argument("Not enough gold to buy this war machine");
		}

		const RequestWaitResult request = submitAndWaitForRequest(typeid(BuyArtifact), CTypeList::getInstance().getTypeID<BuyArtifact>(nullptr), [&]
		{
			cc->buyArtifact(hero, artifactID);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["artifact_id"] = JsonNode(artifactID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Buy artifact request was rejected by server" : "Buy artifact request was not realized by server");
		return true;
	}

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
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["building_id"] = JsonNode(buildingID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
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
		actionResult["source_id"] = JsonNode(dwelling->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["level"] = JsonNode(level);
		actionResult["creature_id"] = JsonNode(creatureID.getNum());
		actionResult["amount"] = JsonNode(amount);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Recruit request was rejected by server" : "Recruit request was not realized by server");
		return true;
	}

	if(type == "hire_hero")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Unknown town or town is not owned by scripted AI");
		if(town->getVisitingHero())
			throw std::invalid_argument("Town has a visiting hero and cannot hire another hero");
		if(cc->getResourceAmount()[EGameResID::GOLD] < GameConstants::HERO_GOLD_COST)
			throw std::invalid_argument("Not enough gold to hire a hero");

		const HeroTypeID heroTypeID(readInteger(action, "hero_type_id"));
		const HeroTypeID nextHeroTypeID = hasField(action, "next_hero_type_id")
			? HeroTypeID(readInteger(action, "next_hero_type_id"))
			: HeroTypeID::NONE;
		const CGHeroInstance * heroToHire = nullptr;
		for(const CGHeroInstance * availableHero : cc->getAvailableHeroes(town))
		{
			if(availableHero && availableHero->getHeroTypeID() == heroTypeID)
			{
				heroToHire = availableHero;
				break;
			}
		}
		if(!heroToHire)
			throw std::invalid_argument("Requested hero_type_id is not available for hiring");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(HireHero), CTypeList::getInstance().getTypeID<HireHero>(nullptr), [&]
		{
			cc->recruitHero(town, heroToHire, nextHeroTypeID);
		});
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["hero_type_id"] = JsonNode(heroTypeID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Hire hero request was rejected by server" : "Hire hero request was not realized by server");
		return true;
	}

	if(type == "transfer_army")
	{
		const auto * source = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(readInteger(action, "source_id")), false));
		const auto * destination = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(readInteger(action, "destination_id")), false));
		if(!source || !destination)
			throw std::invalid_argument("Unknown army transfer source or destination");
		if(source->tempOwner != playerID || destination->tempOwner != playerID)
			throw std::invalid_argument("Army transfer source and destination must be owned by scripted AI");

		const SlotID sourceSlot(readInteger(action, "source_slot"));
		if(!bulkTransferWouldMoveAnything(source, destination, sourceSlot))
			throw std::invalid_argument("Army transfer would not move any creatures");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(BulkMoveArmy), CTypeList::getInstance().getTypeID<BulkMoveArmy>(nullptr), [&]
		{
			cc->bulkMoveArmy(source->id, destination->id, sourceSlot);
		});
		actionResult["source_id"] = JsonNode(source->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["source_slot"] = JsonNode(sourceSlot.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Army transfer request was rejected by server" : "Army transfer request was not realized by server");
		return true;
	}

	if(type == "swap_creatures" || type == "merge_stacks" || type == "split_stack")
	{
		const CArmedInstance * source = readOwnedArmy("source_id", "source");
		const CArmedInstance * destination = readOwnedArmy("destination_id", "destination");
		const SlotID sourceSlot = readValidSlot("source_slot");
		const SlotID destinationSlot = readValidSlot("destination_slot");
		requireStack(source, sourceSlot, "source");

		if(type == "merge_stacks")
			requireStack(destination, destinationSlot, "destination");
		const int32_t amount = type == "split_stack" ? readInteger(action, "amount") : 0;
		if(type == "split_stack" && amount <= 0)
			throw std::invalid_argument("split_stack amount must be positive");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(ArrangeStacks), CTypeList::getInstance().getTypeID<ArrangeStacks>(nullptr), [&]
		{
			if(type == "swap_creatures")
				cc->swapCreatures(source, destination, sourceSlot, destinationSlot);
			else if(type == "merge_stacks")
				cc->mergeStacks(source, destination, sourceSlot, destinationSlot);
			else
				cc->splitStack(source, destination, sourceSlot, destinationSlot, amount);
		});
		actionResult["source_id"] = JsonNode(source->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["source_slot"] = JsonNode(sourceSlot.getNum());
		actionResult["destination_slot"] = JsonNode(destinationSlot.getNum());
		if(type == "split_stack")
			actionResult["amount"] = JsonNode(amount);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Army stack arrangement request was rejected by server" : "Army stack arrangement request was not realized by server");
		return true;
	}

	if(type == "bulk_split_stack" || type == "bulk_merge_stacks" || type == "bulk_split_rebalance_stack")
	{
		const CArmedInstance * army = readOwnedArmy("army_id", "bulk operation");
		const SlotID sourceSlot = readValidSlot("source_slot");
		requireStack(army, sourceSlot, "source");
		const int32_t amount = readInteger(action, "amount", 1);
		if(type == "bulk_split_stack" && amount <= 0)
			throw std::invalid_argument("bulk_split_stack amount must be positive");

		const std::type_info * requestType = type == "bulk_split_stack"
			? &typeid(BulkSplitStack)
			: (type == "bulk_merge_stacks" ? &typeid(BulkMergeStacks) : &typeid(BulkSplitAndRebalanceStack));
		const uint16_t requestTypeID = type == "bulk_split_stack"
			? CTypeList::getInstance().getTypeID<BulkSplitStack>(nullptr)
			: (type == "bulk_merge_stacks" ? CTypeList::getInstance().getTypeID<BulkMergeStacks>(nullptr) : CTypeList::getInstance().getTypeID<BulkSplitAndRebalanceStack>(nullptr));
		const RequestWaitResult request = submitAndWaitForRequest(*requestType, requestTypeID, [&]
		{
			if(type == "bulk_split_stack")
				cc->bulkSplitStack(army->id, sourceSlot, amount);
			else if(type == "bulk_merge_stacks")
				cc->bulkMergeStacks(army->id, sourceSlot);
			else
				cc->bulkSplitAndRebalanceStack(army->id, sourceSlot);
		});
		actionResult["army_id"] = JsonNode(army->id.getNum());
		actionResult["source_slot"] = JsonNode(sourceSlot.getNum());
		if(type == "bulk_split_stack")
			actionResult["amount"] = JsonNode(amount);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Bulk army stack request was rejected by server" : "Bulk army stack request was not realized by server");
		return true;
	}

	if(type == "dismiss_creature")
	{
		const CArmedInstance * army = readOwnedArmy("army_id", "dismiss creature");
		const SlotID slot = readValidSlot("slot");
		requireStack(army, slot, "dismissed creature");
		if(army->stacksCount() < 2 && army->needsLastStack())
			throw std::invalid_argument("Cannot dismiss the last required creature stack");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(DisbandCreature), CTypeList::getInstance().getTypeID<DisbandCreature>(nullptr), [&]
		{
			cc->dismissCreature(army, slot);
		});
		actionResult["army_id"] = JsonNode(army->id.getNum());
		actionResult["slot"] = JsonNode(slot.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Dismiss creature request was rejected by server" : "Dismiss creature request was not realized by server");
		return true;
	}

	if(type == "upgrade_creature")
	{
		const CArmedInstance * army = readOwnedArmy("army_id", "upgrade creature");
		const SlotID slot = readValidSlot("slot");
		requireStack(army, slot, "upgraded creature");
		const CreatureID creatureID = hasField(action, "creature_id") ? CreatureID(readInteger(action, "creature_id")) : CreatureID::NONE;

		const RequestWaitResult request = submitAndWaitForRequest(typeid(UpgradeCreature), CTypeList::getInstance().getTypeID<UpgradeCreature>(nullptr), [&]
		{
			cc->upgradeCreature(army, slot, creatureID);
		});
		actionResult["army_id"] = JsonNode(army->id.getNum());
		actionResult["slot"] = JsonNode(slot.getNum());
		actionResult["creature_id"] = JsonNode(creatureID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Upgrade creature request was rejected by server" : "Upgrade creature request was not realized by server");
		return true;
	}

	if(type == "set_formation")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");
		const int32_t formationID = readInteger(action, "formation_id");
		if(formationID < static_cast<int32_t>(EArmyFormation::LOOSE) || formationID > static_cast<int32_t>(EArmyFormation::TIGHT))
			throw std::invalid_argument("Invalid formation_id");
		const EArmyFormation formation = static_cast<EArmyFormation>(formationID);

		const RequestWaitResult request = submitAndWaitForRequest(typeid(SetFormation), CTypeList::getInstance().getTypeID<SetFormation>(nullptr), [&]
		{
			cc->setFormation(hero, formation);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["formation_id"] = JsonNode(formationID);
		actionResult["formation"] = JsonNode(armyFormationName(formation));
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Set formation request was rejected by server" : "Set formation request was not realized by server");
		return true;
	}

	if(type == "set_tactics")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");
		const bool enabled = readBool(action, "enabled", true);

		const RequestWaitResult request = submitAndWaitForRequest(typeid(SetTactics), CTypeList::getInstance().getTypeID<SetTactics>(nullptr), [&]
		{
			cc->setTactics(hero, enabled);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["enabled"] = JsonNode(enabled);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Set tactics request was rejected by server" : "Set tactics request was not realized by server");
		return true;
	}

	if(type == "swap_garrison_hero")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Unknown town or town is not owned by scripted AI");
		if(!town->getVisitingHero() || !town->getGarrisonHero())
			throw std::invalid_argument("Town must have both visiting and garrison heroes to swap");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(GarrisonHeroSwap), CTypeList::getInstance().getTypeID<GarrisonHeroSwap>(nullptr), [&]
		{
			cc->swapGarrisonHero(town);
		});
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Swap garrison hero request was rejected by server" : "Swap garrison hero request was not realized by server");
		return true;
	}

	if(type == "pick_best_artifacts")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const CGHeroInstance * otherHero = nullptr;
		if(hasField(action, "other_hero_id"))
		{
			otherHero = cc->getHero(ObjectInstanceID(readInteger(action, "other_hero_id")));
			if(!otherHero || otherHero->tempOwner != playerID)
				throw std::invalid_argument("Unknown other hero or other hero is not owned by scripted AI");
			if(hero->visitablePos() != otherHero->visitablePos())
				throw std::invalid_argument("Heroes must be co-located for two-hero artifact preparation");
		}

		AIGateway::pickBestArtifacts(cc, hero, otherHero);
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		if(otherHero)
			actionResult["other_hero_id"] = JsonNode(otherHero->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "ignore_script_query")
	{
		const QueryID queryID(readInteger(action, "query_id"));
		removeScriptQuery(queryID);
		actionResult["query_id"] = JsonNode(queryID.getNum());
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "assemble_artifacts")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const ArtifactPosition slot(readInteger(action, "slot"));
		const CArtifactInstance * artifact = hero->getArt(slot);
		if(!artifact)
			throw std::invalid_argument("No artifact in requested assembly slot");

		const bool assemble = readBool(action, "assemble", true);
		ArtifactID artifactID = ArtifactID::NONE;
		if(assemble)
		{
			artifactID = ArtifactID(hasField(action, "artifact_id") ? readInteger(action, "artifact_id") : readInteger(action, "assemble_to_artifact_id"));
			const CArtifact * combinedArtifact = artifactID.toArtifact();
			if(!combinedArtifact || !combinedArtifact->isCombined())
				throw std::invalid_argument("assemble_artifacts requires a combined artifact_id");
			if(!vstd::contains(ArtifactUtils::assemblyPossibilities(hero, artifact->getTypeId()), combinedArtifact))
				throw std::invalid_argument("Requested combined artifact cannot be assembled from this hero's visible artifacts");
		}
		else
		{
			if(!artifact->isCombined())
				throw std::invalid_argument("Requested artifact is not a combined artifact");
			if(!artifact->hasParts())
				throw std::invalid_argument("Requested combined artifact is fused and cannot be disassembled");
			if(ArtifactUtils::isSlotBackpack(slot)
				&& !ArtifactUtils::isBackpackFreeSlots(hero, artifact->getType()->getConstituents().size() - 1))
				throw std::invalid_argument("Not enough backpack space to disassemble artifact");
		}

		const RequestWaitResult request = submitAndWaitForRequest(typeid(AssembleArtifacts), CTypeList::getInstance().getTypeID<AssembleArtifacts>(nullptr), [&]
		{
			cc->assembleArtifacts(hero->id, slot, assemble, artifactID);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["slot"] = JsonNode(slot.getNum());
		actionResult["assemble"] = JsonNode(assemble);
		actionResult["artifact_id"] = JsonNode(artifactID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(request.applied)
			removeArtifactAssemblyPrompts(hero->id, slot);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Artifact assembly request was rejected by server" : "Artifact assembly request was not realized by server");
		return true;
	}

	if(type == "swap_artifacts")
	{
		const ArtifactLocation src = readArtifactLocation(action, "src");
		const ArtifactLocation dst = readArtifactLocation(action, "dst");
		const auto validateArtifactHolder = [&](const ArtifactLocation & location, const std::string & label)
		{
			const auto * holder = dynamic_cast<const CArtifactSet *>(cc->getObj(location.artHolder, false));
			const auto * object = dynamic_cast<const CGObjectInstance *>(holder);
			if(!holder || !object || object->tempOwner != playerID)
				throw std::invalid_argument("Artifact " + label + " holder is unknown, hidden, or not owned by scripted AI");
			if(!ArtifactUtils::checkIfSlotValid(*holder, location.slot))
				throw std::invalid_argument("Artifact " + label + " slot is invalid for holder");
		};
		validateArtifactHolder(src, "source");
		validateArtifactHolder(dst, "destination");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(ExchangeArtifacts), CTypeList::getInstance().getTypeID<ExchangeArtifacts>(nullptr), [&]
		{
			cc->swapArtifacts(src, dst);
		});
		actionResult["src"]["holder_id"] = JsonNode(src.artHolder.getNum());
		actionResult["src"]["slot"] = JsonNode(src.slot.getNum());
		actionResult["dst"]["holder_id"] = JsonNode(dst.artHolder.getNum());
		actionResult["dst"]["slot"] = JsonNode(dst.slot.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Swap artifacts request was rejected by server" : "Swap artifacts request was not realized by server");
		return true;
	}

	if(type == "bulk_move_artifacts")
	{
		const CGHeroInstance * srcHero = cc->getHero(ObjectInstanceID(readInteger(action, "src_hero_id")));
		const CGHeroInstance * dstHero = cc->getHero(ObjectInstanceID(readInteger(action, "dst_hero_id")));
		if(!srcHero || !dstHero || srcHero->tempOwner != playerID || dstHero->tempOwner != playerID)
			throw std::invalid_argument("Artifact bulk move heroes must both be owned by scripted AI");
		if(srcHero->visitablePos() != dstHero->visitablePos())
			throw std::invalid_argument("Heroes must be co-located for artifact bulk movement");

		const bool swap = readBool(action, "swap", false);
		const bool equipped = readBool(action, "equipped", true);
		const bool backpack = readBool(action, "backpack", true);
		const RequestWaitResult request = submitAndWaitForRequest(typeid(BulkExchangeArtifacts), CTypeList::getInstance().getTypeID<BulkExchangeArtifacts>(nullptr), [&]
		{
			cc->bulkMoveArtifacts(srcHero->id, dstHero->id, swap, equipped, backpack);
		});
		actionResult["src_hero_id"] = JsonNode(srcHero->id.getNum());
		actionResult["dst_hero_id"] = JsonNode(dstHero->id.getNum());
		actionResult["swap"] = JsonNode(swap);
		actionResult["equipped"] = JsonNode(equipped);
		actionResult["backpack"] = JsonNode(backpack);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Bulk artifact move request was rejected by server" : "Bulk artifact move request was not realized by server");
		return true;
	}

	if(type == "sort_backpack_artifacts")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const std::string mode = toLowerAscii(hasField(action, "mode") ? readString(action, "mode") : std::string("slot"));
		if(mode != "slot" && mode != "cost" && mode != "class")
			throw std::invalid_argument("Unsupported backpack artifact sort mode: " + mode);

		const RequestWaitResult request = submitAndWaitForRequest(typeid(ManageBackpackArtifacts), CTypeList::getInstance().getTypeID<ManageBackpackArtifacts>(nullptr), [&]
		{
			if(mode == "slot")
				cc->sortBackpackArtifactsBySlot(hero->id);
			else if(mode == "cost")
				cc->sortBackpackArtifactsByCost(hero->id);
			else
				cc->sortBackpackArtifactsByClass(hero->id);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["mode"] = JsonNode(mode);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Backpack artifact sort request was rejected by server" : "Backpack artifact sort request was not realized by server");
		return true;
	}

	if(type == "scroll_backpack_artifacts")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const bool left = readBool(action, "left", true);
		const RequestWaitResult request = submitAndWaitForRequest(typeid(ManageBackpackArtifacts), CTypeList::getInstance().getTypeID<ManageBackpackArtifacts>(nullptr), [&]
		{
			cc->scrollBackpackArtifacts(hero->id, left);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["left"] = JsonNode(left);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Backpack artifact scroll request was rejected by server" : "Backpack artifact scroll request was not realized by server");
		return true;
	}

	if(type == "manage_hero_costume")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const int32_t costumeIndex = readInteger(action, "costume_index", 0);
		if(costumeIndex < 0)
			throw std::invalid_argument("costume_index must be non-negative");
		const bool saveCostume = readBool(action, "save", false);
		const RequestWaitResult request = submitAndWaitForRequest(typeid(ManageEquippedArtifacts), CTypeList::getInstance().getTypeID<ManageEquippedArtifacts>(nullptr), [&]
		{
			cc->manageHeroCostume(hero->id, static_cast<size_t>(costumeIndex), saveCostume);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["costume_index"] = JsonNode(costumeIndex);
		actionResult["save"] = JsonNode(saveCostume);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Hero costume request was rejected by server" : "Hero costume request was not realized by server");
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
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["route_id"] = JsonNode(route.routeID);
		actionResult["destination"] = jsonPosition(route.destination);
		actionResult["submittedPath"] = jsonPositions(route.requestPath);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
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
		actionResult["query_id"] = JsonNode(queryID.getNum());
		actionResult["answer"] = JsonNode(answer);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
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

AI::AdventureScriptInput CScriptedAdventureAI::makeAdventureScriptInput(const JsonNode & progress)
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
	return input;
}

JsonNode CScriptedAdventureAI::makeScriptInputState()
{
	JsonNode state;
	std::shared_lock gameStateLock(CGameState::mutex);
	constexpr size_t maxVisibleTileSamples = 256;
	constexpr size_t maxVisibleObjects = 512;

	state["day"] = JsonNode(cc->getCalendar().getCurrentDay());
	state["player"]["id"] = JsonNode(playerID.getNum());
	state["player"]["color"] = JsonNode(playerID.toString());
	state["turn"]["active"] = JsonNode(status.haveTurn());
	state["turn"]["pendingQueries"] = JsonNode(status.getQueriesCount());
	state["turn"]["queries"] = makeScriptQueries();
	state["battle"]["state"] = JsonNode(battleStateName(status.getBattle()));
	const int3 mapSize = cc->getMapSize();
	state["map"]["width"] = JsonNode(mapSize.x);
	state["map"]["height"] = JsonNode(mapSize.y);
	state["map"]["levels"] = JsonNode(mapSize.z);

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

	std::map<int32_t, NK2AI::HeroRole> heroRoles;
	std::map<int32_t, float> heroFightingScores;
	std::map<int32_t, float> heroMagicScores;
	{
		std::unique_lock aiLock(nullkiller->aiStateMutex);
		nullkiller->heroManager->update();
		for(const CGHeroInstance * hero : cc->getHeroesInfo())
		{
			if(!hero)
				continue;

			const int32_t heroID = hero->id.getNum();
			heroRoles[heroID] = nullkiller->heroManager->getHeroRoleOrDefault(NK2AI::HeroPtr(hero, cc.get()));
			heroFightingScores[heroID] = nullkiller->heroManager->evaluateHero(hero);
			heroMagicScores[heroID] = nullkiller->heroManager->getMagicStrength(hero);
		}
	}

	state["heroes"].Vector();
	for(const CGHeroInstance * hero : cc->getHeroesInfo())
	{
		if(hero)
		{
			JsonNode heroNode = jsonHero(hero);
			const int32_t heroID = hero->id.getNum();
			if(const auto role = heroRoles.find(heroID); role != heroRoles.end())
			{
				heroNode["nullkillerRoleId"] = JsonNode(static_cast<int32_t>(role->second));
				heroNode["nullkillerRole"] = JsonNode(nullkillerHeroRoleName(role->second));
			}
			if(const auto fightingScore = heroFightingScores.find(heroID); fightingScore != heroFightingScores.end())
				heroNode["nullkillerFightingScore"].Float() = fightingScore->second;
			if(const auto magicScore = heroMagicScores.find(heroID); magicScore != heroMagicScores.end())
				heroNode["nullkillerMagicScore"].Float() = magicScore->second;
			state["heroes"].Vector().push_back(heroNode);
		}
	}

	const ResourceSet resources = cc->getResourceAmount();
	state["towns"].Vector();
	for(const CGTownInstance * town : cc->getTownsInfo())
	{
		if(town)
			state["towns"].Vector().push_back(jsonTown(town, resources));
	}

	const std::vector<int3> visibleTiles = visibleMapTiles(cc, playerID);

	state["map"]["visibleTilesCount"] = JsonNode(static_cast<int32_t>(visibleTiles.size()));
	state["map"]["visibleTileSampleLimit"] = JsonNode(static_cast<int32_t>(maxVisibleTileSamples));
	state["map"]["visibleTiles"].Vector();
	state["map"]["visibleObjects"].Vector();

	std::set<int32_t> seenVisibleObjects;
	size_t visibleObjectCount = 0;
	for(const int3 & position : visibleTiles)
	{
		if(state["map"]["visibleTiles"].Vector().size() < maxVisibleTileSamples)
		{
			if(const TerrainTile * tile = cc->getTile(position, false))
				state["map"]["visibleTiles"].Vector().push_back(jsonVisibleTile(position, *tile));
		}

		const TerrainTile * tile = cc->getTile(position, false);
		if(!tile)
			continue;

		auto appendVisibleObject = [&](ObjectInstanceID objectID)
		{
			if(objectID == ObjectInstanceID())
				return;
			if(!seenVisibleObjects.insert(objectID.getNum()).second)
				return;

			const CGObjectInstance * object = cc->getObj(objectID, false);
			if(!object || !cc->isVisibleFor(object, playerID))
				return;

			++visibleObjectCount;
			if(state["map"]["visibleObjects"].Vector().size() < maxVisibleObjects)
				state["map"]["visibleObjects"].Vector().push_back(jsonMapObject(object, playerID, nullptr));
		};

		for(ObjectInstanceID objectID : tile->visitableObjects)
			appendVisibleObject(objectID);
		for(ObjectInstanceID objectID : tile->blockingObjects)
			appendVisibleObject(objectID);
	}
	state["map"]["visibleObjectsCount"] = JsonNode(static_cast<int32_t>(visibleObjectCount));
	state["map"]["visibleObjectLimit"] = JsonNode(static_cast<int32_t>(maxVisibleObjects));
	state["map"]["visibleObjectsTruncated"] = JsonNode(visibleObjectCount > maxVisibleObjects);
	state["map"]["visibleTilesTruncated"] = JsonNode(visibleTiles.size() > maxVisibleTileSamples);
	return state;
}

JsonNode CScriptedAdventureAI::makeScriptActionSpace() const
{
	JsonNode actionSpace;
	actionSpace["acceptedActionTypes"].Vector();
	for(const std::string & type : AI::acceptedPlanActionTypes())
		actionSpace["acceptedActionTypes"].Vector().push_back(JsonNode(type));
	for(const char * type : { "pick_best_artifacts", "swap_artifacts", "bulk_move_artifacts", "sort_backpack_artifacts", "scroll_backpack_artifacts", "manage_hero_costume", "assemble_artifacts", "ignore_script_query", "swap_creatures", "merge_stacks", "split_stack", "bulk_split_stack", "bulk_merge_stacks", "bulk_split_rebalance_stack", "dismiss_creature", "upgrade_creature", "set_formation", "set_tactics", "swap_garrison_hero", "nullkiller_trade", "trade_resources", "market_trade", "dismiss_hero", "build_boat", "dig", "cast_spell", "buy_artifact", "nullkiller_tasks", "nullkiller_task", "nullkiller_step" })
		actionSpace["acceptedActionTypes"].Vector().push_back(JsonNode(type));

	actionSpace["buildOptions"].Vector();
	actionSpace["recruitOptions"].Vector();
	actionSpace["hireHeroOptions"].Vector();
	actionSpace["armyTransferOptions"].Vector();
	actionSpace["upgradeCreatureOptions"].Vector();
	actionSpace["reachableObjects"].Vector();
	actionSpace["movementOptions"].Vector();
	actionSpace["shipyardOptions"].Vector();
	actionSpace["digOptions"].Vector();
	actionSpace["adventureSpellOptions"].Vector();
	actionSpace["buyArtifactOptions"].Vector();
	actionSpace["recommendedActions"].Vector();

	std::shared_lock gameStateLock(CGameState::mutex);
	const ResourceSet resources = cc->getResourceAmount();
	const std::vector<int3> visibleTiles = visibleMapTiles(cc, playerID);

	std::set<int32_t> seenUpgradeArmies;
	auto appendUpgradeOptions = [&](const CArmedInstance * army)
	{
		if(!army || army->tempOwner != playerID || !seenUpgradeArmies.insert(army->id.getNum()).second)
			return;

		for(int32_t slotIndex = 0; slotIndex < GameConstants::ARMY_SIZE; ++slotIndex)
		{
			const SlotID slot(slotIndex);
			const CStackInstance * stack = army->getStackPtr(slot);
			if(!stack || stack->getCount() <= 0)
				continue;

			UpgradeInfo upgradeInfo(stack->getId());
			cc->fillUpgradeInfo(army, slot, upgradeInfo);
			for(const CreatureID & upgradeID : upgradeInfo.getAvailableUpgrades())
			{
				const ResourceSet unitCost = upgradeInfo.getUpgradeCostsFor(upgradeID);
				const ResourceSet totalCost = unitCost * stack->getCount();
				JsonNode option = jsonUpgradeCreatureOption(army, slot, stack, upgradeID, unitCost, totalCost, resources);
				actionSpace["upgradeCreatureOptions"].Vector().push_back(option);
				if(option["affordable"].Bool() && option["value"].Integer() > 0)
					actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
			}
		}
	};

	for(const CGTownInstance * town : cc->getTownsInfo(true))
	{
		if(!town || town->tempOwner != playerID)
			continue;

		appendUpgradeOptions(town);
		appendUpgradeOptions(town->getVisitingHero());
		appendUpgradeOptions(town->getGarrisonHero());

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

		if(scriptConfig.experimentalSupportActions && !town->getVisitingHero() && resources[EGameResID::GOLD] >= GameConstants::HERO_GOLD_COST)
		{
			for(const CGHeroInstance * hero : cc->getAvailableHeroes(town))
			{
				if(!hero)
					continue;

				JsonNode option = jsonAvailableHeroOption(town, hero);
				actionSpace["hireHeroOptions"].Vector().push_back(option);
				actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
			}
		}

		if(scriptConfig.experimentalSupportActions)
		{
			const CGHeroInstance * visitingHero = town->getVisitingHero();
			const CGHeroInstance * garrisonHero = town->getGarrisonHero();
			auto appendTransferOption = [&](const CArmedInstance * source, const CArmedInstance * destination, ScriptArmyTransferKind kind)
			{
				const std::optional<SlotID> sourceSlot = weakestTransferReserveSlot(source);
				if(!sourceSlot || !bulkTransferWouldMoveAnything(source, destination, *sourceSlot))
					return;

				JsonNode option = jsonArmyTransferOption(source, destination, *sourceSlot, kind, town->id.getNum());
				actionSpace["armyTransferOptions"].Vector().push_back(option);
				actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
			};

			if(visitingHero)
			{
				appendTransferOption(town, visitingHero, ScriptArmyTransferKind::GATHER_TO_HERO);
				appendTransferOption(visitingHero, town, ScriptArmyTransferKind::REINFORCE_TOWN);
			}
			if(visitingHero && garrisonHero)
			{
				appendTransferOption(garrisonHero, visitingHero, ScriptArmyTransferKind::GATHER_TO_HERO);
				appendTransferOption(visitingHero, garrisonHero, ScriptArmyTransferKind::REINFORCE_TOWN);
			}
		}

		if(const CGHeroInstance * visitingHero = town->getVisitingHero())
		{
			if(visitingHero->tempOwner == playerID)
			{
				if(town->hasBuilt(BuildingID::MAGES_GUILD_1) && !visitingHero->hasSpellbook())
				{
					JsonNode option = jsonBuyArtifactOption(visitingHero, ArtifactID::SPELLBOOK, resources);
					actionSpace["buyArtifactOptions"].Vector().push_back(option);
					if(option["affordable"].Bool())
						actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
				}

				const ArtifactID warMachine = town->getWarMachineInBuilding(BuildingID::BLACKSMITH);
				if(warMachine != ArtifactID::NONE && !visitingHero->hasArt(warMachine))
				{
					JsonNode option = jsonBuyArtifactOption(visitingHero, warMachine, resources);
					actionSpace["buyArtifactOptions"].Vector().push_back(option);
					if(option["affordable"].Bool())
						actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
				}
			}
		}
	}

	for(const CGHeroInstance * hero : cc->getHeroesInfo())
		appendUpgradeOptions(hero);

	std::set<int32_t> seenShipyards;
	for(const int3 & position : visibleTiles)
	{
		const TerrainTile * tile = cc->getTile(position, false);
		if(!tile)
			continue;

		auto appendShipyardOption = [&](ObjectInstanceID objectID)
		{
			if(objectID == ObjectInstanceID() || !seenShipyards.insert(objectID.getNum()).second)
				return;

			const CGObjectInstance * object = cc->getObj(objectID, false);
			const IShipyard * shipyard = dynamic_cast<const IShipyard *>(object);
			if(!object || !shipyard || !cc->isVisibleFor(object, playerID))
				return;

			const bool enemy = cc->getPlayerRelations(playerID, object->tempOwner) == PlayerRelations::ENEMIES;
			JsonNode option = jsonShipyardOption(object, shipyard, resources, enemy);
			actionSpace["shipyardOptions"].Vector().push_back(option);
		};

		for(ObjectInstanceID objectID : tile->visitableObjects)
			appendShipyardOption(objectID);
		for(ObjectInstanceID objectID : tile->blockingObjects)
			appendShipyardOption(objectID);
	}

	constexpr size_t maxAdventureSpellOptions = 64;
	constexpr size_t maxSpellTargetsPerSpell = 12;
	constexpr int spellTargetRadius = 12;
	std::set<std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t>> seenSpellTargets;

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
		if(!hero || hero->tempOwner != playerID)
			continue;

		JsonNode digOption = jsonDigOption(hero);
		actionSpace["digOptions"].Vector().push_back(digOption);

		for(const auto & spellPtr : LIBRARY->spellh->objects)
		{
			const CSpell * spell = spellPtr.get();
			if(!spell || !spell->isAdventure() || !hero->canCastThisSpell(spell))
				continue;

			auto appendSpellOption = [&](const std::optional<int3> & target, int32_t targetKindID, const std::string & targetKind)
			{
				if(actionSpace["adventureSpellOptions"].Vector().size() >= maxAdventureSpellOptions)
					return;

				const int3 keyTarget = target.value_or(int3(-1, -1, -1));
				if(!seenSpellTargets.emplace(hero->id.getNum(), spell->getId().getNum(), keyTarget.x, keyTarget.y, keyTarget.z).second)
					return;

				spells::detail::ProblemImpl problem;
				if(!spell->getAdventureMechanics().canBeCastAt(problem, cc.get(), hero, keyTarget))
					return;

				JsonNode option = jsonAdventureSpellOption(hero, spell, target, targetKindID, targetKind);
				actionSpace["adventureSpellOptions"].Vector().push_back(option);
			};

			appendSpellOption(std::nullopt, 0, "default");

			size_t targetsForSpell = 0;
			for(const CGTownInstance * town : cc->getTownsInfo(true))
			{
				if(targetsForSpell >= maxSpellTargetsPerSpell)
					break;
				if(!town || town->tempOwner != playerID)
					continue;

				const size_t before = actionSpace["adventureSpellOptions"].Vector().size();
				appendSpellOption(town->visitablePos(), 1, "owned_town");
				if(actionSpace["adventureSpellOptions"].Vector().size() > before)
					++targetsForSpell;
			}

			FowTilesType targetTiles;
			cc->getTilesInRange(targetTiles, hero->visitablePos(), spellTargetRadius, ETileVisibility::REVEALED, playerID);
			for(const int3 & target : targetTiles)
			{
				if(targetsForSpell >= maxSpellTargetsPerSpell)
					break;
				if(!cc->isInTheMap(target) || !cc->isVisibleFor(target, playerID))
					continue;

				const size_t before = actionSpace["adventureSpellOptions"].Vector().size();
				appendSpellOption(target, 2, "visible_tile");
				if(actionSpace["adventureSpellOptions"].Vector().size() > before)
					++targetsForSpell;
			}
		}

		if(hero->movementPointsRemaining() <= 0)
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
	analysis["candidateLimits"]["spellTargetRadius"] = JsonNode(12);
	analysis["candidateLimits"]["maxAdventureSpellOptions"] = JsonNode(64);
	analysis["candidateLimits"]["maxSpellTargetsPerSpell"] = JsonNode(12);
	analysis["candidateLimits"]["maxVisibleEnemyThreatTiles"] = JsonNode(128);
	analysis["candidateLimits"]["maxLockedObjectClusters"] = JsonNode(32);
	analysis["candidateLimits"]["maxLockedClusterObjects"] = JsonNode(16);
	analysis["candidateLimits"]["maxUpdateEvents"] = JsonNode(static_cast<int32_t>(maxScriptUpdateJournal));
	analysis["execution"]["validatesOwnership"] = JsonNode(true);
	analysis["execution"]["validatesVisibility"] = JsonNode(true);
	analysis["execution"]["validatesRouteIds"] = JsonNode(true);
	analysis["execution"]["replansAfterObjectVisit"] = JsonNode(true);
	analysis["execution"]["fallbackAI"] = JsonNode("Nullkiller2");
	analysis["execution"]["boundedNullkillerSubroutines"] = JsonNode(true);
	analysis["execution"]["nullkillerTaskHandlesExpireAfterRefresh"] = JsonNode(true);
	analysis["experimentalSupportActions"] = JsonNode(scriptConfig.experimentalSupportActions);
	analysis["scriptMemory"]["persistedInPlayerLocalSettings"] = JsonNode(true);
	analysis["scriptMemory"]["localStateKey"] = JsonNode(SCRIPT_MEMORY_LOCAL_STATE_KEY);
	analysis["candidateFields"].Vector();
	for(const char * field : { "reason", "value", "riskId", "risk", "safe", "danger", "dangerRatio", "estimatedLoss", "blockedBy", "kindId", "buildingKindId", "transferKindId", "pathActionId", "levelId", "statusId", "targetKindId", "spell_id", "task_id", "goalTypeId", "priorityTier", "heroRoleId", "nullkillerRoleId", "outcomeId", "failureActionId" })
		analysis["candidateFields"].Vector().push_back(JsonNode(field));
	analysis["danger"]["candidateDangerSource"] = JsonNode("Nullkiller direct object/guard danger evaluator");
	analysis["danger"]["enemyReachSource"] = JsonNode("visible enemy distance and strength alerts");
	analysis["danger"]["visibleEnemyThreatTiles"].Vector();
	analysis["danger"]["visibleEnemyThreatTileLimit"] = JsonNode(128);
	analysis["danger"]["visibleEnemyThreatTileCount"] = JsonNode(0);
	analysis["danger"]["visibleEnemyThreatTilesTruncated"] = JsonNode(false);
	analysis["visibleEnemyHeroes"].Vector();
	analysis["visibleEnemyTowns"].Vector();
	analysis["defenseAlerts"].Vector();
	analysis["heroThreatAlerts"].Vector();
	analysis["lockedObjectClusters"].Vector();
	analysis["lockedObjectClusterLimit"] = JsonNode(32);
	analysis["lockedObjectClusterCount"] = JsonNode(0);
	analysis["lockedObjectClustersTruncated"] = JsonNode(false);

	std::shared_lock gameStateLock(CGameState::mutex);
	const ResourceSet resources = cc->getResourceAmount();
	const std::vector<int3> visibleTiles = visibleMapTiles(cc, playerID);
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

	if(nullkiller && nullkiller->dangerHitMap && nullkiller->objectClusterizer)
	{
		constexpr size_t maxVisibleEnemyThreatTiles = 128;
		constexpr size_t maxLockedObjectClusters = 32;
		constexpr size_t maxLockedClusterObjects = 16;
		size_t visibleEnemyThreatTileCount = 0;
		size_t visibleLockedClusterCount = 0;

		std::unique_lock aiLock(nullkiller->aiStateMutex);
		if(nullkiller->dangerHitMap->isHitMapUpToDate())
		{
			for(const int3 & position : visibleTiles)
			{
				if(!cc->isInTheMap(position) || !cc->isVisibleFor(position, playerID))
					continue;

				const NK2AI::HitMapNode & threat = nullkiller->dangerHitMap->getTileThreat(position);
				if(!hasVisibleThreat(threat, cc, playerID))
					continue;

				++visibleEnemyThreatTileCount;
				if(analysis["danger"]["visibleEnemyThreatTiles"].Vector().size() >= maxVisibleEnemyThreatTiles)
				{
					analysis["danger"]["visibleEnemyThreatTilesTruncated"] = JsonNode(true);
					continue;
				}

				JsonNode tile;
				tile["position"] = jsonPosition(position);
				tile["fastest"] = jsonVisibleHitMapInfo(threat.fastestDanger, cc, playerID);
				tile["maximum"] = jsonVisibleHitMapInfo(threat.maximumDanger, cc, playerID);
				if(nullkiller->dangerHitMap->isTileOwnersUpToDate())
				{
					if(const CGTownInstance * closestTown = nullkiller->dangerHitMap->getClosestTown(position))
					{
						if(cc->isVisibleFor(closestTown, playerID))
						{
							tile["closestTownId"] = JsonNode(closestTown->id.getNum());
							tile["closestTownOwnerId"] = JsonNode(closestTown->getOwner().getNum());
							tile["closestTownPosition"] = jsonPosition(closestTown->visitablePos());
						}
					}
				}
				analysis["danger"]["visibleEnemyThreatTiles"].Vector().push_back(tile);
			}
			analysis["danger"]["visibleEnemyThreatTileCount"] = JsonNode(static_cast<int32_t>(visibleEnemyThreatTileCount));
		}

		if(nullkiller->objectClusterizer->isClusterizationUpToDate())
		{
			for(const std::shared_ptr<NK2AI::ObjectCluster> & cluster : nullkiller->objectClusterizer->getLockedClusters())
			{
				if(!cluster || !cluster->blocker || !cc->isVisibleFor(cluster->blocker, playerID))
					continue;

				JsonNode clusterNode;
				clusterNode["blocker_id"] = JsonNode(cluster->blocker->id.getNum());
				clusterNode["blocker"] = jsonMapObject(cluster->blocker, playerID, nullptr);
				clusterNode["objects"].Vector();
				clusterNode["objectLimit"] = JsonNode(static_cast<int32_t>(maxLockedClusterObjects));
				clusterNode["objectsTruncated"] = JsonNode(false);

				size_t visibleObjects = 0;
				for(const auto & entry : cluster->objects)
				{
					const CGObjectInstance * object = cc->getObj(entry.first, false);
					if(!object || !cc->isVisibleFor(object, playerID))
						continue;

					++visibleObjects;
					if(clusterNode["objects"].Vector().size() >= maxLockedClusterObjects)
					{
						clusterNode["objectsTruncated"] = JsonNode(true);
						continue;
					}

					const NK2AI::ClusterObjectInfo & info = entry.second;
					JsonNode objectNode;
					objectNode["object_id"] = JsonNode(object->id.getNum());
					objectNode["object"] = jsonMapObject(object, playerID, nullptr);
					objectNode["priority"] = JsonNode(static_cast<double>(info.priority));
					objectNode["movementCost"] = JsonNode(static_cast<double>(info.movementCost));
					objectNode["danger"] = JsonNode(static_cast<int64_t>(info.danger));
					objectNode["turn"] = JsonNode(static_cast<int32_t>(info.turn));
					clusterNode["objects"].Vector().push_back(objectNode);
				}
				clusterNode["visibleObjectCount"] = JsonNode(static_cast<int32_t>(visibleObjects));
				if(visibleObjects == 0)
					continue;

				++visibleLockedClusterCount;
				if(analysis["lockedObjectClusters"].Vector().size() >= maxLockedObjectClusters)
				{
					analysis["lockedObjectClustersTruncated"] = JsonNode(true);
					continue;
				}
				analysis["lockedObjectClusters"].Vector().push_back(clusterNode);
			}
			analysis["lockedObjectClusterCount"] = JsonNode(static_cast<int32_t>(visibleLockedClusterCount));
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
			const ScriptThreatLevel level = scriptThreatLevel(strengthRatio);

			JsonNode alert;
			alert["levelId"] = JsonNode(static_cast<int32_t>(level));
			alert["level"] = JsonNode(scriptThreatLevelName(level));
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
			const ScriptThreatLevel level = scriptThreatLevel(strengthRatio);

			JsonNode alert;
			alert["levelId"] = JsonNode(static_cast<int32_t>(level));
			alert["level"] = JsonNode(scriptThreatLevelName(level));
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
			"ScriptedAdventureAI config loaded: script '%s', reload per turn %d, trace %d, support actions %d, max calls %d, max actions %d, max memory bytes %d, max failures %d, disable turns %d",
			scriptPath.c_str(),
			scriptConfig.reloadScriptEachTurn,
			scriptConfig.trace,
			scriptConfig.experimentalSupportActions,
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
	scriptConfig.experimentalSupportActions = readBool(config, "experimentalSupportActions", scriptConfig.experimentalSupportActions);
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

void CScriptedAdventureAI::fallbackToNullkiller(const std::string & reason, bool recordFailure)
{
	logAi->warn("ScriptedAdventureAI falling back to Nullkiller: %s", reason.c_str());
	if(!recordFailure)
	{
		consecutiveScriptFailures = 0;
		return;
	}
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
