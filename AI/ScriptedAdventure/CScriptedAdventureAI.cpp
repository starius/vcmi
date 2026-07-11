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
#include "../../lib/IGameSettings.h"
#include "../../lib/ResourceSet.h"
#include "../../lib/StartInfo.h"
#include "../../lib/UnlockGuard.h"
#include "../../lib/VCMIDirs.h"
#include "../../lib/bonuses/Bonus.h"
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
#include "../../lib/entities/artifact/CArtifactFittingSet.h"
#include "../../lib/entities/artifact/ArtifactUtils.h"
#include "../../lib/entities/artifact/CArtifact.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/mapping/TerrainTile.h"
#include "../../lib/entities/hero/CHero.h"
#include "../../lib/entities/hero/CHeroClass.h"
#include "../../lib/entities/hero/CHeroHandler.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../lib/networkPacks/SaveLocalState.h"
#include "../../lib/pathfinder/CGPathNode.h"
#include "../../lib/pathfinder/PathfinderOptions.h"
#include "../../lib/serializer/CTypeList.h"
#include "../../lib/serializer/JsonSerializer.h"
#include "../../lib/spells/CSpell.h"
#include "../../lib/spells/CSpellHandler.h"
#include "../../lib/spells/Problem.h"
#include "../../lib/spells/ViewSpellInt.h"
#include "../../lib/spells/adventure/AdventureSpellEffect.h"
#include "../../lib/spells/adventure/AdventureSpellMechanics.h"
#include "../../lib/spells/adventure/DimensionDoorEffect.h"
#include "../../lib/spells/adventure/ReinforcementsEffect.h"
#include "../../lib/spells/adventure/RemoveObjectEffect.h"
#include "../../lib/spells/adventure/SummonBoatEffect.h"
#include "../../lib/spells/adventure/TownPortalEffect.h"
#include "../../lib/spells/adventure/ViewWorldEffect.h"
#include "../../luascript/LuaAdventureScriptRunner.h"
#include "../Nullkiller2/AIUtility.h"
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
#include "../Nullkiller2/Pathfinding/Actions/AdventureSpellCastMovementActions.h"
#include "../Nullkiller2/Pathfinding/Actions/BoatActions.h"
#include "../Nullkiller2/Pathfinding/Actions/DimensionDoorAction.h"
#include "../Nullkiller2/Pathfinding/Actions/QuestAction.h"
#include "../Nullkiller2/Pathfinding/Actions/TownPortalAction.h"
#include "../Nullkiller2/Pathfinding/Actions/WhirlpoolAction.h"
#include "../Nullkiller2/Pathfinding/AINodeStorage.h"

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
constexpr int32_t DEFAULT_NULLKILLER_TASK_CANDIDATES = 16;
constexpr int32_t DEFAULT_NULLKILLER_SERIALIZED_TASKS = 64;
constexpr int32_t MAX_EXPLICIT_NULLKILLER_TASK_CANDIDATES = 512;

bool hasField(const JsonNode & node, const std::string & field);
int32_t readInteger(const JsonNode & node, const std::string & field);
std::string readString(const JsonNode & node, const std::string & field);

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

enum class ScriptObjectControlKind : int32_t
{
	SELF = 0,
	ALLY = 1,
	ENEMY = 2,
	NEUTRAL = 3,
	UNFLAGGABLE = 4,
	CANNOT_DETERMINE = 5,
	UNKNOWN = 6
};

enum class ScriptArmyTransferKind : int32_t
{
	UNKNOWN = 0,
	GATHER_TO_HERO = 1,
	REINFORCE_TOWN = 2
};

enum class ScriptQueryKind : int32_t
{
	UNKNOWN = 0,
	HERO_LEVEL_UP = 1,
	COMMANDER_LEVEL_UP = 2,
	BLOCKING_DIALOG = 3,
	TELEPORT_DIALOG = 4,
	MAP_OBJECT_SELECT = 5,
	TAVERN_WINDOW = 6,
	HERO_EXCHANGE = 7,
	GARRISON_DIALOG = 8,
	RECRUITMENT_DIALOG = 9,
	UNIVERSITY_WINDOW = 10,
	MARKET_WINDOW = 11,
	ARTIFACT_ASSEMBLY_PROMPT = 12
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

enum class ScriptSpecialActionKind : int32_t
{
	UNKNOWN = 0,
	COMPOSITE = 1,
	DIMENSION_DOOR = 2,
	TOWN_PORTAL = 3,
	SUMMON_BOAT = 4,
	BUILD_BOAT = 5,
	WHIRLPOOL = 6,
	QUEST = 7,
	ADVENTURE_CAST = 8
};

enum class ScriptAdventureSpellKind : int32_t
{
	UNKNOWN = 0,
	GENERIC = 1,
	DIMENSION_DOOR = 2,
	TOWN_PORTAL = 3,
	SUMMON_BOAT = 4,
	REMOVE_OBJECT = 5,
	REINFORCEMENTS = 6,
	VIEW_WORLD = 7,
	WATER_WALK = 8,
	FLY = 9
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

const char * scriptObjectControlKindName(ScriptObjectControlKind kind)
{
	switch(kind)
	{
	case ScriptObjectControlKind::SELF:
		return "self";
	case ScriptObjectControlKind::ALLY:
		return "ally";
	case ScriptObjectControlKind::ENEMY:
		return "enemy";
	case ScriptObjectControlKind::NEUTRAL:
		return "neutral";
	case ScriptObjectControlKind::UNFLAGGABLE:
		return "unflaggable";
	case ScriptObjectControlKind::CANNOT_DETERMINE:
		return "cannot_determine";
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

ScriptQueryKind scriptQueryKind(const std::string & type)
{
	if(type == "hero_level_up")
		return ScriptQueryKind::HERO_LEVEL_UP;
	if(type == "commander_level_up")
		return ScriptQueryKind::COMMANDER_LEVEL_UP;
	if(type == "blocking_dialog")
		return ScriptQueryKind::BLOCKING_DIALOG;
	if(type == "teleport_dialog")
		return ScriptQueryKind::TELEPORT_DIALOG;
	if(type == "map_object_select")
		return ScriptQueryKind::MAP_OBJECT_SELECT;
	if(type == "tavern_window")
		return ScriptQueryKind::TAVERN_WINDOW;
	if(type == "hero_exchange")
		return ScriptQueryKind::HERO_EXCHANGE;
	if(type == "garrison_dialog")
		return ScriptQueryKind::GARRISON_DIALOG;
	if(type == "recruitment_dialog")
		return ScriptQueryKind::RECRUITMENT_DIALOG;
	if(type == "university_window")
		return ScriptQueryKind::UNIVERSITY_WINDOW;
	if(type == "market_window")
		return ScriptQueryKind::MARKET_WINDOW;
	if(type == "artifact_assembly_prompt")
		return ScriptQueryKind::ARTIFACT_ASSEMBLY_PROMPT;
	return ScriptQueryKind::UNKNOWN;
}

const std::vector<std::pair<int32_t, std::string>> & scriptActionTypeRegistry()
{
	static const std::vector<std::pair<int32_t, std::string>> registry = {
		{1, "build"},
		{2, "recruit"},
		{3, "hire_hero"},
		{4, "transfer_army"},
		{5, "move_hero"},
		{6, "visit_object"},
		{7, "answer_query"},
		{8, "cancel_query"},
		{9, "end_turn"},
		{20, "pick_best_creatures"},
		{21, "pick_best_artifacts"},
		{22, "prepare_hero"},
		{23, "swap_artifacts"},
		{24, "bulk_move_artifacts"},
		{25, "sort_backpack_artifacts"},
		{26, "scroll_backpack_artifacts"},
		{27, "manage_hero_costume"},
		{28, "assemble_artifacts"},
		{29, "ignore_script_query"},
		{30, "erase_transition_artifact"},
		{40, "swap_creatures"},
		{41, "merge_stacks"},
		{42, "merge_or_swap_stacks"},
		{43, "split_stack"},
		{44, "bulk_move_army"},
		{45, "bulk_split_stack"},
		{46, "bulk_merge_stacks"},
		{47, "bulk_split_rebalance_stack"},
		{48, "dismiss_creature"},
		{49, "upgrade_creature"},
		{50, "set_formation"},
		{51, "set_tactics"},
		{52, "set_town_name"},
		{53, "swap_garrison_hero"},
		{60, "trade_resources"},
		{61, "market_trade"},
		{62, "request_statistic"},
		{63, "dismiss_hero"},
		{64, "build_boat"},
		{65, "castle_teleport"},
		{66, "dig"},
		{67, "cast_spell"},
		{68, "buy_artifact"},
		{69, "spell_research"},
		{70, "visit_town_building"},
		{100, "nullkiller_reset"},
		{101, "nullkiller_lock_resources"},
		{102, "nullkiller_lock_hero"},
		{103, "nullkiller_unlock_hero"},
		{104, "nullkiller_trade"},
		{105, "nullkiller_priority_pass"},
		{106, "nullkiller_turn_slice"},
		{107, "nullkiller_build_army"},
		{108, "nullkiller_upgrade_army"},
		{109, "nullkiller_recruit_creatures"},
		{110, "nullkiller_move_creatures_to_hero"},
		{111, "nullkiller_dismiss_weak_hero"},
		{112, "nullkiller_optimize_artifacts"},
		{113, "nullkiller_add_single_creature_stacks"},
		{114, "nullkiller_rearrange_for_whirlpool"},
		{115, "nullkiller_rearrange_for_siege"},
		{116, "nullkiller_tasks"},
		{117, "nullkiller_task"},
		{118, "nullkiller_step"},
		{119, "nullkiller_pass"},
		{120, "nullkiller_answer_query"},
		{121, "nullkiller_object_interaction"},
	};
	return registry;
}

int32_t scriptActionTypeId(const std::string & type)
{
	for(const auto & entry : scriptActionTypeRegistry())
	{
		if(entry.second == type)
			return entry.first;
	}
	return 0;
}

void setScriptActionType(JsonNode & action, const std::string & type)
{
	action["type"] = JsonNode(type);
	const int32_t typeId = scriptActionTypeId(type);
	if(typeId == 0)
		throw std::invalid_argument("Script action type has no stable numeric id: " + type);
	action["type_id"] = JsonNode(typeId);
}

std::string scriptActionTypeFromId(int32_t typeId)
{
	for(const auto & entry : scriptActionTypeRegistry())
	{
		if(entry.first == typeId)
			return entry.second;
	}
	return {};
}

std::optional<int32_t> readScriptActionTypeId(const JsonNode & action)
{
	if(hasField(action, "type_id"))
		return readInteger(action, "type_id");
	return std::nullopt;
}

std::string readScriptActionType(const JsonNode & action)
{
	const std::optional<int32_t> typeId = readScriptActionTypeId(action);
	if(hasField(action, "type"))
	{
		const std::string type = readString(action, "type");
		if(typeId)
		{
			const int32_t expectedTypeId = scriptActionTypeId(type);
			if(expectedTypeId == 0)
				throw std::invalid_argument("Script action type has no stable numeric id: " + type);
			if(expectedTypeId != *typeId)
				throw std::invalid_argument("Script action type_id does not match type string: " + type);
		}
		return type;
	}

	if(typeId)
	{
		const std::string type = scriptActionTypeFromId(*typeId);
		if(type.empty())
			throw std::invalid_argument("Unknown script action type_id: " + std::to_string(*typeId));
		return type;
	}

	return readString(action, "type");
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

const char * scriptSpecialActionKindName(ScriptSpecialActionKind kind)
{
	switch(kind)
	{
	case ScriptSpecialActionKind::COMPOSITE:
		return "composite";
	case ScriptSpecialActionKind::DIMENSION_DOOR:
		return "dimension_door";
	case ScriptSpecialActionKind::TOWN_PORTAL:
		return "town_portal";
	case ScriptSpecialActionKind::SUMMON_BOAT:
		return "summon_boat";
	case ScriptSpecialActionKind::BUILD_BOAT:
		return "build_boat";
	case ScriptSpecialActionKind::WHIRLPOOL:
		return "whirlpool";
	case ScriptSpecialActionKind::QUEST:
		return "quest";
	case ScriptSpecialActionKind::ADVENTURE_CAST:
		return "adventure_cast";
	default:
		return "unknown";
	}
}

const char * scriptAdventureSpellKindName(ScriptAdventureSpellKind kind)
{
	switch(kind)
	{
	case ScriptAdventureSpellKind::GENERIC:
		return "generic";
	case ScriptAdventureSpellKind::DIMENSION_DOOR:
		return "dimension_door";
	case ScriptAdventureSpellKind::TOWN_PORTAL:
		return "town_portal";
	case ScriptAdventureSpellKind::SUMMON_BOAT:
		return "summon_boat";
	case ScriptAdventureSpellKind::REMOVE_OBJECT:
		return "remove_object";
	case ScriptAdventureSpellKind::REINFORCEMENTS:
		return "reinforcements";
	case ScriptAdventureSpellKind::VIEW_WORLD:
		return "view_world";
	case ScriptAdventureSpellKind::WATER_WALK:
		return "water_walk";
	case ScriptAdventureSpellKind::FLY:
		return "fly";
	default:
		return "unknown";
	}
}

ScriptAdventureSpellKind scriptAdventureSpellKind(const CSpell * spell, const CGHeroInstance * hero)
{
	const auto & mechanics = spell->getAdventureMechanics();

	if(mechanics.getEffectAs<DimensionDoorEffect>(hero))
		return ScriptAdventureSpellKind::DIMENSION_DOOR;
	if(mechanics.getEffectAs<TownPortalEffect>(hero))
		return ScriptAdventureSpellKind::TOWN_PORTAL;
	if(mechanics.getEffectAs<SummonBoatEffect>(hero))
		return ScriptAdventureSpellKind::SUMMON_BOAT;
	if(mechanics.getEffectAs<RemoveObjectEffect>(hero))
		return ScriptAdventureSpellKind::REMOVE_OBJECT;
	if(mechanics.getEffectAs<ReinforcementsEffect>(hero))
		return ScriptAdventureSpellKind::REINFORCEMENTS;
	if(mechanics.getEffectAs<ViewWorldEffect>(hero))
		return ScriptAdventureSpellKind::VIEW_WORLD;
	if(mechanics.givesBonus(hero, BonusType::FLYING_MOVEMENT))
		return ScriptAdventureSpellKind::FLY;
	if(mechanics.givesBonus(hero, BonusType::WATER_WALKING))
		return ScriptAdventureSpellKind::WATER_WALK;

	return ScriptAdventureSpellKind::GENERIC;
}

bool adventureSpellUsesNativeRouting(ScriptAdventureSpellKind kind)
{
	switch(kind)
	{
	case ScriptAdventureSpellKind::DIMENSION_DOOR:
	case ScriptAdventureSpellKind::TOWN_PORTAL:
	case ScriptAdventureSpellKind::SUMMON_BOAT:
	case ScriptAdventureSpellKind::WATER_WALK:
	case ScriptAdventureSpellKind::FLY:
		return true;
	default:
		return false;
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

template<typename Identifier>
std::string stableIdentifier(Identifier identifier)
{
	try
	{
		return Identifier::encode(identifier.getNum());
	}
	catch(const std::exception &)
	{
		return std::to_string(identifier.getNum());
	}
}

std::string buildingIdentifier(const CGTownInstance * town, BuildingID buildingID)
{
	try
	{
		const auto & building = town->getTown()->buildings.at(buildingID);
		return building ? building->getJsonKey() : std::to_string(buildingID.getNum());
	}
	catch(const std::exception &)
	{
		return std::to_string(buildingID.getNum());
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

int32_t readIntegerValue(const JsonNode & node, const std::string & label)
{
	if(!node.isNumber())
		throw std::invalid_argument("Missing or non-integer script action value: " + label);

	if(node.getType() == JsonNode::JsonType::DATA_INTEGER)
		return static_cast<int32_t>(node.Integer());

	const double value = node.Float();
	const double integralValue = std::trunc(value);
	if(!std::isfinite(value)
		|| value != integralValue
		|| value < static_cast<double>(std::numeric_limits<int32_t>::min())
		|| value > static_cast<double>(std::numeric_limits<int32_t>::max()))
	{
		throw std::invalid_argument("Missing or non-integer script action value: " + label);
	}
	return static_cast<int32_t>(integralValue);
}

int32_t readInteger(const JsonNode & node, const std::string & field, int32_t defaultValue)
{
	if(!hasField(node, field))
		return defaultValue;
	return readInteger(node, field);
}

size_t readNullkillerCandidateLimit(const JsonNode & node, const std::string & field, int32_t defaultValue)
{
	const int32_t requested = readInteger(node, field, defaultValue);
	if(requested == 0)
		return std::numeric_limits<size_t>::max();
	return static_cast<size_t>(std::clamp<int32_t>(requested, 1, MAX_EXPLICIT_NULLKILLER_TASK_CANDIDATES));
}

size_t readNullkillerSerializedTaskLimit(const JsonNode & node, size_t defaultValue)
{
	if(!hasField(node, "candidate_details_limit"))
		return defaultValue;

	const int32_t requested = readInteger(node, "candidate_details_limit");
	if(requested == 0)
		return std::numeric_limits<size_t>::max();
	return static_cast<size_t>(std::clamp<int32_t>(requested, 1, MAX_EXPLICIT_NULLKILLER_TASK_CANDIDATES));
}

int32_t jsonNullkillerLimit(size_t limit)
{
	return limit == std::numeric_limits<size_t>::max()
		? 0
		: static_cast<int32_t>(std::min<size_t>(limit, static_cast<size_t>(std::numeric_limits<int32_t>::max())));
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
		case static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::STARTUP):
			return NK2AI::ScriptTaskSearchMode::STARTUP;
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
	if(mode == "startup" || mode == "start_up" || mode == "start-up")
		return NK2AI::ScriptTaskSearchMode::STARTUP;
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

NK2AI::HeroLockedReason readNullkillerHeroLockedReason(const JsonNode & node)
{
	const int32_t reason = readInteger(node, "reason_id", static_cast<int32_t>(NK2AI::HeroLockedReason::DEFENCE));
	switch(reason)
	{
	case static_cast<int32_t>(NK2AI::HeroLockedReason::STARTUP):
		return NK2AI::HeroLockedReason::STARTUP;
	case static_cast<int32_t>(NK2AI::HeroLockedReason::DEFENCE):
		return NK2AI::HeroLockedReason::DEFENCE;
	case static_cast<int32_t>(NK2AI::HeroLockedReason::HERO_CHAIN):
		return NK2AI::HeroLockedReason::HERO_CHAIN;
	default:
		throw std::invalid_argument("Unsupported Nullkiller hero lock reason id");
	}
}

std::string nullkillerHeroLockedReasonName(NK2AI::HeroLockedReason reason)
{
	switch(reason)
	{
	case NK2AI::HeroLockedReason::NOT_LOCKED:
		return "none";
	case NK2AI::HeroLockedReason::STARTUP:
		return "startup";
	case NK2AI::HeroLockedReason::DEFENCE:
		return "defense";
	case NK2AI::HeroLockedReason::HERO_CHAIN:
		return "hero_chain";
	default:
		return "unknown";
	}
}

ResourceSet readResourceAmounts(const JsonNode & node, const std::string & field)
{
	const JsonNode & resources = hasField(node, field) ? node[field] : node;
	ResourceSet result;

	if(resources.isVector())
	{
		for(size_t index = 0; index < resources.Vector().size(); ++index)
		{
			if(index >= GameConstants::RESOURCE_QUANTITY)
				throw std::invalid_argument("Too many resource amounts in " + field);

			if(resources.Vector()[index].isStruct())
			{
				const int32_t resourceID = readInteger(resources.Vector()[index], "resource_id");
				if(resourceID < 0 || resourceID >= static_cast<int32_t>(GameConstants::RESOURCE_QUANTITY))
					throw std::invalid_argument("Invalid resource id in " + field);
				const int32_t amount = readInteger(resources.Vector()[index], "amount");
				if(amount < 0)
					throw std::invalid_argument("Resource lock amount must be non-negative");
				result[GameResID(resourceID)] += amount;
			}
			else
			{
				const int32_t amount = readIntegerValue(resources.Vector()[index], field + "[" + std::to_string(index) + "]");
				if(amount < 0)
					throw std::invalid_argument("Resource lock amount must be non-negative");
				result[GameResID(static_cast<int32_t>(index))] = amount;
			}
		}
		return result;
	}

	if(resources.isStruct())
	{
		if(hasField(resources, "resource_entries"))
			return readResourceAmounts(resources, "resource_entries");
		if(hasField(resources, "resources"))
			return readResourceAmounts(resources, "resources");

		for(int32_t resourceID = 0; resourceID < static_cast<int32_t>(GameConstants::RESOURCE_QUANTITY); ++resourceID)
		{
			const std::string fieldName = std::to_string(resourceID);
			if(!hasField(resources, fieldName))
				continue;
			const int32_t amount = readInteger(resources, fieldName);
			if(amount < 0)
				throw std::invalid_argument("Resource lock amount must be non-negative");
			result[GameResID(resourceID)] = amount;
		}
		return result;
	}

	throw std::invalid_argument("Resource amounts must be a vector or object: " + field);
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

std::string playerRelationName(PlayerRelations relation)
{
	switch(relation)
	{
	case PlayerRelations::ENEMIES:
		return "enemies";
	case PlayerRelations::ALLIES:
		return "allies";
	case PlayerRelations::SAME_PLAYER:
		return "same_player";
	}
	return "unknown";
}

std::string playerStatusName(EPlayerStatus status)
{
	switch(status)
	{
	case EPlayerStatus::WRONG:
		return "wrong";
	case EPlayerStatus::INGAME:
		return "ingame";
	case EPlayerStatus::LOSER:
		return "loser";
	case EPlayerStatus::WINNER:
		return "winner";
	}
	return "unknown";
}

ScriptObjectControlKind scriptObjectControlKind(const std::shared_ptr<CCallback> & callback, PlayerColor player, PlayerColor owner)
{
	if(owner == player)
		return ScriptObjectControlKind::SELF;
	if(owner == PlayerColor::NEUTRAL)
		return ScriptObjectControlKind::NEUTRAL;
	if(owner == PlayerColor::UNFLAGGABLE)
		return ScriptObjectControlKind::UNFLAGGABLE;
	if(owner == PlayerColor::CANNOT_DETERMINE)
		return ScriptObjectControlKind::CANNOT_DETERMINE;
	if(owner.isValidPlayer() && callback)
	{
		const PlayerRelations relation = callback->getPlayerRelations(player, owner);
		if(relation == PlayerRelations::ALLIES)
			return ScriptObjectControlKind::ALLY;
		if(relation == PlayerRelations::ENEMIES)
			return ScriptObjectControlKind::ENEMY;
	}
	return ScriptObjectControlKind::UNKNOWN;
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

bool usesSynchronousNativeAiRequests(const std::string & actionType)
{
	if(actionType == "pick_best_creatures"
		|| actionType == "pick_best_artifacts"
		|| actionType == "prepare_hero")
		return true;

	if(actionType == "nullkiller_tasks"
		|| actionType == "nullkiller_reset"
		|| actionType == "nullkiller_lock_resources"
		|| actionType == "nullkiller_lock_hero"
		|| actionType == "nullkiller_unlock_hero"
		|| actionType == "nullkiller_answer_query")
		return false;

	return actionType.starts_with("nullkiller_");
}

std::string componentTypeName(ComponentType type);
std::string infoWindowModeName(EInfoWindowMode mode);
std::string marketModeName(EMarketMode mode);

JsonNode jsonPosition(const int3 & position)
{
	JsonNode node;
	node["x"] = JsonNode(position.x);
	node["y"] = JsonNode(position.y);
	node["z"] = JsonNode(position.z);
	return node;
}

JsonNode jsonGrailInfo(const std::shared_ptr<CCallback> & callback)
{
	JsonNode node;
	node["knownRatio"].Float() = 0.0;
	node["fullyRevealed"] = JsonNode(false);
	node["positionKnown"] = JsonNode(false);

	if(!callback)
		return node;

	double knownRatio = 0.0;
	const int3 position = callback->getGrailPos(&knownRatio);
	knownRatio = std::clamp(knownRatio, 0.0, 1.0);
	node["knownRatio"].Float() = knownRatio;
	node["fullyRevealed"] = JsonNode(knownRatio >= 1.0);

	if(knownRatio >= 1.0 && position.isValid())
	{
		node["positionKnown"] = JsonNode(true);
		node["position"] = jsonPosition(position);
	}

	return node;
}

JsonNode jsonCalendar(const Calendar & calendar)
{
	JsonNode node;
	node["currentDay"] = JsonNode(calendar.getCurrentDay());
	node["dayOfWeek"] = JsonNode(calendar.getDayOfWeek());
	node["dayOfMonth"] = JsonNode(calendar.getDayOfMonth());
	node["week"] = JsonNode(calendar.getWeek());
	node["month"] = JsonNode(calendar.getMonth());
	node["daysInWeek"] = JsonNode(calendar.getDaysInWeek());
	node["weeksInMonth"] = JsonNode(calendar.getWeeksInMonth());
	node["daysInMonth"] = JsonNode(calendar.getDaysInMonth());
	return node;
}

JsonNode jsonObjectPosInfo(const ObjectPosInfo & info)
{
	JsonNode node;
	const MapObjectID objectID(info.id.getNum());
	const MapObjectSubID subtypeID(info.subId);
	const ScriptObjectKind kind = scriptObjectKind(objectID);
	node["position"] = jsonPosition(info.pos);
	node["typeId"] = JsonNode(objectID.getNum());
	node["subtypeId"] = JsonNode(subtypeID.getNum());
	node["kindId"] = JsonNode(static_cast<int32_t>(kind));
	node["kind"] = JsonNode(scriptObjectKindName(kind));
	node["typeIdentifier"] = JsonNode(mapObjectIdentifier(objectID));
	node["subtypeIdentifier"] = JsonNode(mapObjectSubtypeIdentifier(objectID, subtypeID));
	node["owner"] = JsonNode(jsonPlayerColor(info.owner));
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

JsonNode jsonStatisticDataSet(StatisticDataSet & statistic)
{
	JsonNode node;
	JsonSerializer serializer(nullptr, node);
	statistic.serializeJson(serializer);
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

JsonNode jsonMarketModeDetailsList(const IMarket * market)
{
	JsonNode node;
	node.Vector();
	if(!market)
		return node;

	for(EMarketMode mode : market->availableModes())
		node.Vector().push_back(jsonMarketModeDetails(market, mode));

	return node;
}

JsonNode jsonMarketSkillOption(
	const IMarket * market,
	const CGHeroInstance * visitor,
	SecondarySkill skillID,
	const ResourceSet & resources,
	const IGameSettings & settings,
	const NK2AI::HeroManager * heroManager = nullptr)
{
	const int32_t goldCost = settings.getInteger(EGameSettings::MARKETS_UNIVERSITY_GOLD_COST);
	const bool alreadyKnown = visitor && visitor->getSecSkillLevel(skillID) != 0;
	const bool canLearnAny = visitor && visitor->canLearnSkill();
	const bool canLearn = visitor && visitor->canLearnSkill(skillID);
	const bool affordable = resources[EGameResID::GOLD] >= goldCost;

	JsonNode node;
	node["skill_id"] = JsonNode(skillID.getNum());
	node["skillIdentifier"] = JsonNode(stableIdentifier(skillID));
	node["level"] = JsonNode(1);
	node["market_id"] = JsonNode(market->getObjInstanceID().getNum());
	if(visitor)
		node["hero_id"] = JsonNode(visitor->id.getNum());
	node["modeId"] = JsonNode(static_cast<int32_t>(EMarketMode::RESOURCE_SKILL));
	node["mode"] = JsonNode(marketModeName(EMarketMode::RESOURCE_SKILL));
	node["goldCost"] = JsonNode(goldCost);
	node["affordable"] = JsonNode(affordable);
	node["alreadyKnown"] = JsonNode(alreadyKnown);
	node["canLearnAny"] = JsonNode(canLearnAny);
	node["canLearn"] = JsonNode(canLearn);
	node["buyable"] = JsonNode(visitor && !alreadyKnown && canLearnAny && canLearn && affordable);
	if(visitor && heroManager)
		node["nullkillerSkillScore"].Float() = heroManager->evaluateSecSkill(skillID, visitor);
	setScriptActionType(node["planAction"], "market_trade");
	node["planAction"]["market_id"] = JsonNode(market->getObjInstanceID().getNum());
	node["planAction"]["mode_id"] = JsonNode(static_cast<int32_t>(EMarketMode::RESOURCE_SKILL));
	if(visitor)
		node["planAction"]["hero_id"] = JsonNode(visitor->id.getNum());
	node["planAction"]["skill_id"] = JsonNode(skillID.getNum());
	return node;
}

JsonNode jsonMarketSkillOptions(
	const IMarket * market,
	const CGHeroInstance * visitor,
	const ResourceSet & resources,
	const IGameSettings & settings,
	const NK2AI::HeroManager * heroManager = nullptr)
{
	JsonNode node;
	node.Vector();
	if(!market || !visitor || !market->allowsTrade(EMarketMode::RESOURCE_SKILL))
		return node;

	for(const TradeItemBuy & item : market->availableItemsIds(EMarketMode::RESOURCE_SKILL))
	{
		const SecondarySkill skillID = item.as<SecondarySkill>();
		if(skillID.hasValue())
			node.Vector().push_back(jsonMarketSkillOption(market, visitor, skillID, resources, settings, heroManager));
	}

	return node;
}

JsonNode jsonVisibleTile(
	const int3 & position,
	const TerrainTile & tile,
	const std::shared_ptr<CCallback> & callback,
	PlayerColor player)
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
	node["visitableObjectIds"].Vector();
	node["blockingObjectIds"].Vector();

	auto appendVisibleObjectID = [&](JsonNode & target, ObjectInstanceID objectID)
	{
		if(objectID == ObjectInstanceID())
			return;
		const CGObjectInstance * object = callback ? callback->getObj(objectID, false) : nullptr;
		if(!object || !callback->isVisibleFor(object, player))
			return;
		target.Vector().push_back(JsonNode(objectID.getNum()));
	};

	for(ObjectInstanceID objectID : tile.visitableObjects)
		appendVisibleObjectID(node["visitableObjectIds"], objectID);
	for(ObjectInstanceID objectID : tile.blockingObjects)
		appendVisibleObjectID(node["blockingObjectIds"], objectID);

	node["visitableObjectCount"] = JsonNode(static_cast<int32_t>(node["visitableObjectIds"].Vector().size()));
	node["blockingObjectCount"] = JsonNode(static_cast<int32_t>(node["blockingObjectIds"].Vector().size()));
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
		stack["creatureIdentifier"] = JsonNode(stableIdentifier(slot.second->getCreatureID()));
		stack["count"] = JsonNode(slot.second->getCount());
		stack["name"] = JsonNode(jsonText(slot.second->getName()));
		node.Vector().push_back(stack);
	}
	return node;
}

JsonNode jsonBattleCasualties(const std::map<CreatureID, si32> & casualties)
{
	JsonNode node;
	node.Vector();
	for(const auto & [creatureID, count] : casualties)
	{
		JsonNode casualty;
		casualty["creature_id"] = JsonNode(creatureID.getNum());
		casualty["creatureIdentifier"] = JsonNode(stableIdentifier(creatureID));
		casualty["count"] = JsonNode(count);
		node.Vector().push_back(casualty);
	}
	return node;
}

JsonNode jsonSecondarySkill(SecondarySkill skillID, ui8 level)
{
	JsonNode node;
	node["skill_id"] = JsonNode(skillID.getNum());
	node["skillIdentifier"] = JsonNode(stableIdentifier(skillID));
	node["level"] = JsonNode(static_cast<int32_t>(level));
	if(level < NSecondarySkill::levels.size())
		node["levelName"] = JsonNode(NSecondarySkill::levels[level]);
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

JsonNode jsonArtifactSlot(const CGHeroInstance * hero, ArtifactPosition position, const ArtSlotInfo & slotInfo, bool backpack)
{
	JsonNode node;
	node["holder_id"] = JsonNode(hero->id.getNum());
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
			node["nullkillerArtifactScore"] = JsonNode(static_cast<int64_t>(NK2AI::getArtifactScoreForHero(hero, artifact)));
			node["nullkillerPotentialArtifactScore"] = JsonNode(static_cast<int64_t>(NK2AI::getPotentialArtifactScore(artifactType)));
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
	setScriptActionType(data["ignoreAction"], "ignore_script_query");
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
			setScriptActionType(option["planAction"], "assemble_artifacts");
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
			node["worn"].Vector().push_back(jsonArtifactSlot(hero, position, slotInfo, false));
	}

	for(size_t index = 0; index < hero->artifactsInBackpack.size(); ++index)
	{
		const ArtSlotInfo & slotInfo = hero->artifactsInBackpack[index];
		if(slotInfo.getArt())
			node["backpack"].Vector().push_back(jsonArtifactSlot(hero, ArtifactPosition::BACKPACK_START + static_cast<int>(index), slotInfo, true));
	}

	return node;
}

bool heroCanUseMarketAltar(const CGHeroInstance * hero, const CGObjectInstance * marketObject)
{
	if(!hero || !marketObject)
		return false;

	if(const auto * town = dynamic_cast<const CGTownInstance *>(marketObject))
	{
		if(hero->getVisitedTown() == town)
			return true;
	}

	return marketObject->visitablePos().isValid() && hero->visitablePos() == marketObject->visitablePos();
}

JsonNode jsonArtifactAltarStageAction(ObjectInstanceID marketID, ObjectInstanceID heroID, ArtifactPosition slot)
{
	JsonNode action;
	setScriptActionType(action, "swap_artifacts");
	action["src"]["holder_id"] = JsonNode(heroID.getNum());
	action["src"]["slot"] = JsonNode(slot.getNum());
	action["dst"]["holder_id"] = JsonNode(marketID.getNum());
	action["dst"]["slot"] = JsonNode(ArtifactPosition(ArtifactPosition::ALTAR).getNum());
	return action;
}

JsonNode jsonArtifactAltarBulkStageAction(ObjectInstanceID marketID, ObjectInstanceID heroID, bool equipped, bool backpack)
{
	JsonNode action;
	setScriptActionType(action, "bulk_move_artifacts");
	action["src_id"] = JsonNode(heroID.getNum());
	action["dst_id"] = JsonNode(marketID.getNum());
	action["src_hero_id"] = JsonNode(heroID.getNum());
	action["swap"] = JsonNode(false);
	action["equipped"] = JsonNode(equipped);
	action["backpack"] = JsonNode(backpack);
	return action;
}

JsonNode jsonArtifactAltarSacrificeAction(ObjectInstanceID marketID, ObjectInstanceID heroID, ArtifactInstanceID artifactInstanceID)
{
	JsonNode action;
	setScriptActionType(action, "market_trade");
	action["market_id"] = JsonNode(marketID.getNum());
	action["mode_id"] = JsonNode(static_cast<int32_t>(EMarketMode::ARTIFACT_EXP));
	action["hero_id"] = JsonNode(heroID.getNum());
	action["artifact_instance_id"] = JsonNode(artifactInstanceID.getNum());
	return action;
}

JsonNode jsonArtifactAltarBulkSacrificeAction(ObjectInstanceID marketID, ObjectInstanceID heroID, const std::vector<ArtifactInstanceID> & artifactInstanceIDs)
{
	JsonNode action;
	setScriptActionType(action, "market_trade");
	action["market_id"] = JsonNode(marketID.getNum());
	action["mode_id"] = JsonNode(static_cast<int32_t>(EMarketMode::ARTIFACT_EXP));
	action["hero_id"] = JsonNode(heroID.getNum());
	action["artifact_instance_ids"].Vector();
	for(const ArtifactInstanceID & artifactInstanceID : artifactInstanceIDs)
		action["artifact_instance_ids"].Vector().push_back(JsonNode(artifactInstanceID.getNum()));
	return action;
}

JsonNode jsonCreatureAltarSacrificeAction(ObjectInstanceID marketID, ObjectInstanceID heroID, SlotID slot, int32_t amount)
{
	JsonNode action;
	setScriptActionType(action, "market_trade");
	action["market_id"] = JsonNode(marketID.getNum());
	action["mode_id"] = JsonNode(static_cast<int32_t>(EMarketMode::CREATURE_EXP));
	action["hero_id"] = JsonNode(heroID.getNum());
	action["slot"] = JsonNode(slot.getNum());
	action["amount"] = JsonNode(amount);
	return action;
}

JsonNode jsonCreatureAltarBulkSacrificeAction(ObjectInstanceID marketID, ObjectInstanceID heroID, const std::vector<SlotID> & slots, const std::vector<int32_t> & amounts)
{
	JsonNode action;
	setScriptActionType(action, "market_trade");
	action["market_id"] = JsonNode(marketID.getNum());
	action["mode_id"] = JsonNode(static_cast<int32_t>(EMarketMode::CREATURE_EXP));
	action["hero_id"] = JsonNode(heroID.getNum());
	action["slots"].Vector();
	action["amounts"].Vector();
	for(const SlotID & slot : slots)
		action["slots"].Vector().push_back(JsonNode(slot.getNum()));
	for(const int32_t amount : amounts)
		action["amounts"].Vector().push_back(JsonNode(amount));
	return action;
}

JsonNode jsonArtifactAltarCandidate(
	const IMarket * market,
	const CArtifactSet * altarStorage,
	const CGHeroInstance * hero,
	ArtifactPosition position,
	const ArtSlotInfo & slotInfo,
	bool backpack,
	CArtifactFittingSet & fittingSet)
{
	JsonNode node = jsonArtifactSlot(hero, position, slotInfo, backpack);
	const CArtifactInstance * artifact = slotInfo.getArt();
	if(!artifact || !artifact->getType())
		return node;

	int bidQty = 0;
	int rawExperience = 0;
	const bool hasOffer = market->getOffer(artifact->getTypeId(), 0, bidQty, rawExperience, EMarketMode::ARTIFACT_EXP);
	const bool tradable = artifact->getType()->isTradable() && hasOffer && rawExperience > 0;
	const bool removable = backpack || ArtifactUtils::isArtRemovable({ position, slotInfo });
	const ArtifactPosition stageSlot = ArtifactUtils::getArtAnyPosition(&fittingSet, artifact->getTypeId());
	const bool fitsAltar = stageSlot != ArtifactPosition::PRE_FIRST && altarStorage && artifact->canBePutAt(altarStorage, ArtifactPosition::ALTAR);
	const bool canStage = tradable && removable && fitsAltar;

	node["tradable"] = JsonNode(tradable);
	node["removable"] = JsonNode(removable);
	node["fitsAltar"] = JsonNode(fitsAltar);
	node["canStage"] = JsonNode(canStage);
	node["rawExperience"] = JsonNode(rawExperience);
	node["heroExperience"] = JsonNode(hero->calculateXp(rawExperience));

	if(canStage)
	{
		node["stageAction"] = jsonArtifactAltarStageAction(market->getObjInstanceID(), hero->id, position);
		node["sacrificeAction"] = jsonArtifactAltarSacrificeAction(market->getObjInstanceID(), hero->id, artifact->getId());
		node["planActions"].Vector();
		node["planActions"].Vector().push_back(node["stageAction"]);
		node["planActions"].Vector().push_back(node["sacrificeAction"]);
		fittingSet.putArtifact(stageSlot, artifact);
	}

	return node;
}

JsonNode jsonStagedAltarArtifact(const IMarket * market, const CGHeroInstance * hero, ArtifactPosition position, const ArtSlotInfo & slotInfo)
{
	JsonNode node;
	node["holder_id"] = JsonNode(market->getObjInstanceID().getNum());
	node["slot"] = JsonNode(position.getNum());
	node["slotInfo"] = jsonArtifactPosition(position);

	const CArtifactInstance * artifact = slotInfo.getArt();
	if(!artifact || !artifact->getType())
		return node;

	node["artifactInstanceId"] = JsonNode(artifact->getId().getNum());
	node["artifactTypeId"] = JsonNode(artifact->getTypeId().getNum());
	node["artifactIdentifier"] = JsonNode(artifact->getType()->getJsonKey());
	node["artifactName"] = JsonNode(jsonText(artifact->getType()->getNameTranslated()));
	const bool tradable = artifact->getType()->isTradable();
	node["tradable"] = JsonNode(tradable);
	int bidQty = 0;
	int rawExperience = 0;
	if(tradable && market->getOffer(artifact->getTypeId(), 0, bidQty, rawExperience, EMarketMode::ARTIFACT_EXP))
	{
		node["rawExperience"] = JsonNode(rawExperience);
		node["heroExperience"] = JsonNode(hero->calculateXp(rawExperience));
		if(rawExperience > 0)
			node["sacrificeAction"] = jsonArtifactAltarSacrificeAction(market->getObjInstanceID(), hero->id, artifact->getId());
	}
	return node;
}

JsonNode jsonMarketAltarOptions(const IMarket * market, const CGHeroInstance * hero)
{
	JsonNode node;
	node["artifactSacrificeOptions"].Vector();
	node["stagedArtifactOptions"].Vector();
	node["creatureSacrificeOptions"].Vector();
	node["available"] = JsonNode(false);

	if(!market || !hero)
		return node;

	node["market_id"] = JsonNode(market->getObjInstanceID().getNum());
	node["hero_id"] = JsonNode(hero->id.getNum());
	node["available"] = JsonNode(true);

	if(market->allowsTrade(EMarketMode::ARTIFACT_EXP))
	{
		node["artifactAltarAvailable"] = JsonNode(true);
		const bool artifactAltarBlockedByAlignment = hero->getAlignment() == EAlignment::EVIL;
		node["artifactAltarBlockedByAlignment"] = JsonNode(artifactAltarBlockedByAlignment);
		CArtifactSet * altarStorage = market->getArtifactsStorage();
		if(altarStorage && !artifactAltarBlockedByAlignment)
		{
			CArtifactFittingSet fittingSet(*altarStorage);
			std::vector<ArtifactInstanceID> stagedFromHero;
			int rawBulkExperience = 0;

			for(const auto & [position, slotInfo] : hero->artifactsWorn)
			{
				if(!slotInfo.getArt())
					continue;
				JsonNode option = jsonArtifactAltarCandidate(market, altarStorage, hero, position, slotInfo, false, fittingSet);
				if(option["canStage"].Bool())
				{
					stagedFromHero.push_back(slotInfo.getArt()->getId());
					rawBulkExperience += static_cast<int32_t>(option["rawExperience"].Integer());
				}
				node["artifactSacrificeOptions"].Vector().push_back(option);
			}

			for(size_t index = 0; index < hero->artifactsInBackpack.size(); ++index)
			{
				const ArtSlotInfo & slotInfo = hero->artifactsInBackpack[index];
				if(!slotInfo.getArt())
					continue;
				const ArtifactPosition position = ArtifactPosition::BACKPACK_START + static_cast<int>(index);
				JsonNode option = jsonArtifactAltarCandidate(market, altarStorage, hero, position, slotInfo, true, fittingSet);
				if(option["canStage"].Bool())
				{
					stagedFromHero.push_back(slotInfo.getArt()->getId());
					rawBulkExperience += static_cast<int32_t>(option["rawExperience"].Integer());
				}
				node["artifactSacrificeOptions"].Vector().push_back(option);
			}

			if(!stagedFromHero.empty())
			{
				node["sacrificeAllArtifactPlanActions"].Vector();
				node["sacrificeAllArtifactPlanActions"].Vector().push_back(jsonArtifactAltarBulkStageAction(market->getObjInstanceID(), hero->id, true, true));
				node["sacrificeAllArtifactPlanActions"].Vector().push_back(jsonArtifactAltarBulkSacrificeAction(market->getObjInstanceID(), hero->id, stagedFromHero));
				node["sacrificeAllArtifactRawExperience"] = JsonNode(rawBulkExperience);
				node["sacrificeAllArtifactHeroExperience"] = JsonNode(hero->calculateXp(rawBulkExperience));
			}

			for(size_t index = 0; index < altarStorage->artifactsInBackpack.size(); ++index)
			{
				const ArtSlotInfo & slotInfo = altarStorage->artifactsInBackpack[index];
				if(slotInfo.getArt())
					node["stagedArtifactOptions"].Vector().push_back(jsonStagedAltarArtifact(market, hero, ArtifactPosition::BACKPACK_START + static_cast<int>(index), slotInfo));
			}
		}
	}
	else
	{
		node["artifactAltarAvailable"] = JsonNode(false);
	}

	if(market->allowsTrade(EMarketMode::CREATURE_EXP))
	{
		node["creatureAltarAvailable"] = JsonNode(true);
		const bool creatureAltarBlockedByAlignment = hero->getAlignment() == EAlignment::GOOD;
		node["creatureAltarBlockedByAlignment"] = JsonNode(creatureAltarBlockedByAlignment);
		if(creatureAltarBlockedByAlignment)
			return node;

		std::vector<SlotID> bulkSlots;
		std::vector<int32_t> bulkAmounts;
		int rawBulkExperience = 0;

		for(const auto & [slot, stack] : hero->Slots())
		{
			if(!stack)
				continue;

			const int32_t count = stack->getCount();
			int bidQty = 0;
			int rawPerUnitExperience = 0;
			market->getOffer(stack->getCreatureID(), 0, bidQty, rawPerUnitExperience, EMarketMode::CREATURE_EXP);

			JsonNode option;
			const bool wouldSacrificeLastStack = hero->stacksCount() == 1 && hero->needsLastStack();
			const int32_t maxSacrificeAmount = wouldSacrificeLastStack ? std::max<int32_t>(0, count - 1) : count;
			option["slot"] = JsonNode(slot.getNum());
			option["creatureId"] = JsonNode(stack->getCreatureID().getNum());
			option["creatureIdentifier"] = JsonNode(stableIdentifier(stack->getCreatureID()));
			option["count"] = JsonNode(count);
			option["rawExperiencePerUnit"] = JsonNode(rawPerUnitExperience);
			option["heroExperiencePerUnit"] = JsonNode(hero->calculateXp(rawPerUnitExperience));
			option["maxSacrificeAmount"] = JsonNode(maxSacrificeAmount);
			if(maxSacrificeAmount > 0)
				option["planAction"] = jsonCreatureAltarSacrificeAction(market->getObjInstanceID(), hero->id, slot, maxSacrificeAmount);
			node["creatureSacrificeOptions"].Vector().push_back(option);

			if(count > 0)
			{
				bulkSlots.push_back(slot);
				bulkAmounts.push_back(count);
				rawBulkExperience += rawPerUnitExperience * count;
			}
		}

		if(hero->needsLastStack() && !bulkAmounts.empty())
		{
			bulkAmounts.back() -= 1;
			const SlotID preservedSlot = bulkSlots.back();
			const CStackInstance * preservedStack = hero->getStackPtr(preservedSlot);
			if(preservedStack)
			{
				int bidQty = 0;
				int rawPerUnitExperience = 0;
				market->getOffer(preservedStack->getCreatureID(), 0, bidQty, rawPerUnitExperience, EMarketMode::CREATURE_EXP);
				rawBulkExperience -= rawPerUnitExperience;
			}
		}

		for(size_t index = 0; index < bulkAmounts.size();)
		{
			if(bulkAmounts[index] <= 0)
			{
				bulkAmounts.erase(bulkAmounts.begin() + index);
				bulkSlots.erase(bulkSlots.begin() + index);
			}
			else
			{
				++index;
			}
		}

		if(!bulkSlots.empty())
		{
			node["sacrificeAllCreatureAction"] = jsonCreatureAltarBulkSacrificeAction(market->getObjInstanceID(), hero->id, bulkSlots, bulkAmounts);
			node["sacrificeAllCreatureRawExperience"] = JsonNode(rawBulkExperience);
			node["sacrificeAllCreatureHeroExperience"] = JsonNode(hero->calculateXp(rawBulkExperience));
		}
	}
	else
	{
		node["creatureAltarAvailable"] = JsonNode(false);
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

std::string playerBlockedReasonName(int reason)
{
	switch(reason)
	{
	case PlayerBlocked::UPCOMING_BATTLE:
		return "upcoming_battle";
	case PlayerBlocked::ONGOING_MOVEMENT:
		return "ongoing_movement";
	default:
		return "unknown";
	}
}

std::string colorSchemeName(ColorScheme scheme)
{
	switch(scheme)
	{
	case ColorScheme::NONE:
		return "none";
	case ColorScheme::KEEP:
		return "keep";
	case ColorScheme::GRAYSCALE:
		return "grayscale";
	case ColorScheme::H2_SCHEME:
		return "h2_scheme";
	}
	return "unknown";
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

std::string battleResultName(EBattleResult result)
{
	switch(result)
	{
	case EBattleResult::NORMAL:
		return "normal";
	case EBattleResult::ESCAPE:
		return "escape";
	case EBattleResult::SURRENDER:
		return "surrender";
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

std::string infoWindowModeName(EInfoWindowMode mode)
{
	switch(mode)
	{
	case EInfoWindowMode::AUTO:
		return "auto";
	case EInfoWindowMode::MODAL:
		return "modal";
	case EInfoWindowMode::INFO:
		return "info";
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
	case NK2AI::ScriptTaskSearchMode::STARTUP:
		return "startup";
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

JsonNode jsonNullkillerSubroutineOption(
	NK2AI::ScriptTaskSearchMode mode,
	int32_t maxSteps,
	int32_t maxCandidates,
	int32_t maxAttempts)
{
	JsonNode option;
	option["modeId"] = JsonNode(static_cast<int32_t>(mode));
	option["mode"] = JsonNode(nullkillerTaskSearchModeName(mode));
	option["bounded"] = JsonNode(true);
	option["delegatesRestOfDay"] = JsonNode(false);
	option["maxSteps"] = JsonNode(maxSteps);
	option["maxCandidates"] = JsonNode(maxCandidates);
	option["maxAttempts"] = JsonNode(maxAttempts);

	setScriptActionType(option["tasksAction"], "nullkiller_tasks");
	option["tasksAction"]["mode"] = option["modeId"];
	option["tasksAction"]["max_candidates"] = option["maxCandidates"];

	setScriptActionType(option["stepAction"], "nullkiller_step");
	option["stepAction"]["mode"] = option["modeId"];
	option["stepAction"]["max_candidates"] = option["maxCandidates"];
	option["stepAction"]["max_attempts"] = option["maxAttempts"];

	setScriptActionType(option["passAction"], "nullkiller_pass");
	option["passAction"]["mode"] = option["modeId"];
	option["passAction"]["max_steps"] = option["maxSteps"];
	option["passAction"]["max_candidates"] = option["maxCandidates"];
	option["passAction"]["max_attempts"] = option["maxAttempts"];

	option["planAction"] = option["stepAction"];
	return option;
}

JsonNode jsonNullkillerPriorityPassOption()
{
	JsonNode option;
	option["helperKindId"] = JsonNode(1);
	option["helperKind"] = JsonNode("priority_pass");
	option["bounded"] = JsonNode(true);
	option["delegatesRestOfDay"] = JsonNode(false);
	setScriptActionType(option["planAction"], "nullkiller_priority_pass");
	option["planAction"]["pass_index"] = JsonNode(1);
	return option;
}

JsonNode jsonNullkillerResourceTradeOption()
{
	JsonNode option;
	option["helperKindId"] = JsonNode(2);
	option["helperKind"] = JsonNode("resource_trade");
	option["bounded"] = JsonNode(true);
	option["delegatesRestOfDay"] = JsonNode(false);
	setScriptActionType(option["planAction"], "nullkiller_trade");
	return option;
}

JsonNode jsonNullkillerOptimizeArtifactsOption()
{
	JsonNode option;
	option["helperKindId"] = JsonNode(3);
	option["helperKind"] = JsonNode("artifact_optimization");
	option["bounded"] = JsonNode(true);
	option["delegatesRestOfDay"] = JsonNode(false);
	setScriptActionType(option["planAction"], "nullkiller_optimize_artifacts");
	return option;
}

JsonNode jsonNullkillerTurnSliceOption()
{
	JsonNode option;
	option["helperKindId"] = JsonNode(6);
	option["helperKind"] = JsonNode("turn_slice");
	option["bounded"] = JsonNode(true);
	option["delegatesRestOfDay"] = JsonNode(false);
	option["maxPasses"] = JsonNode(1);
	option["maxCandidates"] = JsonNode(16);
	option["maxAttempts"] = JsonNode(16);
	setScriptActionType(option["planAction"], "nullkiller_turn_slice");
	option["planAction"]["max_passes"] = option["maxPasses"];
	option["planAction"]["max_candidates"] = option["maxCandidates"];
	option["planAction"]["max_attempts"] = option["maxAttempts"];
	return option;
}

JsonNode jsonNullkillerResetOption()
{
	JsonNode option;
	option["helperKindId"] = JsonNode(11);
	option["helperKind"] = JsonNode("planner_reset");
	option["bounded"] = JsonNode(true);
	option["delegatesRestOfDay"] = JsonNode(false);
	setScriptActionType(option["planAction"], "nullkiller_reset");
	return option;
}

JsonNode jsonAnswerQueryAction(QueryID queryID, int32_t answer)
{
	JsonNode action;
	setScriptActionType(action, "answer_query");
	action["query_id"] = JsonNode(queryID.getNum());
	action["answer"] = JsonNode(answer);
	return action;
}

JsonNode jsonCancelQueryAction(QueryID queryID)
{
	JsonNode action;
	setScriptActionType(action, "cancel_query");
	action["query_id"] = JsonNode(queryID.getNum());
	return action;
}

JsonNode jsonNullkillerAnswerQueryAction(QueryID queryID)
{
	JsonNode action;
	setScriptActionType(action, "nullkiller_answer_query");
	action["query_id"] = JsonNode(queryID.getNum());
	return action;
}

void attachAnswerPlanActions(JsonNode & query, QueryID queryID, const std::string & field)
{
	if(!hasField(query, field) || !query[field].isVector())
		return;

	for(JsonNode & option : query[field].Vector())
	{
		if(hasField(option, "answer"))
			option["planAction"] = jsonAnswerQueryAction(queryID, readInteger(option, "answer"));
	}
}

void attachScriptQueryActions(JsonNode & query, QueryID queryID)
{
	if(hasField(query, "answerableByQueryReply") && !readBool(query, "answerableByQueryReply", true))
		return;

	query["answerableByQueryReply"] = JsonNode(true);
	query["answerAction"] = jsonAnswerQueryAction(queryID, 0);
	query["nullkillerAnswerAction"] = jsonNullkillerAnswerQueryAction(queryID);
	if(readString(query, "type") == "map_object_select")
		query["cancelAction"] = jsonCancelQueryAction(queryID);
	else if(readString(query, "type") == "blocking_dialog" && readBool(query, "cancel", false))
		query["cancelAction"] = jsonAnswerQueryAction(queryID, 0);

	for(const char * field : { "skill_options", "components", "exits", "objects" })
		attachAnswerPlanActions(query, queryID, field);
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

std::string nullkillerScanDepthName(NK2AI::ScanDepth scanDepth)
{
	switch(scanDepth)
	{
	case NK2AI::ScanDepth::MAIN_FULL:
		return "main_full";
	case NK2AI::ScanDepth::SMALL:
		return "small";
	case NK2AI::ScanDepth::ALL_FULL:
		return "all_full";
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

void memorizeScriptVisibleVisitableObjs(
	const std::unique_ptr<NK2AI::AIMemory> & memory,
	const std::unique_ptr<NK2AI::DangerHitMapAnalyzer> & dangerHitMap,
	PlayerColor playerID,
	const std::shared_ptr<CCallback> & cc)
{
	if(!memory || !dangerHitMap || !cc)
		return;

	std::set<ObjectInstanceID> seenObjects;
	for(const int3 & position : visibleMapTiles(cc, playerID))
	{
		for(const CGObjectInstance * object : cc->getVisitableObjs(position, false))
		{
			if(!object || !seenObjects.insert(object->id).second)
				continue;
			if(object->tempOwner != playerID && !cc->isVisibleFor(object, playerID))
				continue;

			NK2AI::AIGateway::memorizeVisitableObj(object, memory, dangerHitMap, playerID, cc);
		}
	}
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

bool isScriptVisibleTile(const std::shared_ptr<CCallback> & cc, PlayerColor player, const int3 & position)
{
	return position.isValid() && (!cc || (cc->isInTheMap(position) && cc->isVisibleFor(position, player)));
}

bool isScriptVisibleObject(const std::shared_ptr<CCallback> & cc, PlayerColor player, const CGObjectInstance * object)
{
	return object && (!cc || object->tempOwner == player || cc->isVisibleFor(object, player));
}

const CGObjectInstance * getScriptVisibleObject(const std::shared_ptr<CCallback> & cc, PlayerColor player, ObjectInstanceID objectID)
{
	if(!cc)
		return nullptr;

	const CGObjectInstance * object = cc->getObj(objectID, false);
	if(!isScriptVisibleObject(cc, player, object))
		return nullptr;
	return object;
}

ScriptSpecialActionKind specialActionKind(const NK2AI::SpecialAction & action)
{
	if(dynamic_cast<const NK2AI::CompositeAction *>(&action))
		return ScriptSpecialActionKind::COMPOSITE;
	if(dynamic_cast<const NK2AI::AIPathfinding::DimensionDoorAction *>(&action))
		return ScriptSpecialActionKind::DIMENSION_DOOR;
	if(dynamic_cast<const NK2AI::AIPathfinding::TownPortalAction *>(&action))
		return ScriptSpecialActionKind::TOWN_PORTAL;
	if(dynamic_cast<const NK2AI::AIPathfinding::SummonBoatAction *>(&action))
		return ScriptSpecialActionKind::SUMMON_BOAT;
	if(dynamic_cast<const NK2AI::AIPathfinding::BuildBoatAction *>(&action))
		return ScriptSpecialActionKind::BUILD_BOAT;
	if(dynamic_cast<const NK2AI::AIPathfinding::WhirlpoolAction *>(&action))
		return ScriptSpecialActionKind::WHIRLPOOL;
	if(dynamic_cast<const NK2AI::AIPathfinding::QuestAction *>(&action))
		return ScriptSpecialActionKind::QUEST;
	if(dynamic_cast<const NK2AI::AIPathfinding::AdventureCastAction *>(&action))
		return ScriptSpecialActionKind::ADVENTURE_CAST;
	return ScriptSpecialActionKind::UNKNOWN;
}

JsonNode jsonSpecialAction(
	const NK2AI::SpecialAction & action,
	const std::shared_ptr<CCallback> & cc,
	PlayerColor player,
	const int3 & destination,
	int depth)
{
	const ScriptSpecialActionKind kind = specialActionKind(action);
	JsonNode node;
	node["kindId"] = JsonNode(static_cast<int32_t>(kind));
	node["kind"] = JsonNode(scriptSpecialActionKindName(kind));
	node["parts"].Vector();

	if(isScriptVisibleTile(cc, player, destination))
		node["destination"] = jsonPosition(destination);

	if(const CGObjectInstance * target = action.targetObject())
	{
		if(!cc || cc->isVisibleFor(target, player))
		{
			node["targetObjectId"] = JsonNode(target->id.getNum());
			node["targetObjectTypeId"] = JsonNode(target->ID.getNum());
			node["targetPosition"] = jsonPosition(target->visitablePos());
		}
	}

	if(const auto * dimensionDoor = dynamic_cast<const NK2AI::AIPathfinding::DimensionDoorAction *>(&action))
	{
		node["spell_id"] = JsonNode(dimensionDoor->getUsedSpell().getNum());
		node["manaCost"] = JsonNode(dimensionDoor->getManaCost());
		node["movementPointsRequired"] = JsonNode(dimensionDoor->getMovementPointsRequired());
		node["movementPointsTaken"] = JsonNode(dimensionDoor->getMovementPointsTaken());
		node["plannedSourceTurn"] = JsonNode(dimensionDoor->getPlannedSourceTurn());
		node["plannedSourceMoveRemains"] = JsonNode(dimensionDoor->getPlannedSourceMoveRemains());
		node["plannedSourceMoveLimit"] = JsonNode(dimensionDoor->getPlannedSourceMoveLimit());
		node["plannedDimensionDoorCasts"] = JsonNode(dimensionDoor->getPlannedDimensionDoorCasts());
		if(isScriptVisibleTile(cc, player, dimensionDoor->getDestination()))
		{
			node["guardedLandingDanger"] = JsonNode(static_cast<int64_t>(dimensionDoor->getGuardedLandingDanger()));
			node["guardedLandingArmyLoss"] = JsonNode(static_cast<int64_t>(dimensionDoor->getGuardedLandingArmyLoss()));
			node["spellDestination"] = jsonPosition(dimensionDoor->getDestination());
		}
	}
	else if(const auto * townPortal = dynamic_cast<const NK2AI::AIPathfinding::TownPortalAction *>(&action))
	{
		node["spell_id"] = JsonNode(townPortal->getUsedSpell().getNum());
		if(const CGTownInstance * town = townPortal->getTargetTown())
		{
			if(!cc || cc->isVisibleFor(town, player) || town->tempOwner == player)
			{
				node["targetTownId"] = JsonNode(town->id.getNum());
				node["targetTownPosition"] = jsonPosition(town->visitablePos());
			}
		}
	}
	else if(const auto * summonBoat = dynamic_cast<const NK2AI::AIPathfinding::SummonBoatAction *>(&action))
	{
		node["spell_id"] = JsonNode(summonBoat->getUsedSpell().getNum());
	}
	else if(const auto * buildBoat = dynamic_cast<const NK2AI::AIPathfinding::BuildBoatAction *>(&action))
	{
		if(const IShipyard * shipyard = buildBoat->getShipyard())
		{
			if(const CGObjectInstance * shipyardObject = buildBoat->targetObject())
			{
				if(!cc || cc->isVisibleFor(shipyardObject, player))
				{
					ResourceSet cost;
					shipyard->getBoatCost(cost);
					node["shipyard_id"] = JsonNode(shipyardObject->id.getNum());
					node["shipyardPosition"] = jsonPosition(shipyardObject->visitablePos());
					node["shipyardStatusId"] = JsonNode(static_cast<int32_t>(shipyard->shipyardStatus()));
					node["boatTypeId"] = JsonNode(shipyard->getBoatType().getNum());
					node["boatLayerId"] = JsonNode(static_cast<int32_t>(shipyard->getBoatLayer()));
					if(isScriptVisibleTile(cc, player, shipyard->bestLocation()))
						node["bestBoatLocation"] = jsonPosition(shipyard->bestLocation());
					node["boatCost"] = jsonResources(cost);
				}
			}
		}
	}
	else if(const auto * adventureCast = dynamic_cast<const NK2AI::AIPathfinding::AdventureCastAction *>(&action))
	{
		node["spell_id"] = JsonNode(adventureCast->getSpellToCast().getNum());
		node["manaCost"] = JsonNode(adventureCast->getManaCost());
		node["dayFlagsId"] = JsonNode(static_cast<int32_t>(adventureCast->getFlagsToAdd()));
	}
	else if(const auto * questAction = dynamic_cast<const NK2AI::AIPathfinding::QuestAction *>(&action))
	{
		const QuestInfo & questInfo = questAction->getQuestInfo();
		if(cc)
		{
			if(const CGObjectInstance * questObject = questInfo.getObject(cc.get()))
			{
				if(cc->isVisibleFor(questObject, player))
				{
					node["questObjectId"] = JsonNode(questObject->id.getNum());
					node["questObjectTypeId"] = JsonNode(questObject->ID.getNum());
					node["questPosition"] = jsonPosition(questObject->visitablePos());
				}
			}
		}
	}

	const auto parts = action.getParts();
	node["partCount"] = JsonNode(static_cast<int32_t>(parts.size()));
	node["partsTruncated"] = JsonNode(false);
	if(depth > 0 && !parts.empty())
	{
		constexpr size_t maxParts = 8;
		if(parts.size() > maxParts)
			node["partsTruncated"] = JsonNode(true);
		size_t count = 0;
		for(const auto & part : parts)
		{
			if(!part)
				continue;
			if(count++ >= maxParts)
				break;
			node["parts"].Vector().push_back(jsonSpecialAction(*part, cc, player, destination, depth - 1));
		}
	}

	return node;
}

JsonNode jsonNullkillerPath(
	const NK2AI::AIPath & path,
	const std::shared_ptr<CCallback> & cc,
	PlayerColor player,
	size_t maxNodes = 16)
{
	JsonNode node;
	const bool allNodesVisible = std::ranges::all_of(path.nodes, [&](const NK2AI::AIPathNodeInfo & pathNode)
	{
		return isScriptVisibleTile(cc, player, pathNode.coord);
	});

	node["nodeLimit"] = JsonNode(static_cast<int32_t>(maxNodes));
	node["nodeCount"] = JsonNode(allNodesVisible ? static_cast<int32_t>(path.nodes.size()) : 0);
	node["nodesTruncated"] = JsonNode(allNodesVisible && path.nodes.size() > maxNodes);
	node["nodes"].Vector();

	if(!allNodesVisible)
		return node;

	node["exchangeCount"] = JsonNode(static_cast<int32_t>(path.exchangeCount));
	node["chainMask"] = JsonNode(static_cast<int64_t>(path.chainMask));
	node["targetObjectDanger"] = JsonNode(static_cast<int64_t>(path.targetObjectDanger));
	node["armyLoss"] = JsonNode(static_cast<int64_t>(path.armyLoss));
	node["targetObjectArmyLoss"] = JsonNode(static_cast<int64_t>(path.targetObjectArmyLoss));
	if(path.targetHero && isScriptVisibleObject(cc, player, path.targetHero))
	{
		node["hero_id"] = JsonNode(path.targetHero->id.getNum());
		node["heroStrength"] = JsonNode(static_cast<int64_t>(path.getHeroStrength()));
	}
	if(!path.nodes.empty())
	{
		if(isScriptVisibleTile(cc, player, path.firstTileToGet()))
			node["firstTile"] = jsonPosition(path.firstTileToGet());
		if(isScriptVisibleTile(cc, player, path.targetTile()))
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
		if(pathNode.targetHero && isScriptVisibleObject(cc, player, pathNode.targetHero))
			pathNodeJson["hero_id"] = JsonNode(pathNode.targetHero->id.getNum());
		if(pathNode.specialAction)
		{
			pathNodeJson["specialAction"] = jsonSpecialAction(*pathNode.specialAction, cc, player, pathNode.coord, 1);
			if(const CGObjectInstance * target = pathNode.specialAction->targetObject())
			{
				if(!cc || cc->isVisibleFor(target, player))
				{
					pathNodeJson["specialActionTargetObjectId"] = JsonNode(target->id.getNum());
					pathNodeJson["specialActionTargetPosition"] = jsonPosition(target->visitablePos());
				}
			}
		}
		node["nodes"].Vector().push_back(pathNodeJson);
	}
	return node;
}

JsonNode jsonNullkillerGoalSummary(const NK2AI::Goals::AbstractGoal & goal, const std::shared_ptr<CCallback> & cc, PlayerColor playerID, int depth);

JsonNode jsonNullkillerBuildingInfo(const NK2AI::BuildingInfo & building)
{
	JsonNode node;
	node["building_id"] = JsonNode(building.id.getNum());
	node["buildCost"] = jsonResources(building.buildCost);
	node["buildCostWithPrerequisites"] = jsonResources(building.buildCostWithPrerequisites);
	node["dailyIncome"] = jsonResources(building.dailyIncome);
	node["armyCost"] = jsonResources(building.armyCost);
	node["creatureGrowth"] = JsonNode(building.creatureGrowth);
	node["creatureLevel"] = JsonNode(static_cast<int32_t>(building.creatureLevel));
	node["creature_id"] = JsonNode(building.creatureID.getNum());
	node["base_creature_id"] = JsonNode(building.baseCreatureID.getNum());
	node["prerequisitesCount"] = JsonNode(static_cast<int32_t>(building.prerequisitesCount));
	node["armyStrength"] = JsonNode(static_cast<int64_t>(building.armyStrength));
	node["isBuilt"] = JsonNode(building.isBuilt);
	node["isBuildable"] = JsonNode(building.isBuildable);
	node["isMissingResources"] = JsonNode(building.isMissingResources);
	return node;
}

JsonNode jsonNullkillerTownDevelopmentInfo(const NK2AI::TownDevelopmentInfo & developmentInfo)
{
	JsonNode node;
	if(developmentInfo.town)
		node["town_id"] = JsonNode(developmentInfo.town->id.getNum());
	node["townDevelopmentCost"] = jsonResources(developmentInfo.townDevelopmentCost);
	node["requiredResources"] = jsonResources(developmentInfo.requiredResources);
	node["armyCost"] = jsonResources(developmentInfo.armyCost);
	node["armyStrength"] = JsonNode(static_cast<int64_t>(developmentInfo.armyStrength));
	node["toBuild"].Vector();
	for(const NK2AI::BuildingInfo & building : developmentInfo.toBuild)
		node["toBuild"].Vector().push_back(jsonNullkillerBuildingInfo(building));
	node["built"].Vector();
	for(const NK2AI::BuildingInfo & building : developmentInfo.built)
		node["built"].Vector().push_back(jsonNullkillerBuildingInfo(building));
	return node;
}

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
		const JsonNode buildingInfo = jsonNullkillerBuildingInfo(building);
		for(const auto & [field, value] : buildingInfo.Struct())
			details[field] = value;
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
			if(isScriptVisibleTile(cc, playerID, shipyard->bestLocation()))
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
		details["path"] = jsonNullkillerPath(executeChain->getPath(), cc, playerID);

	if(const auto * heroExchange = dynamic_cast<const NK2AI::Goals::HeroExchange *>(&goal))
		details["exchangePath"] = jsonNullkillerPath(heroExchange->exchangePath, cc, playerID);

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
		details["pathToCenter"] = jsonNullkillerPath(unlockCluster->getPathToCenter(), cc, playerID);
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
	if(isScriptVisibleObject(cc, playerID, goal.hero))
		node["hero_id"] = JsonNode(goal.hero->id.getNum());
	if(isScriptVisibleObject(cc, playerID, goal.town))
		node["town_id"] = JsonNode(goal.town->id.getNum());
	if(goal.objid >= 0)
	{
		if(const CGObjectInstance * object = getScriptVisibleObject(cc, playerID, ObjectInstanceID(goal.objid)))
			node["object_id"] = JsonNode(object->id.getNum());
	}
	if(goal.bid >= 0)
		node["building_id"] = JsonNode(goal.bid);
	if(goal.aid >= 0)
		node["artifact_id"] = JsonNode(goal.aid);
	if(goal.resID >= 0)
		node["resource_id"] = JsonNode(goal.resID);
	if(isScriptVisibleTile(cc, playerID, goal.tile))
		node["tile"] = jsonPosition(goal.tile);
	if(goal.goldCost > 0)
		node["goldCost"] = JsonNode(static_cast<int64_t>(goal.goldCost));
	node["buildingCost"] = jsonResources(goal.buildingCost);

	if(const auto * task = dynamic_cast<const NK2AI::Goals::ITask *>(&goal))
	{
		node["priority"].Float() = task->priority;
		node["affectedObjectIds"].Vector();
		for(const ObjectInstanceID objectID : task->getAffectedObjects())
		{
			if(const CGObjectInstance * object = getScriptVisibleObject(cc, playerID, objectID))
				node["affectedObjectIds"].Vector().push_back(JsonNode(object->id.getNum()));
		}
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

JsonNode jsonQuestInfo(const QuestInfo & questInfo, CCallback * callback, PlayerColor player)
{
	JsonNode node;
	node["object_id"] = JsonNode(questInfo.obj.getNum());
	if(!callback)
		return node;

	node["position"] = jsonPosition(questInfo.getPosition(callback));
	if(const CGObjectInstance * object = questInfo.getObject(callback))
	{
		if(callback->isVisibleFor(object, player))
			node["object"] = jsonMapObject(object, player, nullptr);

		if(const auto * questObject = dynamic_cast<const IQuestObject *>(object))
			node["quest"] = jsonQuestObject(questObject, player, nullptr);
	}
	return node;
}

JsonNode jsonHero(const CGHeroInstance * hero, bool includeOwnedDetails = true)
{
	JsonNode node;
	node["id"] = JsonNode(hero->id.getNum());
	node["name"] = JsonNode(jsonText(hero->getNameTranslated()));
	node["position"] = jsonPosition(hero->visitablePos());
	node["owner"] = JsonNode(jsonPlayerColor(hero->tempOwner));
	node["heroTypeId"] = JsonNode(hero->getHeroTypeID().getNum());
	node["heroTypeIdentifier"] = JsonNode(stableIdentifier(hero->getHeroTypeID()));
	node["heroClassId"] = JsonNode(hero->getHeroClassID().getNum());
	node["heroClassIdentifier"] = JsonNode(stableIdentifier(hero->getHeroClassID()));
	node["factionId"] = JsonNode(hero->getFactionID().getNum());
	node["factionIdentifier"] = JsonNode(stableIdentifier(hero->getFactionID()));
	node["level"] = JsonNode(static_cast<int32_t>(hero->level));
	node["mana"] = JsonNode(hero->mana);
	node["manaLimit"] = JsonNode(hero->manaLimit());
	node["movement"] = JsonNode(hero->movementPointsRemaining());
	node["movementLimit"] = JsonNode(hero->movementPointsLimit());
	node["formationId"] = JsonNode(static_cast<int32_t>(hero->formation));
	node["formation"] = JsonNode(armyFormationName(hero->formation));
	node["tacticsEnabled"] = JsonNode(hero->tacticFormationEnabled);
	node["inBoat"] = JsonNode(hero->inBoat());
	node["primarySkills"]["attack"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::ATTACK));
	node["primarySkills"]["defense"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::DEFENSE));
	node["primarySkills"]["spellPower"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER));
	node["primarySkills"]["knowledge"] = JsonNode(hero->getPrimSkillLevel(PrimarySkill::KNOWLEDGE));
	node["secondarySkills"].Vector();
	if(includeOwnedDetails)
	{
		node["manaRegain"] = JsonNode(hero->manaRegain());
		node["manaNextTurn"] = JsonNode(hero->getManaNewTurn());
		node["garrisoned"] = JsonNode(hero->isGarrisoned());
		node["visitedTownId"] = jsonObjectId(hero->getVisitedTown());
		node["patrol"]["active"] = JsonNode(hero->patrol.patrolling);
		node["patrol"]["radius"] = JsonNode(hero->patrol.patrolRadius == CGHeroInstance::NO_PATROLLING ? -1 : static_cast<int32_t>(hero->patrol.patrolRadius));
		node["patrol"]["initialPosition"] = jsonPosition(hero->patrol.initialPos);
		for(const auto & [skillID, level] : hero->secSkills)
		{
			if(skillID == SecondarySkill::NONE)
				continue;
			node["secondarySkills"].Vector().push_back(jsonSecondarySkill(skillID, level));
		}
		node["experience"] = JsonNode(static_cast<int64_t>(hero->exp));
		if(hero->level < LIBRARY->heroh->maxSupportedLevel())
		{
			const TExpType nextLevelExperience = LIBRARY->heroh->reqExp(hero->level + 1);
			node["nextLevelExperience"] = JsonNode(static_cast<int64_t>(nextLevelExperience));
			node["experienceToNextLevel"] = JsonNode(static_cast<int64_t>(nextLevelExperience > hero->exp ? nextLevelExperience - hero->exp : 0));
		}
		node["gainsLevel"] = JsonNode(hero->gainsLevel());
		node["canLearnSkill"] = JsonNode(hero->canLearnSkill());
	}
	node["heroStrength"] = JsonNode(static_cast<int64_t>(hero->getHeroStrength()));
	node["armyStrength"] = JsonNode(static_cast<int64_t>(hero->getArmyStrength()));
	node["totalStrength"] = JsonNode(static_cast<int64_t>(hero->getTotalStrength()));
	node["army"] = jsonArmy(*hero);
	node["hasSpellbook"] = JsonNode(hero->hasSpellbook());
	node["spells"].Vector();
	if(includeOwnedDetails)
	{
		node["artifacts"] = jsonArtifacts(hero);
		for(const SpellID & spellID : hero->getSpellsInSpellbook())
		{
			if(!spellID.hasValue())
				continue;
			if(const CSpell * spell = spellID.toSpell())
				node["spells"].Vector().push_back(jsonKnownSpell(hero, spell));
		}
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
	node["affectedObjects"].Vector();

	if(candidate.task)
	{
		if(const CGHeroInstance * hero = candidate.task->getHero())
		{
			if(isScriptVisibleObject(cc, playerID, hero))
			{
				node["hero_id"] = JsonNode(hero->id.getNum());
				node["hero"] = jsonHero(hero, hero->tempOwner == playerID);
			}
		}

		for(const ObjectInstanceID objectID : candidate.task->getAffectedObjects())
		{
			if(const CGObjectInstance * object = getScriptVisibleObject(cc, playerID, objectID))
			{
				node["affectedObjectIds"].Vector().push_back(JsonNode(object->id.getNum()));
				node["affectedObjects"].Vector().push_back(jsonMapObject(object, playerID, candidate.task->getHero()));
			}
		}

		if(const auto * goal = dynamic_cast<const NK2AI::Goals::AbstractGoal *>(candidate.task.get()))
		{
			node["goal"] = jsonNullkillerGoalSummary(*goal, cc, playerID, 2);
			node["goalTypeId"] = JsonNode(static_cast<int32_t>(goal->goalType));
			node["goalType"] = JsonNode(nullkillerGoalName(goal->goalType));
			if(goal->town)
			{
				if(isScriptVisibleObject(cc, playerID, goal->town))
				{
					node["town_id"] = JsonNode(goal->town->id.getNum());
					node["townObject"] = jsonMapObject(goal->town, playerID, candidate.task->getHero());
				}
			}
			if(goal->objid >= 0)
			{
				if(const CGObjectInstance * object = getScriptVisibleObject(cc, playerID, ObjectInstanceID(goal->objid)))
				{
					node["object_id"] = JsonNode(object->id.getNum());
					node["object"] = jsonMapObject(object, playerID, candidate.task->getHero());
				}
			}
			if(goal->bid >= 0)
				node["building_id"] = JsonNode(goal->bid);
			if(goal->aid >= 0)
				node["artifact_id"] = JsonNode(goal->aid);
			if(goal->resID >= 0)
				node["resource_id"] = JsonNode(goal->resID);
			if(isScriptVisibleTile(cc, playerID, goal->tile))
				node["tile"] = jsonPosition(goal->tile);
			if(goal->goldCost > 0)
				node["goldCost"] = JsonNode(static_cast<int64_t>(goal->goldCost));
			node["buildingCost"] = jsonResources(goal->buildingCost);

		}
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
	setScriptActionType(node["planAction"], "recruit");
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

JsonNode jsonAvailableHeroOption(const CGObjectInstance * source, const CGHeroInstance * hero, PlayerColor player)
{
	JsonNode node;
	node["source_id"] = JsonNode(source->id.getNum());
	node["sourceObject"] = jsonMapObject(source, player, nullptr);
	if(const auto * town = dynamic_cast<const CGTownInstance *>(source))
	{
		node["town_id"] = JsonNode(town->id.getNum());
		node["town"] = JsonNode(jsonText(town->getNameTranslated()));
		node["sourceKindId"] = JsonNode(1);
		node["sourceKind"] = JsonNode("town_tavern");
	}
	else
	{
		node["tavern_id"] = node["source_id"];
		node["sourceKindId"] = JsonNode(2);
		node["sourceKind"] = JsonNode("adventure_tavern");
	}
	node["hero_type_id"] = JsonNode(hero->getHeroTypeID().getNum());
	node["hero"] = JsonNode(jsonText(hero->getNameTranslated()));
	node["heroStrength"] = JsonNode(static_cast<int64_t>(hero->getHeroStrength()));
	node["armyStrength"] = JsonNode(static_cast<int64_t>(hero->getArmyStrength()));
	node["totalStrength"] = JsonNode(static_cast<int64_t>(hero->getTotalStrength()));
	node["cost"]["gold"] = JsonNode(GameConstants::HERO_GOLD_COST);
	node["army"] = jsonArmy(*hero);
	setScriptActionType(node["planAction"], "hire_hero");
	node["planAction"]["source_id"] = node["source_id"];
	if(node.Struct().contains("town_id"))
		node["planAction"]["town_id"] = node["town_id"];
	if(node.Struct().contains("tavern_id"))
		node["planAction"]["tavern_id"] = node["tavern_id"];
	node["planAction"]["hero_type_id"] = node["hero_type_id"];
	return node;
}

JsonNode jsonOwnedArmySnapshot(const CArmedInstance * army)
{
	JsonNode node;
	if(!army)
		return node;

	node["id"] = JsonNode(army->id.getNum());
	node["armyStrength"] = JsonNode(static_cast<int64_t>(army->getArmyStrength()));
	node["army"] = jsonArmy(*army);
	return node;
}

JsonNode jsonBonusChange(const Bonus & bonus, bool gain)
{
	JsonNode node;
	node["gain"] = JsonNode(gain);
	node["typeId"] = JsonNode(static_cast<int32_t>(bonus.type));
	node["sourceId"] = JsonNode(static_cast<int32_t>(bonus.source));
	node["value"] = JsonNode(bonus.val);
	node["valueTypeId"] = JsonNode(static_cast<int32_t>(bonus.valType));
	node["duration"] = JsonNode(static_cast<int32_t>(bonus.duration));
	node["turnsRemain"] = JsonNode(static_cast<int32_t>(bonus.turnsRemain));
	node["subtypeId"] = JsonNode(bonus.subtype.getNum());
	node["sourceObjectId"] = JsonNode(bonus.sid.getNum());
	node["effectRangeId"] = JsonNode(static_cast<int32_t>(bonus.effectRange));
	node["targetSourceTypeId"] = JsonNode(static_cast<int32_t>(bonus.targetSourceType));
	if(!bonus.stacking.empty())
		node["stacking"] = JsonNode(bonus.stacking);
	if(!bonus.description.empty())
		node["description"] = JsonNode(jsonText(bonus.description.toString()));
	return node;
}

JsonNode jsonDwellingPools(const CGDwelling * dwelling)
{
	JsonNode node;
	node.Vector();
	if(!dwelling)
		return node;

	for(int32_t level = 0; level < static_cast<int32_t>(dwelling->creatures.size()); ++level)
	{
		JsonNode pool;
		pool["level"] = JsonNode(level);
		pool["available"] = JsonNode(static_cast<int32_t>(dwelling->creatures[level].first));
		pool["creature_ids"].Vector();
		pool["creatureIdentifiers"].Vector();
		for(const CreatureID & creatureID : dwelling->creatures[level].second)
		{
			pool["creature_ids"].Vector().push_back(JsonNode(creatureID.getNum()));
			pool["creatureIdentifiers"].Vector().push_back(JsonNode(stableIdentifier(creatureID)));
		}
		node.Vector().push_back(pool);
	}
	return node;
}

JsonNode jsonVisibleArmyHolder(const CGObjectInstance * object, PlayerColor player, const std::shared_ptr<CCallback> & callback)
{
	JsonNode node;
	if(!object || !callback || !callback->isVisibleFor(object, player))
		return node;

	node["object"] = jsonMapObject(object, player, nullptr);
	if(object->tempOwner == player)
	{
		if(const auto * army = dynamic_cast<const CArmedInstance *>(object))
			node["ownedArmy"] = jsonOwnedArmySnapshot(army);
	}
	return node;
}

JsonNode jsonRecruitOptions(const CGDwelling * dwelling, const CArmedInstance * destination, int32_t selectedLevel, const ResourceSet & resources)
{
	JsonNode node;
	node.Vector();
	if(!dwelling || !destination)
		return node;

	auto appendLevel = [&](int32_t level)
	{
		JsonNode option = jsonRecruitOption(dwelling, destination, level, resources);
		if(option.isStruct())
			node.Vector().push_back(option);
	};

	if(selectedLevel >= 0)
		appendLevel(selectedLevel);
	else
	{
		for(int32_t level = 0; level < static_cast<int32_t>(dwelling->creatures.size()); ++level)
			appendLevel(level);
	}
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
	setScriptActionType(node["planAction"], "transfer_army");
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
	setScriptActionType(node["planAction"], "upgrade_creature");
	node["planAction"]["army_id"] = node["army_id"];
	node["planAction"]["slot"] = node["slot"];
	node["planAction"]["creature_id"] = node["upgrade_creature_id"];
	return node;
}

JsonNode jsonTownBuilding(const CGTownInstance * town, BuildingID buildingID)
{
	JsonNode node;
	node["building_id"] = JsonNode(buildingID.getNum());
	node["buildingIdentifier"] = JsonNode(buildingIdentifier(town, buildingID));
	if(const auto buildingIter = town->getTown()->buildings.find(buildingID); buildingIter != town->getTown()->buildings.end() && buildingIter->second)
	{
		const CBuilding * building = buildingIter->second.get();
		node["buildingKindId"] = JsonNode(static_cast<int32_t>(scriptBuildingKind(buildingID)));
		node["buildingKind"] = JsonNode(scriptBuildingKindName(scriptBuildingKind(buildingID)));
		node["buildingLevel"] = JsonNode(scriptBuildingLevel(buildingID));
		node["buildingUpgrade"] = JsonNode(scriptBuildingUpgrade(buildingID));
		node["building"] = JsonNode(jsonText(building->getNameTranslated()));
	}
	return node;
}

JsonNode jsonTownMageGuildSpells(const CGTownInstance * town)
{
	JsonNode node;
	node.Vector();
	const int32_t mageGuildLevel = town->mageGuildLevel();
	for(size_t levelIndex = 0; levelIndex < town->spells.size(); ++levelIndex)
	{
		const int32_t spellLevel = static_cast<int32_t>(levelIndex + 1);
		if(spellLevel > mageGuildLevel)
			continue;

		JsonNode levelNode;
		levelNode["level"] = JsonNode(spellLevel);
		levelNode["spells"].Vector();
		const int32_t visibleSpellCount = std::clamp<int32_t>(
			town->spellsAtLevel(static_cast<int32_t>(levelIndex), false),
			0,
			static_cast<int32_t>(town->spells[levelIndex].size()));
		levelNode["visibleSpellCount"] = JsonNode(visibleSpellCount);
		levelNode["researchQueueCount"] = JsonNode(static_cast<int32_t>(town->spells[levelIndex].size()) - visibleSpellCount);
		for(size_t spellIndex = 0; spellIndex < static_cast<size_t>(visibleSpellCount); ++spellIndex)
		{
			const SpellID & spellID = town->spells[levelIndex][spellIndex];
			if(!spellID.hasValue())
				continue;
			JsonNode spellNode;
			spellNode["spell_id"] = JsonNode(spellID.getNum());
			spellNode["spellIdentifier"] = JsonNode(stableIdentifier(spellID));
			if(const CSpell * spell = spellID.toSpell())
				spellNode["spellName"] = JsonNode(jsonText(spell->getNameTranslated()));
			levelNode["spells"].Vector().push_back(spellNode);
		}
		node.Vector().push_back(levelNode);
	}
	return node;
}

std::optional<JsonNode> jsonSpellResearchOption(
	const CGTownInstance * town,
	size_t levelIndex,
	const SpellID & spellID,
	const ResourceSet & resources,
	const IGameSettings & settings)
{
	if(!town || levelIndex >= town->spells.size() || !spellID.hasValue())
		return std::nullopt;
	if(!settings.getBoolean(EGameSettings::TOWNS_SPELL_RESEARCH) || !town->spellResearchAllowed)
		return std::nullopt;

	const int32_t visibleSpellCount = std::clamp<int32_t>(
		town->spellsAtLevel(static_cast<int32_t>(levelIndex), false),
		0,
		static_cast<int32_t>(town->spells[levelIndex].size()));
	const auto spellPosition = vstd::find_pos(town->spells[levelIndex], spellID);
	if(spellPosition < 0 || spellPosition >= visibleSpellCount)
		return std::nullopt;
	if(visibleSpellCount >= static_cast<int32_t>(town->spells[levelIndex].size()))
		return std::nullopt;

	const auto perDay = settings.getValue(EGameSettings::TOWNS_SPELL_RESEARCH_PER_DAY).Vector();
	const auto baseCosts = settings.getValue(EGameSettings::TOWNS_SPELL_RESEARCH_COST).Vector();
	const auto researchMultipliers = settings.getValue(EGameSettings::TOWNS_SPELL_RESEARCH_COST_MULTIPLIER_PER_RESEARCH).Vector();
	const auto rerollMultipliers = settings.getValue(EGameSettings::TOWNS_SPELL_RESEARCH_COST_MULTIPLIER_PER_REROLL).Vector();
	if(levelIndex >= perDay.size() || levelIndex >= baseCosts.size() || levelIndex >= researchMultipliers.size() || levelIndex >= rerollMultipliers.size())
		return std::nullopt;

	if(town->spellResearchCounterDay >= perDay[levelIndex].Float())
		return std::nullopt;
	if(levelIndex >= town->spellResearchPendingRerollsCounters.size())
		return std::nullopt;

	ResourceSet costBase;
	costBase.resolveFromJson(baseCosts[levelIndex]);
	const double pastResearchesMultiplier = std::pow(researchMultipliers[levelIndex].Float(), town->spellResearchAcceptedCounter);
	const double pastRerollsMultiplier = std::pow(rerollMultipliers[levelIndex].Float(), town->spellResearchPendingRerollsCounters[levelIndex]);
	const ResourceSet cost = costBase.multipliedBy(pastResearchesMultiplier * pastRerollsMultiplier);
	const SpellID replacementSpellID = town->spells[levelIndex][visibleSpellCount];

	JsonNode node;
	node["town_id"] = JsonNode(town->id.getNum());
	node["spell_level"] = JsonNode(static_cast<int32_t>(levelIndex + 1));
	node["spell_id"] = JsonNode(spellID.getNum());
	node["spellIdentifier"] = JsonNode(stableIdentifier(spellID));
	if(const CSpell * spell = spellID.toSpell())
		node["spellName"] = JsonNode(jsonText(spell->getNameTranslated()));
	node["replacement_spell_id"] = JsonNode(replacementSpellID.getNum());
	node["replacementSpellIdentifier"] = JsonNode(stableIdentifier(replacementSpellID));
	if(const CSpell * spell = replacementSpellID.toSpell())
		node["replacementSpellName"] = JsonNode(jsonText(spell->getNameTranslated()));
	node["cost"] = jsonResources(cost);
	node["affordable"] = JsonNode(resources.canAfford(cost));
	node["acceptedResearches"] = JsonNode(town->spellResearchAcceptedCounter);
	node["pendingRerolls"] = JsonNode(town->spellResearchPendingRerollsCounters[levelIndex]);
	node["researchesToday"] = JsonNode(town->spellResearchCounterDay);
	setScriptActionType(node["planAction"], "spell_research");
	node["planAction"]["town_id"] = node["town_id"];
	node["planAction"]["spell_id"] = node["spell_id"];
	node["planAction"]["accept"] = JsonNode(true);
	setScriptActionType(node["rerollAction"], "spell_research");
	node["rerollAction"]["town_id"] = node["town_id"];
	node["rerollAction"]["spell_id"] = node["spell_id"];
	node["rerollAction"]["accept"] = JsonNode(false);
	return node;
}

JsonNode jsonTownDwellingLevels(const CGTownInstance * town)
{
	JsonNode node;
	node.Vector();
	for(int32_t level = 0; level < static_cast<int32_t>(town->creatures.size()); ++level)
	{
		JsonNode levelNode;
		levelNode["level"] = JsonNode(level);
		levelNode["available"] = JsonNode(static_cast<int32_t>(town->creatures[level].first));
		levelNode["growth"] = JsonNode(town->creatureGrowth(level));
		levelNode["creatures"].Vector();
		for(const CreatureID & creatureID : town->creatures[level].second)
		{
			JsonNode creatureNode;
			creatureNode["creature_id"] = JsonNode(creatureID.getNum());
			creatureNode["creatureIdentifier"] = JsonNode(stableIdentifier(creatureID));
			levelNode["creatures"].Vector().push_back(creatureNode);
		}
		node.Vector().push_back(levelNode);
	}
	return node;
}

JsonNode jsonTownHordeLevels(const CGTownInstance * town)
{
	JsonNode node;
	node.Vector();
	for(int32_t hordeIndex = 0; hordeIndex < 2; ++hordeIndex)
	{
		JsonNode hordeNode;
		hordeNode["slot"] = JsonNode(hordeIndex);
		hordeNode["creatureLevel"] = JsonNode(town->getHordeLevel(hordeIndex));
		const BuildingID baseBuilding = hordeIndex == 0 ? BuildingID::HORDE_1 : BuildingID::HORDE_2;
		const BuildingID upgradeBuilding = hordeIndex == 0 ? BuildingID::HORDE_1_UPGR : BuildingID::HORDE_2_UPGR;
		hordeNode["building_id"] = JsonNode(baseBuilding.getNum());
		hordeNode["buildingIdentifier"] = JsonNode(buildingIdentifier(town, baseBuilding));
		hordeNode["built"] = JsonNode(town->hasBuilt(baseBuilding));
		hordeNode["upgrade_building_id"] = JsonNode(upgradeBuilding.getNum());
		hordeNode["upgradeBuildingIdentifier"] = JsonNode(buildingIdentifier(town, upgradeBuilding));
		hordeNode["upgradeBuilt"] = JsonNode(town->hasBuilt(upgradeBuilding));
		node.Vector().push_back(hordeNode);
	}
	return node;
}

JsonNode jsonTown(const CGTownInstance * town, const ResourceSet & resources, bool includeOwnedDetails = true)
{
	JsonNode node;
	node["id"] = JsonNode(town->id.getNum());
	node["name"] = JsonNode(jsonText(town->getNameTranslated()));
	node["position"] = jsonPosition(town->visitablePos());
	node["owner"] = JsonNode(jsonPlayerColor(town->tempOwner));
	node["factionId"] = JsonNode(town->getFactionID().getNum());
	node["factionIdentifier"] = JsonNode(stableIdentifier(town->getFactionID()));
	node["fortLevel"] = JsonNode(static_cast<int32_t>(town->fortLevel()));
	node["hallLevel"] = JsonNode(town->hallLevel());
	node["mageGuildLevel"] = JsonNode(town->mageGuildLevel());
	node["townLevel"] = JsonNode(town->getTownLevel());
	node["builtThisTurn"] = JsonNode(town->built);
	node["destroyedThisTurn"] = JsonNode(town->destroyed);
	node["hasFort"] = JsonNode(town->hasFort());
	node["hasCapitol"] = JsonNode(town->hasCapitol());
	node["armedGarrison"] = JsonNode(town->armedGarrison());
	node["hasResourceMarketplace"] = JsonNode(town->hasBuiltResourceMarketplace());
	node["visitingHeroId"] = jsonObjectId(town->getVisitingHero());
	node["garrisonHeroId"] = jsonObjectId(town->getGarrisonHero());
	node["armyStrength"] = JsonNode(static_cast<int64_t>(town->getUpperArmy()->getArmyStrength(town->fortLevel())));
	node["army"] = jsonArmy(*town);
	node["buildings"].Vector();
	node["buildingDetails"].Vector();
	for(const BuildingID & building : town->getBuildings())
	{
		node["buildings"].Vector().push_back(JsonNode(building.getNum()));
		node["buildingDetails"].Vector().push_back(jsonTownBuilding(town, building));
	}
	node["hordeLevels"] = jsonTownHordeLevels(town);

	const ArtifactID warMachine = town->getWarMachineInBuilding(BuildingID::BLACKSMITH);
	node["blacksmithWarMachineArtifactId"] = JsonNode(warMachine.getNum());
	if(warMachine != ArtifactID::NONE)
		node["blacksmithWarMachineArtifactIdentifier"] = JsonNode(stableIdentifier(warMachine));

	node["recruitOptions"].Vector();
	if(includeOwnedDetails)
	{
		node["mageGuildSpells"] = jsonTownMageGuildSpells(town);
		node["dwellingLevels"] = jsonTownDwellingLevels(town);
		node["forbiddenBuildings"].Vector();
		for(const BuildingID & building : town->forbiddenBuildings)
			node["forbiddenBuildings"].Vector().push_back(jsonTownBuilding(town, building));
	}
	if(includeOwnedDetails && !town->getVisitingHero())
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
	setScriptActionType(node["planAction"], "build");
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
	setScriptActionType(node["planAction"], "dig");
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
	setScriptActionType(node["planAction"], "build_boat");
	node["planAction"]["shipyard_id"] = node["shipyard_id"];
	return node;
}

JsonNode jsonAdventureSpellOption(
	const std::shared_ptr<CCallback> & cc,
	PlayerColor player,
	const CGHeroInstance * hero,
	const CSpell * spell,
	const std::optional<int3> & target,
	int32_t targetKindID,
	const std::string & targetKind)
{
	const auto & mechanics = spell->getAdventureMechanics();
	const ScriptAdventureSpellKind spellKind = scriptAdventureSpellKind(spell, hero);
	const int castsLimit = mechanics.getCastsLimit(hero, cc->getMapSize());
	const int castsAlreadyPerformed = mechanics.getCastsAlreadyPerformed(hero);
	const int castsByMana = hero->getSpellCost(spell) > 0 ? hero->mana / hero->getSpellCost(spell) : 0;
	const int castsRemainingByLimit = castsLimit > 0 ? std::max(0, castsLimit - castsAlreadyPerformed) : castsByMana;
	const int castsRemaining = std::min(castsByMana, castsRemainingByLimit);

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
	node["castsLimit"] = JsonNode(castsLimit);
	node["castsAlreadyPerformed"] = JsonNode(castsAlreadyPerformed);
	node["castsRemaining"] = JsonNode(castsRemaining);
	node["spellKindId"] = JsonNode(static_cast<int32_t>(spellKind));
	node["spellKind"] = JsonNode(scriptAdventureSpellKindName(spellKind));
	node["givesWaterWalking"] = JsonNode(mechanics.givesBonus(hero, BonusType::WATER_WALKING));
	node["givesFlyingMovement"] = JsonNode(mechanics.givesBonus(hero, BonusType::FLYING_MOVEMENT));
	node["targetKindId"] = JsonNode(targetKindID);
	node["targetKind"] = JsonNode(targetKind);
	node["hasTarget"] = JsonNode(static_cast<bool>(target));
	setScriptActionType(node["planAction"], "cast_spell");
	node["planAction"]["hero_id"] = node["hero_id"];
	node["planAction"]["spell_id"] = node["spell_id"];

	if(const auto * ranged = dynamic_cast<const AdventureSpellRangedEffect *>(mechanics.getEffectAs<IAdventureSpellEffect>(hero)))
	{
		node["range"]["x"] = JsonNode(ranged->getRangeX());
		node["range"]["y"] = JsonNode(ranged->getRangeY());
		node["range"]["ignoresFogOfWar"] = JsonNode(ranged->ignoresFogOfWar());
	}
	if(const auto * dimensionDoor = mechanics.getEffectAs<DimensionDoorEffect>(hero))
	{
		node["dimensionDoor"]["movementPointsRequired"] = JsonNode(dimensionDoor->getMovementPointsRequired());
		node["dimensionDoor"]["movementPointsTaken"] = JsonNode(dimensionDoor->getMovementPointsTaken());
		node["dimensionDoor"]["waterLandFailureTakesPoints"] = JsonNode(dimensionDoor->doesWaterLandFailureTakePoints());
		node["dimensionDoor"]["exposesFogOfWar"] = JsonNode(dimensionDoor->doesExposeFogOfWar());
	}
	if(const auto * townPortal = mechanics.getEffectAs<TownPortalEffect>(hero))
	{
		node["townPortal"]["movementPointsRequired"] = JsonNode(townPortal->getMovementPointsRequired());
		node["townPortal"]["townSelectionAllowed"] = JsonNode(townPortal->townSelectionAllowed());
		node["townPortal"]["opensSelectionDialog"] = JsonNode(townPortal->townSelectionAllowed() && !target);
	}
	if(const auto * summonBoat = mechanics.getEffectAs<SummonBoatEffect>(hero))
	{
		node["summonBoat"]["successChance"] = JsonNode(summonBoat->getSuccessChance(hero));
		node["summonBoat"]["canCreateNewBoat"] = JsonNode(summonBoat->canCreateNewBoat());
		node["summonBoat"]["bestLocation"] = jsonPosition(hero->bestLocation());
	}

	node["nativePlanner"]["recommended"] = JsonNode(adventureSpellUsesNativeRouting(spellKind));
	node["nativePlanner"]["modeId"] = JsonNode(static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::ADVENTURE));
	node["nativePlanner"]["mode"] = JsonNode(nullkillerTaskSearchModeName(NK2AI::ScriptTaskSearchMode::ADVENTURE));
	if(adventureSpellUsesNativeRouting(spellKind))
	{
		JsonNode option = jsonNullkillerSubroutineOption(NK2AI::ScriptTaskSearchMode::ADVENTURE, 4, 16, 16);
		node["nativePlanner"]["tasksAction"] = option["tasksAction"];
		node["nativePlanner"]["stepAction"] = option["stepAction"];
		node["nativePlanner"]["passAction"] = option["passAction"];
	}

	if(target)
	{
		node["target"] = jsonPosition(*target);
		node["targetVisible"] = JsonNode(cc->isVisibleFor(*target, player));
		node["targetGuarded"] = JsonNode(cc->isVisibleFor(*target, player) && cc->isTileGuardedUnchecked(*target));
		if(const CGObjectInstance * targetObject = cc->getTopObj(*target))
		{
			if(cc->isVisibleFor(targetObject, player))
			{
				node["targetObjectId"] = JsonNode(targetObject->id.getNum());
				node["targetObjectTypeId"] = JsonNode(targetObject->ID.getNum());
				node["targetObjectKindId"] = JsonNode(static_cast<int32_t>(scriptObjectKind(targetObject->ID)));
				node["targetObjectKind"] = JsonNode(scriptObjectKindName(scriptObjectKind(targetObject->ID)));
				node["targetObject"] = jsonMapObject(targetObject, player, hero);
			}
		}
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
	setScriptActionType(node["planAction"], "buy_artifact");
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
		std::lock_guard turnLock(scriptedTurnMutex);
		if(!status.haveTurn())
		{
			logAi->debug("ScriptedAdventureAI skipped stale turn task because the turn is already over.");
			return;
		}
		status.waitTillFree();
		if(!status.haveTurn())
		{
			logAi->debug("ScriptedAdventureAI skipped stale turn task because the turn is already over.");
			return;
		}
		makeScriptedTurn();
	});
}

void CScriptedAdventureAI::availableCreaturesChanged(const CGDwelling * dwelling)
{
	if(dwelling && cc && cc->isVisibleFor(dwelling, playerID))
	{
		JsonNode data;
		data["dwelling_id"] = JsonNode(dwelling->id.getNum());
		data["dwelling"] = jsonMapObject(dwelling, playerID, nullptr);
		if(dwelling->tempOwner == playerID)
			data["creaturePools"] = jsonDwellingPools(dwelling);
		appendScriptUpdate("available_creatures_changed", data, isOpponent(dwelling->tempOwner));
	}

	AIGateway::availableCreaturesChanged(dwelling);
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
	data["typeId"] = JsonNode(static_cast<int32_t>(scriptQueryKind(type)));
	data["type"] = JsonNode(type);
	attachScriptQueryActions(data, queryID);

	std::lock_guard guard(scriptQueryMutex);
	scriptQueries[queryID] = data;
}

std::optional<JsonNode> CScriptedAdventureAI::getScriptQuery(QueryID queryID) const
{
	std::lock_guard guard(scriptQueryMutex);
	const auto iter = scriptQueries.find(queryID);
	if(iter == scriptQueries.end())
		return std::nullopt;
	return iter->second;
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

	std::vector<std::optional<float>> nullkillerSkillScores(skills.size());
	int nullkillerSelectedIndex = -1;
	if(hero && hero->tempOwner == playerID && nullkiller && nullkiller->heroManager)
	{
		std::unique_lock aiLock(nullkiller->aiStateMutex);
		nullkiller->heroManager->update();
		nullkillerSelectedIndex = nullkiller->heroManager->selectBestSkillIndex(NK2AI::HeroPtr(hero, cc.get()), skills);
		for(size_t index = 0; index < skills.size(); ++index)
			nullkillerSkillScores[index] = nullkiller->heroManager->evaluateSecSkill(skills[index], hero);
	}

	for(size_t index = 0; index < skills.size(); ++index)
	{
		JsonNode option;
		option["answer"] = JsonNode(static_cast<int32_t>(index));
		option["skill_id"] = JsonNode(skills[index].getNum());
		option["skillIdentifier"] = JsonNode(stableIdentifier(skills[index]));
		option["level"] = JsonNode(1);
		if(nullkillerSkillScores[index])
			option["nullkillerSkillScore"].Float() = *nullkillerSkillScores[index];
		option["nullkillerPreferred"] = JsonNode(nullkillerSelectedIndex == static_cast<int>(index));
		data["skill_options"].Vector().push_back(option);
	}
	if(nullkillerSelectedIndex >= 0 && nullkillerSelectedIndex < static_cast<int>(skills.size()))
	{
		data["nullkillerSelectedSkillIndex"] = JsonNode(nullkillerSelectedIndex);
		data["nullkillerSelectedAnswer"] = JsonNode(nullkillerSelectedIndex);
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
	const ObjectInstanceID nullkillerTarget = destinationTeleport;
	for(size_t index = 0; index < exits.size(); ++index)
	{
		JsonNode exit;
		exit["answer"] = JsonNode(static_cast<int32_t>(index));
		exit["object_id"] = JsonNode(exits[index].first.getNum());
		exit["position"] = jsonPosition(exits[index].second);
		exit["nullkillerPreferred"] = JsonNode(nullkillerTarget != ObjectInstanceID() && exits[index].first == nullkillerTarget);
		if(cc)
		{
			if(const CGObjectInstance * object = cc->getObj(exits[index].first, false))
			{
				if(cc->isVisibleFor(object, playerID))
					exit["object"] = jsonMapObject(object, playerID, hero);
			}
		}
		data["exits"].Vector().push_back(exit);
	}
	if(nullkillerTarget != ObjectInstanceID())
	{
		for(const JsonNode & exit : data["exits"].Vector())
		{
			if(readBool(exit, "nullkillerPreferred", false))
			{
				data["nullkillerSelectedObjectId"] = JsonNode(nullkillerTarget.getNum());
				data["nullkillerSelectedAnswer"] = exit["answer"];
				break;
			}
		}
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
	const ObjectInstanceID nullkillerTarget = selectedObject;
	for(const ObjectInstanceID & objectID : objects)
	{
		JsonNode option;
		option["answer"] = JsonNode(objectID.getNum());
		option["object_id"] = JsonNode(objectID.getNum());
		option["nullkillerPreferred"] = JsonNode(nullkillerTarget != ObjectInstanceID() && objectID == nullkillerTarget);
		if(cc)
		{
			if(const CGObjectInstance * object = cc->getObj(objectID, false))
			{
				if(cc->isVisibleFor(object, playerID))
					option["object"] = jsonMapObject(object, playerID, nullptr);
			}
		}
		data["objects"].Vector().push_back(option);
	}
	if(nullkillerTarget != ObjectInstanceID())
	{
		for(const JsonNode & object : data["objects"].Vector())
		{
			if(readBool(object, "nullkillerPreferred", false))
			{
				data["nullkillerSelectedObjectId"] = JsonNode(nullkillerTarget.getNum());
				data["nullkillerSelectedAnswer"] = object["answer"];
				break;
			}
		}
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
		data["town"] = jsonTown(town, cc->getResourceAmount(), town->tempOwner == playerID);
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
			data["hero"] = jsonHero(hero, hero->tempOwner == playerID);

		appendScriptUpdate("hero_moved", data, hero ? isOpponent(hero->tempOwner) : true);
	}
	AIGateway::heroMoved(details, verbose);
}

void CScriptedAdventureAI::centerView(int3 pos, int focusTime)
{
	if(cc && cc->isInTheMap(pos) && cc->isVisibleFor(pos, playerID))
	{
		JsonNode data;
		data["position"] = jsonPosition(pos);
		data["focusTime"] = JsonNode(focusTime);
		appendScriptUpdate("center_view", data, false);
	}

	AIGateway::centerView(pos, focusTime);
}

void CScriptedAdventureAI::heroInGarrisonChange(const CGTownInstance * town)
{
	if(town && cc && cc->isVisibleFor(town, playerID))
	{
		JsonNode data;
		data["town_id"] = JsonNode(town->id.getNum());
		data["town"] = jsonTown(town, cc->getResourceAmount(), town->tempOwner == playerID);
		appendScriptUpdate("hero_in_garrison_changed", data, isOpponent(town->tempOwner));
	}

	AIGateway::heroInGarrisonChange(town);
}

void CScriptedAdventureAI::tileHidden(const FowTilesType & pos)
{
	JsonNode data;
	data["count"] = JsonNode(static_cast<int32_t>(pos.size()));
	data["tiles"] = jsonPositions(pos, 32);
	appendScriptUpdate("tile_hidden", data, false);

	AIGateway::tileHidden(pos);
}

void CScriptedAdventureAI::artifactMoved(const ArtifactLocation & src, const ArtifactLocation & dst)
{
	const CGObjectInstance * srcHolder = cc ? cc->getObj(src.artHolder, false) : nullptr;
	const CGObjectInstance * dstHolder = cc ? cc->getObj(dst.artHolder, false) : nullptr;
	const bool srcVisibleOwned = srcHolder && srcHolder->tempOwner == playerID && cc->isVisibleFor(srcHolder, playerID);
	const bool dstVisibleOwned = dstHolder && dstHolder->tempOwner == playerID && cc->isVisibleFor(dstHolder, playerID);
	if(srcVisibleOwned || dstVisibleOwned)
	{
		JsonNode data;
		data["src"] = jsonArtifactLocation(src);
		data["dst"] = jsonArtifactLocation(dst);
		if(srcVisibleOwned)
			data["srcHolder"] = jsonVisibleArmyHolder(srcHolder, playerID, cc);
		if(dstVisibleOwned)
			data["dstHolder"] = jsonVisibleArmyHolder(dstHolder, playerID, cc);
		appendScriptUpdate("artifact_moved", data, false);
	}

	AIGateway::artifactMoved(src, dst);
}

void CScriptedAdventureAI::artifactPut(const ArtifactLocation & location)
{
	const CGObjectInstance * holder = cc ? cc->getObj(location.artHolder, false) : nullptr;
	if(holder && holder->tempOwner == playerID && cc->isVisibleFor(holder, playerID))
	{
		JsonNode data;
		data["location"] = jsonArtifactLocation(location);
		data["holder"] = jsonVisibleArmyHolder(holder, playerID, cc);
		appendScriptUpdate("artifact_put", data, false);
	}

	AIGateway::artifactPut(location);
}

void CScriptedAdventureAI::artifactRemoved(const ArtifactLocation & location)
{
	const CGObjectInstance * holder = cc ? cc->getObj(location.artHolder, false) : nullptr;
	if(holder && holder->tempOwner == playerID && cc->isVisibleFor(holder, playerID))
	{
		JsonNode data;
		data["location"] = jsonArtifactLocation(location);
		data["holder"] = jsonVisibleArmyHolder(holder, playerID, cc);
		appendScriptUpdate("artifact_removed", data, false);
	}

	AIGateway::artifactRemoved(location);
}

void CScriptedAdventureAI::bulkArtMovementStart(size_t totalNumOfArts, size_t possibleAssemblyNumOfArts)
{
	JsonNode data;
	data["totalArtifacts"] = JsonNode(static_cast<int32_t>(totalNumOfArts));
	data["possibleAssemblyArtifacts"] = JsonNode(static_cast<int32_t>(possibleAssemblyNumOfArts));
	appendScriptUpdate("bulk_artifact_movement_started", data, false);
}

void CScriptedAdventureAI::heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start)
{
	const bool visitorVisible = visitor && cc && cc->isVisibleFor(visitor, playerID);
	const bool objectVisible = visitedObj && cc && cc->isVisibleFor(visitedObj, playerID);
	if(visitorVisible || objectVisible)
	{
		JsonNode data;
		data["start"] = JsonNode(start);
		if(visitor)
			data["hero_id"] = JsonNode(visitor->id.getNum());
		if(visitorVisible)
			data["hero"] = jsonHero(visitor, visitor->tempOwner == playerID);
		if(visitedObj)
			data["object_id"] = JsonNode(visitedObj->id.getNum());
		if(objectVisible)
			data["object"] = jsonMapObject(visitedObj, playerID, visitorVisible ? visitor : nullptr);
		appendScriptUpdate("hero_visit", data, (visitor && isOpponent(visitor->tempOwner)) || (visitedObj && isOpponent(visitedObj->tempOwner)));
	}

	AIGateway::heroVisit(visitor, visitedObj, start);
}

void CScriptedAdventureAI::heroCreated(const CGHeroInstance * hero)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID))
	{
		JsonNode data;
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		appendScriptUpdate("hero_created", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroCreated(hero);
}

void CScriptedAdventureAI::heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town)
{
	if(hero && town && cc && (cc->isVisibleFor(hero, playerID) || cc->isVisibleFor(town, playerID)))
	{
		JsonNode data;
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		data["town"] = jsonMapObject(town, playerID, hero);
		appendScriptUpdate("hero_visits_town", data, isOpponent(hero->tempOwner) || isOpponent(town->tempOwner));
	}

	AIGateway::heroVisitsTown(hero, town);
}

void CScriptedAdventureAI::heroExperienceChanged(const CGHeroInstance * hero, si64 val)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID))
	{
		JsonNode data;
		data["hero_id"] = JsonNode(hero->id.getNum());
		data["delta"] = JsonNode(static_cast<int64_t>(val));
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		appendScriptUpdate("hero_experience_changed", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroExperienceChanged(hero, val);
}

void CScriptedAdventureAI::heroPrimarySkillChanged(const CGHeroInstance * hero, PrimarySkill which, si64 val)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID))
	{
		JsonNode data;
		data["hero_id"] = JsonNode(hero->id.getNum());
		data["primary_skill_id"] = JsonNode(which.getNum());
		data["delta"] = JsonNode(static_cast<int64_t>(val));
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		appendScriptUpdate("hero_primary_skill_changed", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroPrimarySkillChanged(hero, which, val);
}

void CScriptedAdventureAI::heroMovePointsChanged(const CGHeroInstance * hero)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID))
	{
		JsonNode data;
		data["hero_id"] = JsonNode(hero->id.getNum());
		data["movement"] = JsonNode(hero->movementPointsRemaining());
		data["movementLimit"] = JsonNode(hero->movementPointsLimit());
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		appendScriptUpdate("hero_move_points_changed", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroMovePointsChanged(hero);
}

void CScriptedAdventureAI::garrisonsChanged(ObjectInstanceID id1, ObjectInstanceID id2)
{
	const CGObjectInstance * first = cc ? cc->getObj(id1, false) : nullptr;
	const CGObjectInstance * second = cc ? cc->getObj(id2, false) : nullptr;
	const bool firstVisible = first && cc->isVisibleFor(first, playerID);
	const bool secondVisible = second && cc->isVisibleFor(second, playerID);
	if(firstVisible || secondVisible)
	{
		JsonNode data;
		data["id1"] = JsonNode(id1.getNum());
		data["id2"] = JsonNode(id2.getNum());
		if(firstVisible)
			data["first"] = jsonVisibleArmyHolder(first, playerID, cc);
		if(secondVisible)
			data["second"] = jsonVisibleArmyHolder(second, playerID, cc);
		appendScriptUpdate("garrisons_changed", data, (first && isOpponent(first->tempOwner)) || (second && isOpponent(second->tempOwner)));
	}

	AIGateway::garrisonsChanged(id1, id2);
}

void CScriptedAdventureAI::showPuzzleMap()
{
	JsonNode data = jsonGrailInfo(cc);
	data["grailPositionHidden"] = JsonNode(!readBool(data, "positionKnown", false));
	appendScriptUpdate("puzzle_map_shown", data, false);

	AIGateway::showPuzzleMap();
}

void CScriptedAdventureAI::viewWorldMap()
{
	JsonNode data;
	appendScriptUpdate("world_map_viewed", data, false);
}

void CScriptedAdventureAI::showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QueryID queryID)
{
	JsonNode data;
	if(object)
	{
		data["object_id"] = JsonNode(object->id.getNum());
		data["object"] = jsonMapObject(object, playerID, visitor);
	}
	if(visitor)
		data["visitor_hero_id"] = JsonNode(visitor->id.getNum());
	data["hireHeroOptions"].Vector();
	const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(object);
	if(!town && visitor)
		town = visitor->getVisitedTown();
	const CGObjectInstance * hireSource = town ? static_cast<const CGObjectInstance *>(town) : object;
	if(town && town->tempOwner == playerID && !town->getVisitingHero())
	{
		for(const CGHeroInstance * hero : cc->getAvailableHeroes(town))
		{
			if(hero)
				data["hireHeroOptions"].Vector().push_back(jsonAvailableHeroOption(town, hero, playerID));
		}
	}
	else if(hireSource && hireSource->ID == Obj::TAVERN && visitor && visitor->tempOwner == playerID && cc->isVisibleFor(hireSource, playerID))
	{
		for(const CGHeroInstance * hero : cc->getAvailableHeroes(hireSource))
		{
			if(hero)
				data["hireHeroOptions"].Vector().push_back(jsonAvailableHeroOption(hireSource, hero, playerID));
		}
	}
	recordScriptQuery(queryID, "tavern_window", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI tavern window", queryID);
		return;
	}
	AIGateway::showTavernWindow(object, visitor, queryID);
}

void CScriptedAdventureAI::showThievesGuildWindow(const CGObjectInstance * obj)
{
	JsonNode data;
	if(obj)
	{
		data["object_id"] = JsonNode(obj->id.getNum());
		if(cc && cc->isVisibleFor(obj, playerID))
			data["object"] = jsonMapObject(obj, playerID, nullptr);
	}
	appendScriptUpdate("thieves_guild_window", data, obj ? isOpponent(obj->tempOwner) : false);

	AIGateway::showThievesGuildWindow(obj);
}

void CScriptedAdventureAI::showShipyardDialog(const IShipyard * shipyard)
{
	JsonNode data;
	const auto * object = dynamic_cast<const CGObjectInstance *>(shipyard);
	if(object)
	{
		data["shipyard_id"] = JsonNode(object->id.getNum());
		if(cc && cc->isVisibleFor(object, playerID))
			data["object"] = jsonMapObject(object, playerID, nullptr);
		if(cc)
		{
			const bool enemy = cc->getPlayerRelations(playerID, object->tempOwner) == PlayerRelations::ENEMIES;
			data["shipyard"] = jsonShipyardOption(object, shipyard, cc->getResourceAmount(), enemy);
		}
	}
	appendScriptUpdate("shipyard_dialog", data, object ? isOpponent(object->tempOwner) : false);

	AIGateway::showShipyardDialog(shipyard);
}

void CScriptedAdventureAI::playerBonusChanged(const Bonus & bonus, bool gain)
{
	if(!bonus.hidden)
	{
		JsonNode data;
		data["bonus"] = jsonBonusChange(bonus, gain);
		appendScriptUpdate("player_bonus_changed", data, false);
	}

	AIGateway::playerBonusChanged(bonus, gain);
}

void CScriptedAdventureAI::advmapSpellCast(const CGHeroInstance * caster, SpellID spellID)
{
	if(caster && cc && cc->isVisibleFor(caster, playerID))
	{
		JsonNode data;
		data["hero_id"] = JsonNode(caster->id.getNum());
		data["hero"] = jsonHero(caster, caster->tempOwner == playerID);
		data["spell_id"] = JsonNode(spellID.getNum());
		if(const CSpell * spell = spellID.toSpell())
			data["spellIdentifier"] = JsonNode(spell->getJsonKey());
		appendScriptUpdate("adventure_spell_cast", data, isOpponent(caster->tempOwner));
	}

	AIGateway::advmapSpellCast(caster, spellID);
}

void CScriptedAdventureAI::heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query)
{
	JsonNode data;
	data["hero1_id"] = JsonNode(hero1.getNum());
	data["hero2_id"] = JsonNode(hero2.getNum());
	if(const CGHeroInstance * firstHero = cc->getHero(hero1))
		data["hero1"] = jsonHero(firstHero);
	if(const CGHeroInstance * secondHero = cc->getHero(hero2))
		data["hero2"] = jsonHero(secondHero);
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
	{
		data["upper_army_id"] = JsonNode(up->id.getNum());
		data["upperArmy"] = jsonOwnedArmySnapshot(up);
	}
	if(down)
	{
		data["lower_hero_id"] = JsonNode(down->id.getNum());
		data["lowerHero"] = jsonHero(down);
	}
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
	{
		data["dwelling_id"] = JsonNode(dwelling->id.getNum());
		data["dwelling"] = jsonMapObject(dwelling, playerID, nullptr);
	}
	if(dst)
	{
		data["destination_id"] = JsonNode(dst->id.getNum());
		data["destinationArmy"] = jsonOwnedArmySnapshot(dst);
	}
	data["level"] = JsonNode(level);
	data["recruitOptions"] = jsonRecruitOptions(dwelling, dst, level, cc->getResourceAmount());
	recordScriptQuery(queryID, "recruitment_dialog", data);

	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI recruitment dialog", queryID);
		return;
	}

	AIGateway::showRecruitmentDialog(dwelling, dst, level, queryID);
}

void CScriptedAdventureAI::showHillFortWindow(const CGObjectInstance * object, const CGHeroInstance * visitor)
{
	JsonNode data;
	if(object)
	{
		data["object_id"] = JsonNode(object->id.getNum());
		if(cc && cc->isVisibleFor(object, playerID))
			data["object"] = jsonMapObject(object, playerID, visitor);
	}
	if(visitor)
	{
		data["visitor_hero_id"] = JsonNode(visitor->id.getNum());
		if(cc && cc->isVisibleFor(visitor, playerID))
			data["visitorHero"] = jsonHero(visitor, visitor->tempOwner == playerID);
	}
	appendScriptUpdate("hill_fort_window", data, (object && isOpponent(object->tempOwner)) || (visitor && isOpponent(visitor->tempOwner)));

	AIGateway::showHillFortWindow(object, visitor);
}

void CScriptedAdventureAI::showInfoDialog(EInfoWindowMode type, const std::string & text, const std::vector<Component> & components, int soundID)
{
	JsonNode data;
	data["modeId"] = JsonNode(static_cast<int32_t>(type));
	data["mode"] = JsonNode(infoWindowModeName(type));
	data["text"] = JsonNode(jsonText(text));
	data["sound_id"] = JsonNode(soundID);
	data["components"] = jsonComponents(components);
	appendScriptUpdate("info_dialog", data, false);

	AIGateway::showInfoDialog(type, text, components, soundID);
}

void CScriptedAdventureAI::receivedResource()
{
	JsonNode data;
	if(cc)
		data["resources"] = jsonResources(cc->getResourceAmount());
	appendScriptUpdate("received_resource", data, false);

	AIGateway::receivedResource();
}

void CScriptedAdventureAI::showQuestLog()
{
	JsonNode data;
	data["quests"].Vector();
	if(cc)
	{
		for(const QuestInfo & questInfo : cc->getMyQuests())
			data["quests"].Vector().push_back(jsonQuestInfo(questInfo, cc.get(), playerID));
	}
	data["count"] = JsonNode(static_cast<int32_t>(data["quests"].Vector().size()));
	appendScriptUpdate("quest_log_shown", data, false);
}

void CScriptedAdventureAI::showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	JsonNode data;
	if(market)
	{
		data["market_id"] = JsonNode(market->getObjInstanceID().getNum());
		data["modeDetails"] = jsonMarketModeDetailsList(market);
		if(const CGObjectInstance * object = cc->getObj(market->getObjInstanceID(), false))
		{
			if(cc->isVisibleFor(object, playerID))
				data["market"] = jsonMapObject(object, playerID, visitor);
		}
	}
	if(visitor)
	{
		data["visitor_hero_id"] = JsonNode(visitor->id.getNum());
		data["visitorHero"] = jsonHero(visitor);
	}
	if(visitor && visitor->tempOwner == playerID && nullkiller && nullkiller->heroManager)
	{
		std::unique_lock aiLock(nullkiller->aiStateMutex);
		nullkiller->heroManager->update();
		data["skillOptions"] = jsonMarketSkillOptions(market, visitor, cc->getResourceAmount(), cc->getSettings(), nullkiller->heroManager.get());
	}
	else
	{
		data["skillOptions"] = jsonMarketSkillOptions(market, visitor, cc->getResourceAmount(), cc->getSettings());
	}
	recordScriptQuery(queryID, "university_window", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI university window", queryID);
		return;
	}
	AIGateway::showUniversityWindow(market, visitor, queryID);
}

void CScriptedAdventureAI::heroManaPointsChanged(const CGHeroInstance * hero)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID))
	{
		JsonNode data;
		data["hero_id"] = JsonNode(hero->id.getNum());
		data["mana"] = JsonNode(hero->mana);
		data["manaLimit"] = JsonNode(hero->manaLimit());
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		appendScriptUpdate("hero_mana_points_changed", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroManaPointsChanged(hero);
}

void CScriptedAdventureAI::heroSecondarySkillChanged(const CGHeroInstance * hero, int which, int val)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID))
	{
		JsonNode data;
		data["hero_id"] = JsonNode(hero->id.getNum());
		data["skill_id"] = JsonNode(which);
		data["level"] = JsonNode(val);
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		appendScriptUpdate("hero_secondary_skill_changed", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroSecondarySkillChanged(hero, which, val);
}

void CScriptedAdventureAI::heroBonusChanged(const CGHeroInstance * hero, const Bonus & bonus, bool gain)
{
	if(hero && cc && cc->isVisibleFor(hero, playerID) && !bonus.hidden)
	{
		JsonNode data;
		data["hero_id"] = JsonNode(hero->id.getNum());
		data["hero"] = jsonHero(hero, hero->tempOwner == playerID);
		data["bonus"] = jsonBonusChange(bonus, gain);
		appendScriptUpdate("hero_bonus_changed", data, isOpponent(hero->tempOwner));
	}

	AIGateway::heroBonusChanged(hero, bonus, gain);
}

void CScriptedAdventureAI::showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QueryID queryID)
{
	JsonNode data;
	if(market)
	{
		data["market_id"] = JsonNode(market->getObjInstanceID().getNum());
		data["modeDetails"] = jsonMarketModeDetailsList(market);
		if(const CGObjectInstance * object = cc->getObj(market->getObjInstanceID(), false))
		{
			if(cc->isVisibleFor(object, playerID))
				data["market"] = jsonMapObject(object, playerID, visitor);
		}
	}
	if(visitor)
	{
		data["visitor_hero_id"] = JsonNode(visitor->id.getNum());
		data["visitorHero"] = jsonHero(visitor);
	}
	data["altarOptions"] = jsonMarketAltarOptions(market, visitor);
	if(visitor && visitor->tempOwner == playerID && nullkiller && nullkiller->heroManager)
	{
		std::unique_lock aiLock(nullkiller->aiStateMutex);
		nullkiller->heroManager->update();
		data["skillOptions"] = jsonMarketSkillOptions(market, visitor, cc->getResourceAmount(), cc->getSettings(), nullkiller->heroManager.get());
	}
	else
	{
		data["skillOptions"] = jsonMarketSkillOptions(market, visitor, cc->getResourceAmount(), cc->getSettings());
	}
	recordScriptQuery(queryID, "market_window", data);
	if(isScriptActionAutoAnswerMode())
	{
		pauseForScriptActionQuery("ScriptedAdventureAI market window", queryID);
		return;
	}
	AIGateway::showMarketWindow(market, visitor, queryID);
}

void CScriptedAdventureAI::availableArtifactsChanged(const CGBlackMarket * blackMarket)
{
	JsonNode data;
	if(blackMarket)
	{
		data["black_market_id"] = JsonNode(blackMarket->id.getNum());
		if(cc && cc->isVisibleFor(blackMarket, playerID))
			data["blackMarket"] = jsonMapObject(blackMarket, playerID, nullptr);
		appendScriptUpdate("available_artifacts_changed", data, blackMarket && isOpponent(blackMarket->tempOwner));
	}
	else
	{
		data["global"] = JsonNode(true);
		appendScriptUpdate("available_artifacts_changed", data, false);
	}

	AIGateway::availableArtifactsChanged(blackMarket);
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

void CScriptedAdventureAI::responseStatistic(StatisticDataSet & statistic)
{
	JsonNode data;
	data["statistic"] = jsonStatisticDataSet(statistic);
	appendScriptUpdate("statistics_response", data, false);
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

void CScriptedAdventureAI::objectRemovedAfter()
{
	JsonNode data;
	appendScriptUpdate("object_removed_after", data, false);
}

void CScriptedAdventureAI::playerBlocked(int reason, bool start)
{
	JsonNode data;
	data["reasonId"] = JsonNode(reason);
	data["reason"] = JsonNode(playerBlockedReasonName(reason));
	data["started"] = JsonNode(start);
	appendScriptUpdate("player_blocked", data, false);

	AIGateway::playerBlocked(reason, start);
}

void CScriptedAdventureAI::gameOver(PlayerColor player, const EVictoryLossCheckResult & victoryLossCheckResult)
{
	JsonNode data;
	data["player"] = JsonNode(jsonPlayerColor(player));
	data["player_id"] = JsonNode(player.getNum());
	data["victory"] = JsonNode(victoryLossCheckResult.victory());
	data["loss"] = JsonNode(victoryLossCheckResult.loss());
	data["resultId"] = JsonNode(victoryLossCheckResult.victory() ? 1 : (victoryLossCheckResult.loss() ? -1 : 0));
	data["messageToSelf"] = JsonNode(jsonText(victoryLossCheckResult.messageToSelf.toString()));
	data["messageToOthers"] = JsonNode(jsonText(victoryLossCheckResult.messageToOthers.toString()));
	appendScriptUpdate("game_over", data, isOpponent(player));

	AIGateway::gameOver(player, victoryLossCheckResult);
}

void CScriptedAdventureAI::playerStartsTurn(PlayerColor player)
{
	JsonNode data;
	data["player"] = JsonNode(jsonPlayerColor(player));
	data["player_id"] = JsonNode(player.getNum());
	appendScriptUpdate("player_starts_turn", data, isOpponent(player));
}

void CScriptedAdventureAI::playerEndsTurn(PlayerColor player)
{
	JsonNode data;
	data["player"] = JsonNode(jsonPlayerColor(player));
	data["player_id"] = JsonNode(player.getNum());
	appendScriptUpdate("player_ends_turn", data, isOpponent(player));
}

void CScriptedAdventureAI::battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed)
{
	JsonNode data;
	data["battle_id"] = JsonNode(battleID.getNum());
	data["tile"] = jsonPosition(tile);
	data["sideId"] = JsonNode(static_cast<int32_t>(side));
	data["side"] = JsonNode(battleSideName(side));
	data["replayAllowed"] = JsonNode(replayAllowed);
	if(hero1)
	{
		data["attacker_hero_id"] = JsonNode(hero1->id.getNum());
		if(cc && cc->isVisibleFor(hero1, playerID))
			data["attackerHero"] = jsonHero(hero1, hero1->tempOwner == playerID);
	}
	if(hero2)
	{
		data["defender_hero_id"] = JsonNode(hero2->id.getNum());
		if(cc && cc->isVisibleFor(hero2, playerID))
			data["defenderHero"] = jsonHero(hero2, hero2->tempOwner == playerID);
	}
	if(army1)
		data["attackerArmy"] = jsonArmy(*army1);
	if(army2)
		data["defenderArmy"] = jsonArmy(*army2);
	appendScriptUpdate("battle_started", data, (hero1 && isOpponent(hero1->tempOwner)) || (hero2 && isOpponent(hero2->tempOwner)));

	AIGateway::battleStart(battleID, army1, army2, tile, hero1, hero2, side, replayAllowed);
}

void CScriptedAdventureAI::battleEnd(const BattleID & battleID, const BattleResult * br, QueryID queryID)
{
	JsonNode data;
	data["battle_id"] = JsonNode(battleID.getNum());
	data["query_id"] = JsonNode(queryID.getNum());
	if(br)
	{
		data["resultId"] = JsonNode(static_cast<int32_t>(br->result));
		data["result"] = JsonNode(battleResultName(br->result));
		data["winnerSideId"] = JsonNode(static_cast<int32_t>(br->winner));
		data["winnerSide"] = JsonNode(battleSideName(br->winner));
		data["attackerPlayer"] = JsonNode(jsonPlayerColor(br->attacker));
		data["attackerPlayerId"] = JsonNode(br->attacker.getNum());
		data["attackerExperience"] = JsonNode(static_cast<int64_t>(br->exp[BattleSide::ATTACKER]));
		data["defenderExperience"] = JsonNode(static_cast<int64_t>(br->exp[BattleSide::DEFENDER]));
		data["attackerCasualties"] = jsonBattleCasualties(br->casualties[BattleSide::ATTACKER]);
		data["defenderCasualties"] = jsonBattleCasualties(br->casualties[BattleSide::DEFENDER]);
	}
	appendScriptUpdate("battle_ended_with_result", data, false);

	AIGateway::battleEnd(battleID, br, queryID);
}

void CScriptedAdventureAI::battleResultsApplied()
{
	JsonNode data;
	appendScriptUpdate("battle_results_applied", data, false);

	AIGateway::battleResultsApplied();
}

void CScriptedAdventureAI::battleEnded()
{
	JsonNode data;
	appendScriptUpdate("battle_ended", data, false);

	AIGateway::battleEnded();
}

void CScriptedAdventureAI::showWorldViewEx(const std::vector<ObjectPosInfo> & objectPositions, bool showTerrain)
{
	JsonNode data;
	data["showTerrain"] = JsonNode(showTerrain);
	data["objects"].Vector();
	for(const ObjectPosInfo & objectPosition : objectPositions)
		data["objects"].Vector().push_back(jsonObjectPosInfo(objectPosition));
	data["count"] = JsonNode(static_cast<int32_t>(data["objects"].Vector().size()));
	appendScriptUpdate("world_view_shown", data, false);

	AIGateway::showWorldViewEx(objectPositions, showTerrain);
}

void CScriptedAdventureAI::setColorScheme(ColorScheme scheme)
{
	JsonNode data;
	data["schemeId"] = JsonNode(static_cast<int32_t>(scheme));
	data["scheme"] = JsonNode(colorSchemeName(scheme));
	appendScriptUpdate("color_scheme_changed", data, false);
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

JsonNode CScriptedAdventureAI::makeNullkillerTaskCandidates(const JsonNode & action, bool executionMode)
{
	const NK2AI::ScriptTaskSearchMode mode = readNullkillerTaskSearchMode(action);
	const size_t maxCandidates = readNullkillerCandidateLimit(action, "max_candidates", DEFAULT_NULLKILLER_TASK_CANDIDATES);
	const bool unboundedCandidates = maxCandidates == std::numeric_limits<size_t>::max();
	const size_t defaultSerializedTasks = executionMode && unboundedCandidates
		? static_cast<size_t>(DEFAULT_NULLKILLER_SERIALIZED_TASKS)
		: maxCandidates;
	const size_t serializedTaskLimit = readNullkillerSerializedTaskLimit(action, defaultSerializedTasks);

	JsonNode result;
	result["modeId"] = JsonNode(static_cast<int32_t>(mode));
	result["mode"] = JsonNode(nullkillerTaskSearchModeName(mode));
	result["candidateLimit"] = JsonNode(jsonNullkillerLimit(maxCandidates));
	result["serializedTaskLimit"] = JsonNode(jsonNullkillerLimit(serializedTaskLimit));
	result["tasks"].Vector();

	{
		std::shared_lock gameStateLock(CGameState::mutex);
		std::lock_guard sharedStorageLock(NK2AI::AISharedStorage::locker);
		NK2AI::Nullkiller::ScriptVisibleOnlyScope visibleOnly(*nullkiller);
		memorizeScriptVisibleVisitableObjs(nullkiller->memory, nullkiller->dangerHitMap, playerID, cc);
		AIGateway::memorizeRevisitableObjs(nullkiller->memory, playerID, cc);

		const auto candidates = nullkiller->getScriptTaskCandidates(mode, maxCandidates);

		nullkillerTaskHandles.clear();
		for(const NK2AI::ScriptTaskCandidate & candidate : candidates)
		{
			const int32_t taskID = nextNullkillerTaskHandle++;
			JsonNode taskJson = jsonNullkillerTaskCandidate(taskID, candidate, cc, playerID);
			nullkillerTaskHandles.push_back(NullkillerTaskHandle{taskID, candidate, taskJson});
			if(result["tasks"].Vector().size() < serializedTaskLimit)
				result["tasks"].Vector().push_back(taskJson);
		}

		result["candidateCount"] = JsonNode(static_cast<int32_t>(std::min<size_t>(candidates.size(), static_cast<size_t>(std::numeric_limits<int32_t>::max()))));
	}

	result["count"] = JsonNode(static_cast<int32_t>(result["tasks"].Vector().size()));
	result["serializedCount"] = result["count"];
	result["truncated"] = JsonNode(nullkillerTaskHandles.size() > result["tasks"].Vector().size());
	return result;
}

bool CScriptedAdventureAI::executeNullkillerTaskAction(const JsonNode & action, JsonNode & actionResult)
{
	const int32_t taskID = readInteger(action, "task_id");
	const auto taskIter = std::ranges::find_if(nullkillerTaskHandles, [taskID](const auto & entry)
	{
		return entry.id == taskID;
	});
	if(taskIter == nullkillerTaskHandles.end())
		throw std::invalid_argument("Unknown or expired Nullkiller task handle");

	const NK2AI::Goals::TTask task = taskIter->candidate.task;
	actionResult["task_id"] = JsonNode(taskID);

	bool executed = false;
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		std::lock_guard sharedStorageLock(NK2AI::AISharedStorage::locker);
		NK2AI::Nullkiller::ScriptVisibleOnlyScope visibleOnly(*nullkiller);
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

bool CScriptedAdventureAI::executeNullkillerQueryAction(const JsonNode & action, JsonNode & actionResult)
{
	const QueryID queryID(readInteger(action, "query_id"));
	const bool allowExpired = readBool(action, "allow_expired", false);
	if(allowExpired && status.getQueriesCount() <= 0)
	{
		removeScriptQuery(queryID);
		actionResult["query_id"] = JsonNode(queryID.getNum());
		actionResult["handledByNullkiller"] = JsonNode(false);
		actionResult["expired"] = JsonNode(true);
		actionResult["pendingQueries"] = JsonNode(0);
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	const std::optional<JsonNode> query = getScriptQuery(queryID);
	if(!query)
	{
		if(allowExpired)
		{
			actionResult["query_id"] = JsonNode(queryID.getNum());
			actionResult["handledByNullkiller"] = JsonNode(false);
			actionResult["expired"] = JsonNode(true);
			actionResult["ok"] = JsonNode(true);
			return true;
		}
		throw std::invalid_argument("Unknown or expired pending query");
	}

	const std::string queryType = readString(*query, "type");
	actionResult["query_id"] = JsonNode(queryID.getNum());
	actionResult["queryType"] = JsonNode(queryType);
	actionResult["handledByNullkiller"] = JsonNode(true);

	auto answerAndWait = [&](int answer) -> bool
	{
		answerQuery(queryID, answer);
		actionResult["answer"] = JsonNode(answer);
		if(!waitTillFreeForScriptAction(actionResult, "nullkiller_answer_query"))
			return false;
		removeScriptQuery(queryID);
		actionResult["ok"] = JsonNode(true);
		return true;
	};

	if(queryType == "hero_level_up")
	{
		const CGHeroInstance * hero = nullptr;
		if(hasField(*query, "hero_id"))
			hero = cc->getHero(ObjectInstanceID(readInteger(*query, "hero_id")));

		int answer = 0;
		if(hero && hero->tempOwner == playerID && hasField(*query, "skill_options") && (*query)["skill_options"].isVector())
		{
			const auto & options = (*query)["skill_options"].Vector();
			std::vector<SecondarySkill> skills;
			skills.reserve(options.size());
			for(const JsonNode & option : options)
				skills.emplace_back(readInteger(option, "skill_id"));

			if(!skills.empty())
			{
				std::unique_lock aiLock(nullkiller->aiStateMutex);
				nullkiller->heroManager->update();
				const int selectedIndex = nullkiller->heroManager->selectBestSkillIndex(NK2AI::HeroPtr(hero, cc.get()), skills);
				if(selectedIndex >= 0 && selectedIndex < static_cast<int>(options.size()))
					answer = readInteger(options[selectedIndex], "answer", selectedIndex);
			}
		}

		return answerAndWait(answer);
	}

	if(queryType == "commander_level_up")
		return answerAndWait(0);

	if(queryType == "blocking_dialog")
	{
		const bool selection = readBool(*query, "selection", false);
		const bool cancel = readBool(*query, "cancel", false);
		int answer = 0;

		if(!selection && cancel)
		{
			bool accept = true;
			const NK2AI::HeroPtr heroPtr = nullkiller->getActiveHero();
			const int3 target = nullkiller->getTargetTile();
			const auto objects = target.isValid() ? cc->getVisitableObjs(target) : std::vector<const CGObjectInstance *>();

			if(heroPtr.isVerified() && target.isValid() && !objects.empty())
			{
				const CGObjectInstance * topObj = objects.front()->id == heroPtr->id ? objects.back() : objects.front();
				const MapObjectID objectType = topObj->ID;
				const ObjectInstanceID goalObjectID = nullkiller->getTargetObject();
				const uint64_t danger = nullkiller->dangerEvaluator->evaluateDanger(target, heroPtr.get());
				const float ratio = static_cast<float>(danger) / static_cast<float>(heroPtr->getTotalStrength());

				if(topObj->id != goalObjectID && nullkiller->dangerEvaluator->evaluateDanger(topObj) > 0)
					accept = false;

				if(objectType == Obj::BORDERGUARD || objectType == Obj::QUEST_GUARD)
				{
					accept = true;
				}
				else if(objectType == Obj::ARTIFACT || objectType == Obj::RESOURCE)
				{
					const bool dangerUnknown = danger == 0;
					const bool dangerTooHigh = ratio * nullkiller->settings->getSafeAttackRatio() > 1;
					accept = !dangerUnknown && !dangerTooHigh;
				}
			}

			answer = accept ? 1 : 0;
			return answerAndWait(answer);
		}

		if(selection && hasField(*query, "components") && (*query)["components"].isVector())
		{
			const auto & components = (*query)["components"].Vector();
			if(!components.empty())
			{
				size_t selectedIndex = components.size() - 1;
				const NK2AI::HeroPtr heroPtr = nullkiller->getActiveHero();
				if(heroPtr.isVerified()
					&& components.size() == 2
					&& hasField(components.front(), "typeId")
					&& readInteger(components.front(), "typeId") == static_cast<int32_t>(ComponentType::RESOURCE))
				{
					std::unique_lock aiLock(nullkiller->aiStateMutex);
					if(nullkiller->heroManager->getHeroRoleOrDefault(heroPtr) != NK2AI::HeroRole::MAIN
						|| nullkiller->buildAnalyzer->isGoldPressureOverMax())
					{
						selectedIndex = 0;
					}
				}

				answer = readInteger(components[selectedIndex], "answer", static_cast<int32_t>(selectedIndex + 1));
			}
		}

		return answerAndWait(answer);
	}

	if(queryType == "teleport_dialog")
	{
		int answer = -1;
		const bool impassable = readBool(*query, "impassable", false);
		if(!impassable && hasField(*query, "exits") && (*query)["exits"].isVector())
		{
			const auto & exits = (*query)["exits"].Vector();
			for(size_t index = 0; index < exits.size(); ++index)
			{
				const ObjectInstanceID objectID(readInteger(exits[index], "object_id"));
				if(destinationTeleport != ObjectInstanceID() && objectID == destinationTeleport)
				{
					answer = readInteger(exits[index], "answer", static_cast<int32_t>(index));
					break;
				}
			}

			if(answer == -1 && status.channelProbing())
			{
				for(size_t index = 0; index < exits.size(); ++index)
				{
					const ObjectInstanceID objectID(readInteger(exits[index], "object_id"));
					if(objectID == destinationTeleport)
					{
						answer = readInteger(exits[index], "answer", static_cast<int32_t>(index));
						break;
					}
				}
			}

			if(answer == -1 && !exits.empty())
				answer = readInteger(exits.front(), "answer", 0);
		}
		return answerAndWait(answer);
	}

	if(queryType == "map_object_select")
	{
		int answer = selectedObject.getNum();
		if(answer == ObjectInstanceID().getNum() && hasField(*query, "objects") && (*query)["objects"].isVector() && !(*query)["objects"].Vector().empty())
			answer = readInteger((*query)["objects"].Vector().front(), "answer", 0);
		return answerAndWait(answer);
	}

	if(queryType == "hero_exchange")
	{
		const CGHeroInstance * firstHero = cc->getHero(ObjectInstanceID(readInteger(*query, "hero1_id")));
		const CGHeroInstance * secondHero = cc->getHero(ObjectInstanceID(readInteger(*query, "hero2_id")));
		if(firstHero && secondHero && firstHero->tempOwner == secondHero->tempOwner)
		{
			auto transferFromSourceToDestination = [this](const CGHeroInstance * destination, const CGHeroInstance * source)
			{
				pickBestCreatures(destination, source);
				AIGateway::pickBestArtifacts(cc, destination, source);
			};

			if(nullkiller->isActive(firstHero))
				transferFromSourceToDestination(secondHero, firstHero);
			else
				transferFromSourceToDestination(firstHero, secondHero);
		}
		return answerAndWait(0);
	}

	if(queryType == "garrison_dialog")
	{
		const auto * upper = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(readInteger(*query, "upper_army_id")), false));
		const CGHeroInstance * lower = cc->getHero(ObjectInstanceID(readInteger(*query, "lower_hero_id")));
		if(upper && lower
			&& readBool(*query, "removable_units", false)
			&& upper->tempOwner == lower->tempOwner
			&& nullkiller->settings->isGarrisonTroopsUsageAllowed()
			&& !cc->getStartInfo()->restrictedGarrisonsForAI())
		{
			pickBestCreatures(lower, upper);
		}
		return answerAndWait(0);
	}

	if(queryType == "recruitment_dialog")
	{
		const auto * dwelling = dynamic_cast<const CGDwelling *>(cc->getObj(ObjectInstanceID(readInteger(*query, "dwelling_id")), false));
		const auto * destination = dynamic_cast<const CArmedInstance *>(cc->getObj(ObjectInstanceID(readInteger(*query, "destination_id")), false));
		if(dwelling && destination && destination->tempOwner == playerID)
			recruitCreatures(dwelling, destination);
		return answerAndWait(0);
	}

	if(queryType == "tavern_window" || queryType == "university_window" || queryType == "market_window")
		return answerAndWait(0);

	actionResult["handledByNullkiller"] = JsonNode(false);
	return answerAndWait(readInteger(action, "default_answer", 0));
}

bool CScriptedAdventureAI::executeNullkillerStepAction(const JsonNode & action, JsonNode & actionResult)
{
	const JsonNode candidates = makeNullkillerTaskCandidates(action, true);
	actionResult["nullkiller"] = candidates;
	const auto & tasks = candidates["tasks"].Vector();
	if(nullkillerTaskHandles.empty())
	{
		actionResult["ok"] = JsonNode(true);
		actionResult["didExecute"] = JsonNode(false);
		return true;
	}

	const size_t maxAttempts = readNullkillerCandidateLimit(action, "max_attempts", static_cast<int32_t>(nullkillerTaskHandles.size()));
	NK2AI::Goals::TTaskVec nativeTasks;
	nativeTasks.reserve(nullkillerTaskHandles.size());
	for(const NullkillerTaskHandle & taskHandle : nullkillerTaskHandles)
		nativeTasks.push_back(taskHandle.candidate.task);

	auto taskJsonByIndex = [&](size_t index) -> std::optional<JsonNode>
	{
		if(index >= nullkillerTaskHandles.size())
			return std::nullopt;

		const NullkillerTaskHandle & taskHandle = nullkillerTaskHandles[index];
		if(index < tasks.size() && tasks[index]["task_id"].isNumber() && tasks[index]["task_id"].Integer() == taskHandle.id)
			return tasks[index];

		return taskHandle.taskJson;
	};

	NK2AI::ScriptTaskExecutionResult result;
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		std::lock_guard sharedStorageLock(NK2AI::AISharedStorage::locker);
		NK2AI::Nullkiller::ScriptVisibleOnlyScope visibleOnly(*nullkiller);
		result = nullkiller->executeScriptTaskSequence(nativeTasks, maxAttempts);
	}

	actionResult["ok"] = JsonNode(true);
	actionResult["didExecute"] = JsonNode(result.executed);
	actionResult["attempted"] = JsonNode(result.attempted);
	actionResult["attempts"] = JsonNode(static_cast<int32_t>(result.attempts));
	actionResult["maxAttempts"] = JsonNode(jsonNullkillerLimit(maxAttempts));
	actionResult["selectedTaskIndex"] = JsonNode(static_cast<int32_t>(result.selectedTaskIndex));
	actionResult["attemptedTasks"].Vector();
	for(const NK2AI::ScriptTaskAttemptResult & attemptResult : result.attemptResults)
	{
		JsonNode attempt;
		attempt["taskIndex"] = JsonNode(static_cast<int32_t>(attemptResult.taskIndex));
		attempt["executed"] = JsonNode(attemptResult.executed);
		attempt["failureActionId"] = JsonNode(static_cast<int32_t>(attemptResult.failureAction));
		attempt["failureAction"] = JsonNode(nullkillerTaskFailureActionName(attemptResult.failureAction));
		if(const auto taskJson = taskJsonByIndex(attemptResult.taskIndex))
		{
			attempt["task"] = *taskJson;
			attempt["task_id"] = (*taskJson)["task_id"];
		}
		if(!attemptResult.error.empty())
			attempt["error"] = JsonNode(attemptResult.error);
		actionResult["attemptedTasks"].Vector().push_back(attempt);
	}
	actionResult["attemptedTaskCount"] = JsonNode(static_cast<int32_t>(actionResult["attemptedTasks"].Vector().size()));
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
	if(const auto selectedTask = taskJsonByIndex(result.selectedTaskIndex))
	{
		actionResult["selectedTask"] = *selectedTask;
		actionResult["task_id"] = (*selectedTask)["task_id"];
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

bool CScriptedAdventureAI::executeNullkillerPassAction(const JsonNode & action, JsonNode & actionResult)
{
	const int32_t requestedMaxSteps = readInteger(action, "max_steps", 4);
	const size_t maxSteps = static_cast<size_t>(std::clamp<int32_t>(requestedMaxSteps, 1, 16));

	JsonNode steps;
	steps.Vector();
	int32_t executedSteps = 0;
	int32_t replanSteps = 0;
	int32_t stopTurnSteps = 0;
	int32_t exhaustedSteps = 0;
	bool paused = false;
	bool failed = false;

	for(size_t stepIndex = 0; stepIndex < maxSteps && status.haveTurn(); ++stepIndex)
	{
		JsonNode stepAction = action;
		setScriptActionType(stepAction, "nullkiller_step");
		if(!hasField(stepAction, "mode"))
			stepAction["mode"] = JsonNode(static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::ADVENTURE));

		JsonNode stepResult;
		stepResult["stepIndex"] = JsonNode(static_cast<int32_t>(stepIndex));
		const bool continueAfterStep = executeNullkillerStepAction(stepAction, stepResult);

		if(readBool(stepResult, "didExecute", false))
			++executedSteps;
		if(readBool(stepResult, "shouldReplan", false))
			++replanSteps;
		if(readBool(stepResult, "shouldStopTurn", false))
			++stopTurnSteps;
		if(readBool(stepResult, "exhaustedCandidates", false))
			++exhaustedSteps;
		if(hasField(stepResult, "ok") && stepResult["ok"].isBool() && !stepResult["ok"].Bool())
			failed = true;

		steps.Vector().push_back(stepResult);

		if(!continueAfterStep)
		{
			paused = !readBool(stepResult, "shouldStopTurn", false);
			break;
		}
		if(readBool(stepResult, "shouldStopTurn", false) || readBool(stepResult, "exhaustedCandidates", false))
			break;
		if(!readBool(stepResult, "didExecute", false) && !readBool(stepResult, "shouldReplan", false))
			break;
	}

	bool traded = false;
	if(executedSteps > 0 && status.haveTurn() && !paused)
	{
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			std::lock_guard sharedStorageLock(NK2AI::AISharedStorage::locker);
			NK2AI::Nullkiller::ScriptVisibleOnlyScope visibleOnly(*nullkiller);
			traded = nullkiller->executeScriptResourceTrade();
		}
		if(!waitTillFreeForScriptAction(actionResult, "nullkiller_pass"))
		{
			if(hasField(actionResult, "ok") && actionResult["ok"].isBool() && !actionResult["ok"].Bool())
				failed = true;
			paused = true;
		}
	}

	if(!hasField(actionResult, "ok"))
		actionResult["ok"] = JsonNode(!failed);
	else if(actionResult["ok"].isBool() && actionResult["ok"].Bool() && failed)
		actionResult["ok"] = JsonNode(false);
	actionResult["maxSteps"] = JsonNode(static_cast<int32_t>(maxSteps));
	actionResult["steps"] = steps;
	actionResult["stepCount"] = JsonNode(static_cast<int32_t>(steps.Vector().size()));
	actionResult["executedSteps"] = JsonNode(executedSteps);
	actionResult["replanSteps"] = JsonNode(replanSteps);
	actionResult["stopTurnSteps"] = JsonNode(stopTurnSteps);
	actionResult["exhaustedSteps"] = JsonNode(exhaustedSteps);
	actionResult["didTrade"] = JsonNode(traded);
	actionResult["paused"] = JsonNode(paused);
	return !failed && !paused && stopTurnSteps == 0;
}

bool CScriptedAdventureAI::executeNullkillerTurnSliceAction(const JsonNode & action, JsonNode & actionResult)
{
	const int32_t requestedMaxPasses = readInteger(action, "max_passes", 1);
	const size_t maxPasses = static_cast<size_t>(std::clamp<int32_t>(requestedMaxPasses, 1, 16));
	const bool includePriority = readBool(action, "include_priority", true);
	const bool includeAdventure = readBool(action, "include_adventure", true);
	const bool includeTrade = readBool(action, "include_trade", true);
	const bool optimizeArtifacts = readBool(action, "optimize_artifacts", true);
	const int32_t firstPassIndex = std::max(1, readInteger(action, "first_pass_index", 1));

	JsonNode passes;
	passes.Vector();
	int32_t priorityPasses = 0;
	int32_t priorityTasksExecuted = 0;
	int32_t adventureSteps = 0;
	int32_t adventureStepsExecuted = 0;
	int32_t adventureReplanSteps = 0;
	int32_t adventureStopTurnSteps = 0;
	int32_t adventureExhaustedSteps = 0;
	int32_t tradePasses = 0;
	int32_t artifactCleanupPasses = 0;
	bool didWork = false;
	bool paused = false;
	bool failed = false;
	bool shouldStopTurn = false;
	bool exhausted = false;

	for(size_t passIndex = 0; passIndex < maxPasses && status.haveTurn(); ++passIndex)
	{
		JsonNode passResult;
		passResult["passIndex"] = JsonNode(static_cast<int32_t>(passIndex));
		bool passDidWork = false;
		bool passPaused = false;
		bool passShouldStop = false;

		if(includePriority)
		{
			JsonNode priorityAction;
			setScriptActionType(priorityAction, "nullkiller_priority_pass");
			priorityAction["pass_index"] = JsonNode(firstPassIndex + static_cast<int32_t>(passIndex));

			JsonNode priorityResult;
			const bool continueAfterPriority = executeScriptAction(priorityAction, priorityResult);
			passResult["priority"] = priorityResult;
			++priorityPasses;

			if(hasField(priorityResult, "ok") && priorityResult["ok"].isBool() && !priorityResult["ok"].Bool())
				failed = true;

			const int32_t executed = readInteger(priorityResult, "executed", 0);
			if(executed > 0)
			{
				passDidWork = true;
				didWork = true;
				priorityTasksExecuted += executed;
			}

			if(!readBool(priorityResult, "completed", true))
				passShouldStop = true;
			if(!continueAfterPriority)
				passPaused = !passShouldStop;
		}

		if(!passPaused && !passShouldStop && includeAdventure && status.haveTurn())
		{
			JsonNode stepAction = action;
			setScriptActionType(stepAction, "nullkiller_step");
			if(hasField(action, "adventure_mode"))
				stepAction["mode"] = action["adventure_mode"];
			else if(!hasField(stepAction, "mode"))
				stepAction["mode"] = JsonNode(static_cast<int32_t>(NK2AI::ScriptTaskSearchMode::ADVENTURE));

			JsonNode stepResult;
			const bool continueAfterStep = executeNullkillerStepAction(stepAction, stepResult);
			passResult["adventure"] = stepResult;
			++adventureSteps;

			if(hasField(stepResult, "ok") && stepResult["ok"].isBool() && !stepResult["ok"].Bool())
				failed = true;
			if(readBool(stepResult, "didExecute", false))
			{
				passDidWork = true;
				didWork = true;
				++adventureStepsExecuted;
			}
			if(readBool(stepResult, "shouldReplan", false))
			{
				passDidWork = true;
				didWork = true;
				++adventureReplanSteps;
			}
			if(readBool(stepResult, "shouldStopTurn", false))
			{
				passShouldStop = true;
				++adventureStopTurnSteps;
			}
			if(readBool(stepResult, "exhaustedCandidates", false))
			{
				exhausted = true;
				++adventureExhaustedSteps;
			}

			if(!continueAfterStep)
				passPaused = !passShouldStop;
		}

		bool traded = false;
		if(!passPaused && !passShouldStop && includeTrade && status.haveTurn())
		{
			{
				std::shared_lock gameStateLock(CGameState::mutex);
				std::lock_guard sharedStorageLock(NK2AI::AISharedStorage::locker);
				NK2AI::Nullkiller::ScriptVisibleOnlyScope visibleOnly(*nullkiller);
				traded = nullkiller->executeScriptResourceTrade();
			}
			if(!waitTillFreeForScriptAction(actionResult, "nullkiller_turn_slice"))
			{
				if(hasField(actionResult, "ok") && actionResult["ok"].isBool() && !actionResult["ok"].Bool())
					failed = true;
				passPaused = true;
			}

			if(traded)
			{
				++tradePasses;
				passDidWork = true;
				didWork = true;
			}
		}
		passResult["didTrade"] = JsonNode(traded);

		if(!passPaused && !passShouldStop && includePriority && includeAdventure && includeTrade && !passDidWork)
			passShouldStop = true;

		if(!passPaused && !passShouldStop && optimizeArtifacts && passDidWork && status.haveTurn())
		{
			for(const auto * heroInfo : cc->getHeroesInfo())
				AIGateway::pickBestArtifacts(cc, heroInfo);
			++artifactCleanupPasses;
		}

		passResult["didWork"] = JsonNode(passDidWork);
		passes.Vector().push_back(passResult);

		if(passPaused)
		{
			paused = true;
			break;
		}
		if(passShouldStop)
		{
			shouldStopTurn = true;
			break;
		}
		if(!passDidWork)
			break;
	}

	if(!hasField(actionResult, "ok"))
		actionResult["ok"] = JsonNode(!failed);
	else if(actionResult["ok"].isBool() && actionResult["ok"].Bool() && failed)
		actionResult["ok"] = JsonNode(false);
	actionResult["maxPasses"] = JsonNode(static_cast<int32_t>(maxPasses));
	actionResult["passes"] = passes;
	actionResult["passCount"] = JsonNode(static_cast<int32_t>(passes.Vector().size()));
	actionResult["priorityPasses"] = JsonNode(priorityPasses);
	actionResult["priorityTasksExecuted"] = JsonNode(priorityTasksExecuted);
	actionResult["adventureSteps"] = JsonNode(adventureSteps);
	actionResult["adventureStepsExecuted"] = JsonNode(adventureStepsExecuted);
	actionResult["adventureReplanSteps"] = JsonNode(adventureReplanSteps);
	actionResult["adventureStopTurnSteps"] = JsonNode(adventureStopTurnSteps);
	actionResult["adventureExhaustedSteps"] = JsonNode(adventureExhaustedSteps);
	actionResult["tradePasses"] = JsonNode(tradePasses);
	actionResult["artifactCleanupPasses"] = JsonNode(artifactCleanupPasses);
	actionResult["didWork"] = JsonNode(didWork);
	actionResult["paused"] = JsonNode(paused);
	actionResult["shouldStopTurn"] = JsonNode(shouldStopTurn);
	actionResult["exhaustedCandidates"] = JsonNode(exhausted);
	return !failed && !paused && !shouldStopTurn;
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
		cachedRunner.reset();
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

	std::unique_ptr<scripting::LuaAdventureScriptRunner> transientRunner;
	scripting::LuaAdventureScriptRunner * runner = nullptr;
	if(scriptConfig.reloadScriptEachTurn)
	{
		cachedRunner.reset();
		transientRunner = makeRunner(*source);
		runner = transientRunner.get();
	}
	else
	{
		if(!cachedRunner)
			cachedRunner = makeRunner(*source);
		runner = cachedRunner.get();
	}

	{
		std::shared_lock gameStateLock(CGameState::mutex);
		nullkiller->resetScriptTaskState();
	}

	if(!runner->hasRunDay())
	{
		fallbackToNullkiller("script does not define imperative runDay");
		return false;
	}

	return tryMakeImperativeScriptedTurn(*runner);
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

			if(kind == "inspect")
			{
				response["result"] = executeScriptInspect(command["payload"]);
				if(scriptConfig.trace)
				{
					JsonNode trace;
					trace["command"] = command;
					trace["response"] = response;
					trace["progress"] = progress;
					writeTraceEvent("imperative-command", trace);
				}
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

JsonNode CScriptedAdventureAI::executeScriptInspect(const JsonNode & request)
{
	const std::string what = readString(request, "what");

	if(what == "state")
		return makeScriptInputState();
	if(what == "action_space")
		return makeScriptActionSpace();
	if(what == "analysis")
		return makeScriptAnalysis();
	if(what == "queries")
		return makeScriptQueries();
	if(what == "updates")
		return makeScriptUpdates(readBool(request, "opponent_only", false));
	if(what == "limits")
		return makeScriptInputLimits();
	if(what == "grail")
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		return jsonGrailInfo(cc);
	}
	if(what == "nullkiller_tasks" || what == "nullkiller_task_candidates")
	{
		JsonNode action = request;
		if(!hasField(action, "mode") && hasField(action, "mode_id"))
			action["mode"] = action["mode_id"];
		return makeNullkillerTaskCandidates(action);
	}
	if(what == "danger" || what == "risk")
	{
		if(!nullkiller || !nullkiller->dangerEvaluator || !nullkiller->settings)
			throw std::invalid_argument("Nullkiller danger evaluator is not available");

		std::shared_lock gameStateLock(CGameState::mutex);
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(request, "hero_id")));
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

		const bool checkGuards = readBool(request, "check_guards", true);
		JsonNode node;
		node["hero_id"] = JsonNode(hero->id.getNum());
		node["checkGuards"] = JsonNode(checkGuards);

		uint64_t danger = 0;
		if(hasField(request, "object_id"))
		{
			const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(request, "object_id")), false);
			if(!object || !cc->isVisibleFor(object, playerID))
				throw std::invalid_argument("Unknown object or object is not visible to scripted AI");

			const uint64_t objectDanger = nullkiller->dangerEvaluator->evaluateDanger(object);
			danger = objectDanger;
			node["targetKindId"] = JsonNode(2);
			node["targetKind"] = JsonNode("object");
			node["object_id"] = JsonNode(object->id.getNum());
			node["object"] = jsonMapObject(object, playerID, hero);
			node["objectDanger"] = JsonNode(static_cast<int64_t>(objectDanger));

			const int3 position = object->visitablePos();
			if(position.isValid() && cc->isInTheMap(position) && cc->isVisibleFor(position, playerID))
			{
				const uint64_t tileDanger = nullkiller->dangerEvaluator->evaluateDanger(position, hero, checkGuards);
				danger = tileDanger;
				node["position"] = jsonPosition(position);
				node["tileDanger"] = JsonNode(static_cast<int64_t>(tileDanger));
			}
		}
		else
		{
			const int3 position(readInteger(request, "x"), readInteger(request, "y"), readInteger(request, "z", hero->visitablePos().z));
			if(!cc->isInTheMap(position) || !cc->isVisibleFor(position, playerID))
				throw std::invalid_argument("Danger target tile is outside the map or is not visible to scripted AI");

			danger = nullkiller->dangerEvaluator->evaluateDanger(position, hero, checkGuards);
			node["targetKindId"] = JsonNode(1);
			node["targetKind"] = JsonNode("tile");
			node["position"] = jsonPosition(position);
			node["tileDanger"] = JsonNode(static_cast<int64_t>(danger));
		}

		const bool safe = !danger || NK2AI::isSafeToVisit(hero, danger, nullkiller->settings->getSafeAttackRatio());
		JsonNode risk = jsonRisk(hero, danger, safe);
		for(const auto & [key, value] : risk.Struct())
			node[key] = value;
		return node;
	}

	if(what == "object")
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(request, "object_id")), false);
		if(!object || !cc->isVisibleFor(object, playerID))
			throw std::invalid_argument("Unknown object or object is not visible to scripted AI");

		const CGHeroInstance * contextHero = nullptr;
		if(hasField(request, "hero_id"))
		{
			contextHero = cc->getHero(ObjectInstanceID(readInteger(request, "hero_id")));
			if(!contextHero || contextHero->tempOwner != playerID || !cc->isVisibleFor(contextHero, playerID))
				throw std::invalid_argument("Unknown context hero, hero is not visible, or hero is not owned by scripted AI");
		}

		return jsonMapObject(object, playerID, contextHero);
	}

	if(what == "hero")
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(request, "hero_id")));
		if(!hero || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero or hero is not visible to scripted AI");

		return jsonHero(hero, hero->tempOwner == playerID);
	}

	if(what == "town")
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(request, "town_id")));
		if(!town || !cc->isVisibleFor(town, playerID))
			throw std::invalid_argument("Unknown town or town is not visible to scripted AI");

		return jsonTown(town, cc->getResourceAmount(), town->tempOwner == playerID);
	}

	if(what == "tile" || what == "objects_at")
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		const int3 position(readInteger(request, "x"), readInteger(request, "y"), readInteger(request, "z", 0));
		if(!cc->isInTheMap(position) || !cc->isVisibleFor(position, playerID))
			throw std::invalid_argument("Requested tile is outside the map or is not visible to scripted AI");

		const TerrainTile * tile = cc->getTile(position, false);
		if(!tile)
			throw std::invalid_argument("Requested tile is not available to scripted AI");

		if(what == "tile")
			return jsonVisibleTile(position, *tile, cc, playerID);

		JsonNode node;
		node["position"] = jsonPosition(position);
		node["topObject"] = JsonNode();
		if(const CGObjectInstance * topObject = cc->getTopObj(position))
		{
			if(cc->isVisibleFor(topObject, playerID))
				node["topObject"] = jsonMapObject(topObject, playerID, nullptr);
		}

		auto appendVisibleObject = [&](JsonNode & target, const CGObjectInstance * object)
		{
			if(object && cc->isVisibleFor(object, playerID))
				target.Vector().push_back(jsonMapObject(object, playerID, nullptr));
		};

		node["visitableObjects"].Vector();
		for(const CGObjectInstance * object : cc->getVisitableObjs(position, false))
			appendVisibleObject(node["visitableObjects"], object);

		node["blockingObjects"].Vector();
		for(const CGObjectInstance * object : cc->getBlockingObjs(position))
			appendVisibleObject(node["blockingObjects"], object);

		return node;
	}

	if(what == "available_heroes")
	{
		std::shared_lock gameStateLock(CGameState::mutex);
		const CGObjectInstance * source = cc->getObj(ObjectInstanceID(readInteger(request, "source_id")), false);
		if(!source || !cc->isVisibleFor(source, playerID))
			throw std::invalid_argument("Unknown hero-recruitment source or source is not visible to scripted AI");

		JsonNode node;
		node["source_id"] = JsonNode(source->id.getNum());
		node["sourceObject"] = jsonMapObject(source, playerID, nullptr);
		node["heroes"].Vector();
		for(const CGHeroInstance * hero : cc->getAvailableHeroes(source))
		{
			if(!hero)
				continue;
			JsonNode heroNode = jsonHero(hero, false);
			heroNode["planAction"] = jsonAvailableHeroOption(source, hero, playerID)["planAction"];
			node["heroes"].Vector().push_back(heroNode);
		}
		node["heroCount"] = JsonNode(static_cast<int32_t>(node["heroes"].Vector().size()));
		return node;
	}

	if(what == "path")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(request, "hero_id")));
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

		int3 destination;
		ObjectInstanceID objectID = ObjectInstanceID::NONE;
		if(hasField(request, "object_id"))
		{
			const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(request, "object_id")), false);
			if(!object || !cc->isVisibleFor(object, playerID))
				throw std::invalid_argument("Unknown object or object is not visible to scripted AI");
			if(!isScriptObjectTarget(object, playerID))
				throw std::invalid_argument("Path target object is not a valid script visit target");
			objectID = object->id;
			destination = object->visitablePos();
		}
		else
		{
			destination = int3(readInteger(request, "x"), readInteger(request, "y"), readInteger(request, "z", hero->visitablePos().z));
			if(!cc->isInTheMap(destination) || !cc->isVisibleFor(destination, playerID))
				throw std::invalid_argument("Path destination is outside the map or is not visible to scripted AI");
		}

		JsonNode node;
		node["hero_id"] = JsonNode(hero->id.getNum());
		node["destination"] = jsonPosition(destination);
		if(objectID != ObjectInstanceID::NONE)
			node["object_id"] = JsonNode(objectID.getNum());

		const RoutePlan route = makeRoutePlan(hero, destination, std::nullopt);
		node["reachable"] = JsonNode(route.ok);
		if(!route.ok)
		{
			node["error"] = JsonNode(route.error);
			return node;
		}

		node["route_id"] = JsonNode(route.routeID);
		node["submittedPath"] = jsonPositions(route.requestPath);
		node["stopAfterMove"] = JsonNode(route.stopAfterMove);
		node["transit"] = JsonNode(route.transit);
		node["layerId"] = JsonNode(route.layer.getNum());
		if(objectID != ObjectInstanceID::NONE)
		{
			setScriptActionType(node["planAction"], "visit_object");
			node["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
			node["planAction"]["object_id"] = JsonNode(objectID.getNum());
			node["planAction"]["route_id"] = JsonNode(route.routeID);
		}
		else
		{
			setScriptActionType(node["planAction"], "move_hero");
			node["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
			node["planAction"]["x"] = JsonNode(destination.x);
			node["planAction"]["y"] = JsonNode(destination.y);
			node["planAction"]["z"] = JsonNode(destination.z);
			node["planAction"]["route_id"] = JsonNode(route.routeID);
		}
		return node;
	}

	if(what == "reachable")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(request, "hero_id")));
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

		const int32_t radius = std::clamp(readInteger(request, "radius", 16), 1, 64);
		const size_t maxMovementOptions = static_cast<size_t>(std::clamp(readInteger(request, "max_movement_options", 64), 0, 512));
		const size_t maxObjectTargets = static_cast<size_t>(std::clamp(readInteger(request, "max_object_targets", 64), 0, 512));

		JsonNode node;
		node["hero_id"] = JsonNode(hero->id.getNum());
		node["radius"] = JsonNode(radius);
		node["movementOptions"].Vector();
		node["reachableObjects"].Vector();

		std::shared_lock gameStateLock(CGameState::mutex);
		if(hero->movementPointsRemaining() <= 0)
			return node;

		CPathsInfo paths(cc->getMapSize(), hero);
		auto config = std::make_shared<SingleHeroPathfinderConfig>(paths, *cc, hero);
		cc->calculatePaths(config);

		FowTilesType tiles;
		cc->getTilesInRange(tiles, hero->visitablePos(), radius, ETileVisibility::REVEALED, playerID);

		size_t reachableTileCount = 0;
		size_t reachableObjectCount = 0;
		std::set<int32_t> seenTargetObjects;
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

			++reachableTileCount;
			const std::string routeID = makeRouteId(hero->id.getNum(), hero->visitablePos(), position, pathNode->layer, pathNode->moveRemains);
			const uint64_t danger = nullkiller && nullkiller->dangerEvaluator ? nullkiller->dangerEvaluator->evaluateDanger(position, hero, true) : 0;
			const bool safe = !danger || (nullkiller && nullkiller->settings && NK2AI::isSafeToVisit(hero, danger, nullkiller->settings->getSafeAttackRatio()));
			const JsonNode risk = jsonRisk(hero, danger, safe);

			if(node["movementOptions"].Vector().size() < maxMovementOptions)
			{
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
				setScriptActionType(option["planAction"], "move_hero");
				option["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
				option["planAction"]["x"] = JsonNode(position.x);
				option["planAction"]["y"] = JsonNode(position.y);
				option["planAction"]["z"] = JsonNode(position.z);
				option["planAction"]["route_id"] = JsonNode(routeID);
				node["movementOptions"].Vector().push_back(option);
			}

			const CGObjectInstance * topObject = cc->getTopObj(position);
			if(topObject
				&& isObjectPathAction(pathNode->action)
				&& isScriptObjectTarget(topObject, playerID)
				&& seenTargetObjects.insert(topObject->id.getNum()).second)
			{
				++reachableObjectCount;
				if(node["reachableObjects"].Vector().size() < maxObjectTargets)
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
					setScriptActionType(target["planAction"], "visit_object");
					target["planAction"]["hero_id"] = JsonNode(hero->id.getNum());
					target["planAction"]["object_id"] = JsonNode(topObject->id.getNum());
					target["planAction"]["route_id"] = JsonNode(routeID);
					node["reachableObjects"].Vector().push_back(target);
				}
			}
		}

		node["reachableTileCount"] = JsonNode(static_cast<int32_t>(reachableTileCount));
		node["reachableObjectCount"] = JsonNode(static_cast<int32_t>(reachableObjectCount));
		node["movementOptionsTruncated"] = JsonNode(reachableTileCount > node["movementOptions"].Vector().size());
		node["reachableObjectsTruncated"] = JsonNode(reachableObjectCount > node["reachableObjects"].Vector().size());
		return node;
	}

	throw std::invalid_argument("Unsupported inspect request: " + what);
}

bool CScriptedAdventureAI::executeScriptAction(const JsonNode & action, JsonNode & actionResult)
{
	const std::string type = readScriptActionType(action);
	ScopedCallbackWaitMode callbackWaitMode(cc, usesSynchronousNativeAiRequests(type));
	actionResult["type"] = JsonNode(type);
	const int32_t typeId = scriptActionTypeId(type);
	if(typeId != 0)
		actionResult["typeId"] = JsonNode(typeId);
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
	if(type != "nullkiller_reset" && type != "nullkiller_trade" && type != "nullkiller_priority_pass" && type != "nullkiller_tasks" && type != "nullkiller_task" && type != "nullkiller_step" && type != "nullkiller_pass" && type != "nullkiller_turn_slice" && type != "nullkiller_answer_query")
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

	if(type == "nullkiller_reset")
	{
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			nullkiller->resetScriptTaskState();
		}
		nullkillerTaskHandles.clear();
		actionResult["ok"] = JsonNode(true);
		actionResult["reset"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_lock_resources")
	{
		const ResourceSet resources = readResourceAmounts(action, "resources");
		ResourceSet lockedResources;
		ResourceSet freeResources;
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			std::unique_lock aiLock(nullkiller->aiStateMutex);
			nullkiller->lockResources(resources);
			lockedResources = nullkiller->getLockedResources();
			freeResources = nullkiller->getFreeResources();
		}

		actionResult["ok"] = JsonNode(true);
		actionResult["resources"] = jsonResources(resources);
		actionResult["lockedResources"] = jsonResources(lockedResources);
		actionResult["freeResources"] = jsonResources(freeResources);
		return true;
	}

	if(type == "nullkiller_lock_hero")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

		const NK2AI::HeroLockedReason reason = readNullkillerHeroLockedReason(action);
		{
			std::unique_lock aiLock(nullkiller->aiStateMutex);
			nullkiller->lockHero(hero, reason);
		}

		actionResult["ok"] = JsonNode(true);
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["reasonId"] = JsonNode(static_cast<int32_t>(reason));
		actionResult["reason"] = JsonNode(nullkillerHeroLockedReasonName(reason));
		return true;
	}

	if(type == "nullkiller_unlock_hero")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

		{
			std::unique_lock aiLock(nullkiller->aiStateMutex);
			nullkiller->unlockHero(hero);
		}

		actionResult["ok"] = JsonNode(true);
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["reasonId"] = JsonNode(static_cast<int32_t>(NK2AI::HeroLockedReason::NOT_LOCKED));
		actionResult["reason"] = JsonNode(nullkillerHeroLockedReasonName(NK2AI::HeroLockedReason::NOT_LOCKED));
		return true;
	}

	if(type == "nullkiller_trade")
	{
		bool traded = false;
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			std::lock_guard sharedStorageLock(NK2AI::AISharedStorage::locker);
			NK2AI::Nullkiller::ScriptVisibleOnlyScope visibleOnly(*nullkiller);
			traded = nullkiller->executeScriptResourceTrade();
		}
		actionResult["ok"] = JsonNode(true);
		actionResult["didTrade"] = JsonNode(traded);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		return true;
	}

	if(type == "nullkiller_priority_pass")
	{
		const int32_t passIndex = std::max(1, readInteger(action, "pass_index", 1));
		NK2AI::ScriptPriorityPassResult result;
		std::optional<JsonNode> lastTask;
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			std::lock_guard sharedStorageLock(NK2AI::AISharedStorage::locker);
			NK2AI::Nullkiller::ScriptVisibleOnlyScope visibleOnly(*nullkiller);
			result = nullkiller->executeScriptPriorityPass(passIndex);
			if(result.lastTask)
			{
				NK2AI::HeroRole heroRole = NK2AI::HeroRole::MAIN;
				if(const CGHeroInstance * hero = result.lastTask->getHero())
				{
					NK2AI::HeroPtr heroPtr(hero, cc.get());
					if(heroPtr.isVerified() && nullkiller->heroManager)
						heroRole = nullkiller->heroManager->getHeroRoleOrDefault(heroPtr);
				}

				lastTask = jsonNullkillerTaskCandidate(
					0,
					NK2AI::ScriptTaskCandidate{
						result.lastTask,
						NK2AI::ScriptTaskSearchMode::PRIORITY,
						NK2AI::PriorityEvaluator::PriorityTier::BUILDINGS,
						heroRole
					},
					cc,
					playerID);
			}
		}

		actionResult["ok"] = JsonNode(true);
		actionResult["completed"] = JsonNode(result.completed);
		actionResult["maxPriorityPassReached"] = JsonNode(result.maxPriorityPassReached);
		actionResult["passIndex"] = JsonNode(result.passIndex);
		actionResult["attempts"] = JsonNode(result.attempts);
		actionResult["executed"] = JsonNode(result.executed);
		actionResult["lastPriority"].Float() = result.lastPriority;
		if(!result.lastTaskDescription.empty())
			actionResult["lastTaskDescription"] = JsonNode(result.lastTaskDescription);
		if(lastTask)
			actionResult["lastTask"] = *lastTask;
		if(!result.error.empty())
			actionResult["error"] = JsonNode(result.error);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		return true;
	}

	if(type == "nullkiller_build_army")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID || !cc->isVisibleFor(town, playerID))
			throw std::invalid_argument("Unknown town, town is not visible, or town is not owned by scripted AI");

		buildArmyIn(town);
		actionResult["town_id"] = JsonNode(town->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_upgrade_army")
	{
		const CArmedInstance * army = readOwnedArmy("army_id", "upgrade");
		if(!cc->isVisibleFor(army, playerID))
			throw std::invalid_argument("Army holder is not visible to scripted AI");

		const bool upgraded = makePossibleUpgrades(army);
		actionResult["army_id"] = JsonNode(army->id.getNum());
		actionResult["didUpgrade"] = JsonNode(upgraded);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_recruit_creatures")
	{
		const CGObjectInstance * sourceObject = cc->getObj(ObjectInstanceID(readInteger(action, "source_id")), false);
		const CGDwelling * dwelling = dynamic_cast<const CGDwelling *>(sourceObject);
		const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(sourceObject);
		if(!sourceObject || !dwelling || sourceObject->tempOwner != playerID || !cc->isVisibleFor(sourceObject, playerID))
			throw std::invalid_argument("Unknown recruitment source, source is not visible, or source is not owned by scripted AI");

		const CArmedInstance * destination = town ? town->getUpperArmy() : dynamic_cast<const CArmedInstance *>(sourceObject);
		if(hasField(action, "destination_id"))
			destination = readOwnedArmy("destination_id", "recruitment destination");
		if(!destination || destination->tempOwner != playerID || !cc->isVisibleFor(destination, playerID))
			throw std::invalid_argument("Unknown recruitment destination, destination is not visible, or destination is not owned by scripted AI");

		recruitCreatures(dwelling, destination);
		actionResult["source_id"] = JsonNode(sourceObject->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_move_creatures_to_hero")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID || !cc->isVisibleFor(town, playerID))
			throw std::invalid_argument("Unknown town, town is not visible, or town is not owned by scripted AI");

		const CGHeroInstance * visitingHero = town->getVisitingHero();
		if(!visitingHero || visitingHero->tempOwner != playerID)
			throw std::invalid_argument("Town must have an owned visiting hero for Nullkiller creature pickup");
		if(!town->armedGarrison())
			throw std::invalid_argument("Town garrison has no creatures to move to the visiting hero");

		moveCreaturesToHero(town);
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["hero_id"] = JsonNode(visitingHero->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_dismiss_weak_hero")
	{
		if(!nullkiller || !nullkiller->heroManager)
			throw std::invalid_argument("Nullkiller hero manager is not available");

		const bool requireCapReached = readBool(action, "require_cap_reached", true);
		const uint64_t armyLimit = hasField(action, "army_limit")
			? static_cast<uint64_t>(std::max<int32_t>(0, readInteger(action, "army_limit")))
			: std::numeric_limits<uint64_t>::max();
		const CGTownInstance * townToSpare = nullptr;
		if(hasField(action, "town_to_spare_id"))
		{
			townToSpare = cc->getTown(ObjectInstanceID(readInteger(action, "town_to_spare_id")));
			if(!townToSpare || townToSpare->tempOwner != playerID || !cc->isVisibleFor(townToSpare, playerID))
				throw std::invalid_argument("Unknown spare town, town is not visible, or town is not owned by scripted AI");
		}

		ObjectInstanceID selectedHeroID = ObjectInstanceID::NONE;
		{
			std::shared_lock gameStateLock(CGameState::mutex);
			std::unique_lock aiLock(nullkiller->aiStateMutex);
			nullkiller->heroManager->update();
			if(!requireCapReached || nullkiller->heroManager->heroCapReached())
			{
				if(const CGHeroInstance * selectedHero = nullkiller->heroManager->findWeakHeroToDismiss(armyLimit, townToSpare))
					selectedHeroID = selectedHero->id;
			}
		}

		if(selectedHeroID == ObjectInstanceID::NONE)
		{
			actionResult["didDismiss"] = JsonNode(false);
			actionResult["ok"] = JsonNode(true);
			return true;
		}

		const CGHeroInstance * hero = cc->getHero(selectedHeroID);
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Nullkiller selected an unknown, non-visible, or non-owned weak hero");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(DismissHero), CTypeList::getInstance().getTypeID<DismissHero>(nullptr), [&]
		{
			cc->dismissHero(hero);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["didDismiss"] = JsonNode(request.applied);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Dismiss weak hero request was rejected by server" : "Dismiss weak hero request was not realized by server");
		return true;
	}

	if(type == "nullkiller_optimize_artifacts")
	{
		int32_t optimizedHeroes = 0;
		if(hasField(action, "hero_id"))
		{
			const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
			if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
				throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

			AIGateway::pickBestArtifacts(cc, hero);
			actionResult["hero_id"] = JsonNode(hero->id.getNum());
			optimizedHeroes = 1;
		}
		else
		{
			for(const auto * heroInfo : cc->getHeroesInfo())
			{
				if(heroInfo && heroInfo->tempOwner == playerID && cc->isVisibleFor(heroInfo, playerID))
				{
					AIGateway::pickBestArtifacts(cc, heroInfo);
					++optimizedHeroes;
				}
			}
		}

		actionResult["optimizedHeroes"] = JsonNode(optimizedHeroes);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_add_single_creature_stacks" || type == "nullkiller_rearrange_for_whirlpool")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

		if(type == "nullkiller_rearrange_for_whirlpool")
			nullkiller->armyFormation->rearrangeArmyForWhirlpool(hero);
		else
			nullkiller->armyFormation->addSingleCreatureStacks(hero);
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_rearrange_for_siege")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			throw std::invalid_argument("Unknown hero, hero is not visible, or hero is not owned by scripted AI");

		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || !cc->isVisibleFor(town, playerID))
			throw std::invalid_argument("Unknown siege target town or town is not visible to scripted AI");
		if(cc->getPlayerRelations(playerID, town->tempOwner) != PlayerRelations::ENEMIES)
			throw std::invalid_argument("Siege preparation target town is not an enemy");

		nullkiller->armyFormation->rearrangeArmyForSiege(town, hero);
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["town_id"] = JsonNode(town->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
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
		auto readIntegerVector = [&](const std::string & field) -> std::vector<int32_t>
		{
			if(!hasField(action, field) || !action[field].isVector())
				throw std::invalid_argument("Missing or non-vector script action field: " + field);
			std::vector<int32_t> result;
			result.reserve(action[field].Vector().size());
			for(size_t index = 0; index < action[field].Vector().size(); ++index)
				result.push_back(readIntegerValue(action[field].Vector()[index], field + "[" + std::to_string(index) + "]"));
			if(result.empty())
				throw std::invalid_argument("Market trade vector field must not be empty: " + field);
			return result;
		};

		TradeItemSell sell;
		TradeItemBuy buy;
		ui32 amount = 1;
		std::vector<TradeItemSell> sells;
		std::vector<TradeItemBuy> buys;
		std::vector<ui32> amounts;
		bool bulkTrade = false;
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
			if(hasField(action, "artifact_instance_ids"))
			{
				for(const int32_t artifactInstanceID : readIntegerVector("artifact_instance_ids"))
					sells.emplace_back(ArtifactInstanceID(artifactInstanceID));
				bulkTrade = true;
			}
			else
			{
				sell = ArtifactInstanceID(readInteger(action, "artifact_instance_id"));
				buy = GameResID(EGameResID::GOLD);
			}
			break;
		case EMarketMode::CREATURE_EXP:
			requireHero();
			if(hasField(action, "slots") || hasField(action, "amounts"))
			{
				const std::vector<int32_t> slotIDs = readIntegerVector("slots");
				const std::vector<int32_t> amountValues = readIntegerVector("amounts");
				if(slotIDs.size() != amountValues.size())
					throw std::invalid_argument("Market trade slots and amounts must have the same size");

				for(size_t index = 0; index < slotIDs.size(); ++index)
				{
					const SlotID slot(slotIDs[index]);
					if(!slot.validSlot())
						throw std::invalid_argument("Invalid army slot id in slots[" + std::to_string(index) + "]");
					if(!hero->hasStackAtSlot(slot))
						throw std::invalid_argument("No creature stack at sacrifice slot " + std::to_string(slotIDs[index]));
					if(amountValues[index] <= 0)
						throw std::invalid_argument("Market trade amount must be positive in amounts[" + std::to_string(index) + "]");

					sells.emplace_back(slot);
					amounts.push_back(static_cast<ui32>(amountValues[index]));
				}
				bulkTrade = true;
			}
			else
			{
				sell = readValidSlot("slot");
				buy = GameResID(EGameResID::GOLD);
				amount = readPositiveAmount("amount");
			}
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

		if(!bulkTrade)
		{
			sells.push_back(sell);
			buys.push_back(buy);
			amounts.push_back(amount);
		}

		const RequestWaitResult request = submitAndWaitForRequest(typeid(TradeOnMarketplace), CTypeList::getInstance().getTypeID<TradeOnMarketplace>(nullptr), [&]
		{
			cc->trade(object->id, mode, sells, buys, amounts, hero);
		});
		actionResult["market_id"] = JsonNode(object->id.getNum());
		actionResult["mode_id"] = JsonNode(modeID);
		actionResult["mode"] = JsonNode(marketModeName(mode));
		if(hero)
			actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["amount"] = JsonNode(static_cast<int32_t>(amount));
		actionResult["bulk"] = JsonNode(bulkTrade);
		actionResult["itemCount"] = JsonNode(static_cast<int32_t>(sells.size()));
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

	if(type == "nullkiller_answer_query")
		return executeNullkillerQueryAction(action, actionResult);

	if(type == "nullkiller_object_interaction")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const CGObjectInstance * object = cc->getObj(ObjectInstanceID(readInteger(action, "object_id")), false);
		if(!object || !cc->isVisibleFor(object, playerID))
			throw std::invalid_argument("Unknown object or object is not visible to scripted AI");

		const auto * town = dynamic_cast<const CGTownInstance *>(object);
		const bool heroVisitsTown = town && hero->getVisitedTown() == town;
		const bool heroAtObject = object->visitablePos().isValid() && hero->visitablePos() == object->visitablePos();
		if(!heroVisitsTown && !heroAtObject)
			throw std::invalid_argument("Hero must be visiting or standing at the object for Nullkiller object interaction");

		performObjectInteraction(object, NK2AI::HeroPtr(hero, cc.get()));
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["object_id"] = JsonNode(object->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "nullkiller_step")
		return executeNullkillerStepAction(action, actionResult);

	if(type == "nullkiller_pass")
		return executeNullkillerPassAction(action, actionResult);

	if(type == "nullkiller_turn_slice")
		return executeNullkillerTurnSliceAction(action, actionResult);

	if(type == "request_statistic")
	{
		const RequestWaitResult request = submitAndWaitForRequest(typeid(RequestStatistic), CTypeList::getInstance().getTypeID<RequestStatistic>(nullptr), [&]
		{
			cc->requestStatistic();
		});
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Statistic request was rejected by server" : "Statistic request was not realized by server");
		return true;
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

	if(type == "castle_teleport")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const int32_t destinationTownID = hasField(action, "destination_town_id") ? readInteger(action, "destination_town_id") : readInteger(action, "town_id");
		const CGTownInstance * destination = cc->getTown(ObjectInstanceID(destinationTownID));
		if(!destination || destination->tempOwner != playerID)
			throw std::invalid_argument("Unknown destination town or town is not owned by scripted AI");

		const CGTownInstance * source = hero->getVisitedTown();
		if(!source || source->tempOwner != playerID)
			throw std::invalid_argument("Hero must be visiting an owned source town for Castle Gate teleport");
		if(source == destination)
			throw std::invalid_argument("Castle Gate destination must be a different town");
		if(source->getFactionID() != destination->getFactionID())
			throw std::invalid_argument("Castle Gate destination must have the same faction as source town");
		if(!source->hasBuilt(BuildingSubID::CASTLE_GATE))
			throw std::invalid_argument("Source town does not have a Castle Gate");
		if(!destination->hasBuilt(BuildingSubID::CASTLE_GATE))
			throw std::invalid_argument("Destination town does not have a Castle Gate");
		if(destination->getVisitingHero())
			throw std::invalid_argument("Castle Gate destination town already has a visiting hero");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(CastleTeleportHero), CTypeList::getInstance().getTypeID<CastleTeleportHero>(nullptr), [&]
		{
			cc->teleportHero(hero, destination);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["source_town_id"] = JsonNode(source->id.getNum());
		actionResult["destination_town_id"] = JsonNode(destination->id.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Castle Gate teleport request was rejected by server" : "Castle Gate teleport request was not realized by server");
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

	if(type == "spell_research")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Unknown town or town is not owned by scripted AI");
		if(!cc->getSettings().getBoolean(EGameSettings::TOWNS_SPELL_RESEARCH) || !town->spellResearchAllowed)
			throw std::invalid_argument("Spell research is not available for this town");

		const SpellID spellID(readInteger(action, "spell_id"));
		int32_t levelIndex = -1;
		int32_t spellPosition = -1;
		for(size_t index = 0; index < town->spells.size(); ++index)
		{
			const int32_t found = vstd::find_pos(town->spells[index], spellID);
			if(found != -1)
			{
				levelIndex = static_cast<int32_t>(index);
				spellPosition = found;
				break;
			}
		}
		if(levelIndex < 0)
			throw std::invalid_argument("Spell is not present in this town mage guild");

		const int32_t visibleSpellCount = std::clamp<int32_t>(
			town->spellsAtLevel(levelIndex, false),
			0,
			static_cast<int32_t>(town->spells[levelIndex].size()));
		if(spellPosition >= visibleSpellCount)
			throw std::invalid_argument("Spell research can only replace currently visible mage guild spells");
		if(visibleSpellCount >= static_cast<int32_t>(town->spells[levelIndex].size()))
			throw std::invalid_argument("No replacement spell is available for this mage guild level");

		const auto & perDay = cc->getSettings().getValue(EGameSettings::TOWNS_SPELL_RESEARCH_PER_DAY).Vector();
		const auto & baseCosts = cc->getSettings().getValue(EGameSettings::TOWNS_SPELL_RESEARCH_COST).Vector();
		const auto & researchMultipliers = cc->getSettings().getValue(EGameSettings::TOWNS_SPELL_RESEARCH_COST_MULTIPLIER_PER_RESEARCH).Vector();
		const auto & rerollMultipliers = cc->getSettings().getValue(EGameSettings::TOWNS_SPELL_RESEARCH_COST_MULTIPLIER_PER_REROLL).Vector();
		const size_t level = static_cast<size_t>(levelIndex);
		if(level >= perDay.size() || level >= baseCosts.size() || level >= researchMultipliers.size() || level >= rerollMultipliers.size())
			throw std::invalid_argument("Spell research settings are missing for this mage guild level");
		if(town->spellResearchCounterDay >= perDay[level].Float())
			throw std::invalid_argument("Spell research daily limit is already reached");
		if(level >= town->spellResearchPendingRerollsCounters.size())
			throw std::invalid_argument("Spell research reroll state is missing for this mage guild level");

		ResourceSet costBase;
		costBase.resolveFromJson(baseCosts[level]);
		const double pastResearchesMultiplier = std::pow(researchMultipliers[level].Float(), town->spellResearchAcceptedCounter);
		const double pastRerollsMultiplier = std::pow(rerollMultipliers[level].Float(), town->spellResearchPendingRerollsCounters[level]);
		const ResourceSet cost = costBase.multipliedBy(pastResearchesMultiplier * pastRerollsMultiplier);
		if(!cc->getResourceAmount().canAfford(cost))
			throw std::invalid_argument("Spell research cannot be afforded");

		const bool accepted = hasField(action, "accepted") ? readBool(action, "accepted", true) : readBool(action, "accept", true);
		const RequestWaitResult request = submitAndWaitForRequest(typeid(SpellResearch), CTypeList::getInstance().getTypeID<SpellResearch>(nullptr), [&]
		{
			cc->spellResearch(town, spellID, accepted);
		});
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["spell_id"] = JsonNode(spellID.getNum());
		actionResult["replacement_spell_id"] = JsonNode(town->spells[level][visibleSpellCount].getNum());
		actionResult["accept"] = JsonNode(accepted);
		actionResult["cost"] = jsonResources(cost);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Spell research request was rejected by server" : "Spell research request was not realized by server");
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

	if(type == "visit_town_building")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Unknown town or town is not owned by scripted AI");

		const BuildingID buildingID(readInteger(action, "building_id"));
		if(!town->hasBuilt(buildingID))
			throw std::invalid_argument("Town building is not built");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(VisitTownBuilding), CTypeList::getInstance().getTypeID<VisitTownBuilding>(nullptr), [&]
		{
			cc->visitTownBuilding(town, buildingID);
		});
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["building_id"] = JsonNode(buildingID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Town building visit request was rejected by server" : "Town building visit request was not realized by server");
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
		const int32_t sourceID = hasField(action, "source_id")
			? readInteger(action, "source_id")
			: (hasField(action, "tavern_id") ? readInteger(action, "tavern_id") : readInteger(action, "town_id"));
		const CGObjectInstance * source = cc->getObj(ObjectInstanceID(sourceID), false);
		if(!source)
			throw std::invalid_argument("Unknown hero hiring source");

		const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(source);
		if(town)
		{
			if(town->tempOwner != playerID)
				throw std::invalid_argument("Hero hiring town is not owned by scripted AI");
			if(town->getVisitingHero())
				throw std::invalid_argument("Town has a visiting hero and cannot hire another hero");
		}
		else if(source->ID == Obj::TAVERN)
		{
			if(!cc->isVisibleFor(source, playerID))
				throw std::invalid_argument("Adventure tavern is not visible to scripted AI");
		}
		else
		{
			throw std::invalid_argument("Hero hiring source must be an owned town or visible adventure tavern");
		}
		if(cc->getResourceAmount()[EGameResID::GOLD] < GameConstants::HERO_GOLD_COST)
			throw std::invalid_argument("Not enough gold to hire a hero");

		const HeroTypeID heroTypeID(readInteger(action, "hero_type_id"));
		const HeroTypeID nextHeroTypeID = hasField(action, "next_hero_type_id")
			? HeroTypeID(readInteger(action, "next_hero_type_id"))
			: HeroTypeID::NONE;
		const CGHeroInstance * heroToHire = nullptr;
		for(const CGHeroInstance * availableHero : cc->getAvailableHeroes(source))
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
			cc->recruitHero(source, heroToHire, nextHeroTypeID);
		});
		actionResult["source_id"] = JsonNode(source->id.getNum());
		if(town)
			actionResult["town_id"] = JsonNode(town->id.getNum());
		else
			actionResult["tavern_id"] = JsonNode(source->id.getNum());
		actionResult["hero_type_id"] = JsonNode(heroTypeID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Hire hero request was rejected by server" : "Hire hero request was not realized by server");
		return true;
	}

	if(type == "transfer_army" || type == "bulk_move_army")
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

	if(type == "swap_creatures" || type == "merge_stacks" || type == "merge_or_swap_stacks" || type == "split_stack")
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
		const bool mergeOrSwapWillMerge = type == "merge_or_swap_stacks"
			&& destination->hasStackAtSlot(destinationSlot)
			&& source->getCreature(sourceSlot) == destination->getCreature(destinationSlot);

		const RequestWaitResult request = submitAndWaitForRequest(typeid(ArrangeStacks), CTypeList::getInstance().getTypeID<ArrangeStacks>(nullptr), [&]
		{
			if(type == "swap_creatures")
				cc->swapCreatures(source, destination, sourceSlot, destinationSlot);
			else if(type == "merge_stacks")
				cc->mergeStacks(source, destination, sourceSlot, destinationSlot);
			else if(type == "merge_or_swap_stacks")
				cc->mergeOrSwapStacks(source, destination, sourceSlot, destinationSlot);
			else
				cc->splitStack(source, destination, sourceSlot, destinationSlot, amount);
		});
		actionResult["source_id"] = JsonNode(source->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["source_slot"] = JsonNode(sourceSlot.getNum());
		actionResult["destination_slot"] = JsonNode(destinationSlot.getNum());
		if(type == "split_stack")
			actionResult["amount"] = JsonNode(amount);
		if(type == "merge_or_swap_stacks")
			actionResult["resolved_operation"] = JsonNode(mergeOrSwapWillMerge ? "merge" : "swap");
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

	if(type == "set_town_name")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Unknown town or town is not owned by scripted AI");
		std::string name = readString(action, "name");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(SetTownName), CTypeList::getInstance().getTypeID<SetTownName>(nullptr), [&]
		{
			cc->setTownName(town, name);
		});
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["name"] = JsonNode(name);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Set town name request was rejected by server" : "Set town name request was not realized by server");
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

	if(type == "pick_best_creatures")
	{
		const CArmedInstance * destination = readOwnedArmy("destination_id", "destination");
		const CArmedInstance * source = readOwnedArmy("source_id", "source");
		if(destination == source)
			throw std::invalid_argument("Creature preparation source and destination must be different armies");
		if(destination->visitablePos() != source->visitablePos())
			throw std::invalid_argument("Creature preparation armies must be co-located");

		pickBestCreatures(destination, source);
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["source_id"] = JsonNode(source->id.getNum());
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(true);
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

	if(type == "prepare_hero")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");

		const bool includeArtifacts = readBool(action, "include_artifacts", true);
		const bool includeCreatures = readBool(action, "include_creatures", true);
		if(!includeArtifacts && !includeCreatures)
			throw std::invalid_argument("Hero preparation must include artifacts or creatures");
		const CGHeroInstance * otherHero = nullptr;
		if(hasField(action, "other_hero_id"))
		{
			otherHero = cc->getHero(ObjectInstanceID(readInteger(action, "other_hero_id")));
			if(!otherHero || otherHero->tempOwner != playerID)
				throw std::invalid_argument("Unknown other hero or other hero is not owned by scripted AI");
			if(hero == otherHero)
				throw std::invalid_argument("Hero preparation other hero must differ from target hero");
			if(hero->visitablePos() != otherHero->visitablePos())
				throw std::invalid_argument("Heroes must be co-located for hero preparation");
		}

		const CArmedInstance * source = nullptr;
		if(hasField(action, "source_id"))
		{
			source = readOwnedArmy("source_id", "preparation source");
			if(source == hero)
				throw std::invalid_argument("Hero preparation source must differ from target hero");
			if(hero->visitablePos() != source->visitablePos())
				throw std::invalid_argument("Hero preparation source must be co-located with target hero");
		}
		else if(otherHero)
		{
			source = otherHero;
		}
		else if(const CGTownInstance * town = hero->getVisitedTown())
		{
			if(town->tempOwner == playerID)
				source = town;
		}

		if(includeCreatures)
		{
			if(!source)
				throw std::invalid_argument("Hero creature preparation requires source_id, other_hero_id, or a visited owned town");
			pickBestCreatures(hero, source);
		}

		if(includeArtifacts)
			AIGateway::pickBestArtifacts(cc, hero, otherHero);

		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["include_artifacts"] = JsonNode(includeArtifacts);
		actionResult["include_creatures"] = JsonNode(includeCreatures);
		if(source)
			actionResult["source_id"] = JsonNode(source->id.getNum());
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
		const auto validateArtifactHolder = [&](const ArtifactLocation & location, const std::string & label, bool allowFirstAvailable) -> const CGObjectInstance *
		{
			const CArtifactSet * holder = cc->getArtSet(location);
			const CGObjectInstance * object = cc->getObj(location.artHolder, false);
			const IMarket * market = object ? dynamic_cast<const IMarket *>(object) : nullptr;
			const bool ownedHolder = object && object->tempOwner == playerID;
			const bool visibleAltarHolder = object && market && market->allowsTrade(EMarketMode::ARTIFACT_EXP) && cc->isVisibleFor(object, playerID);
			if(!holder || !object || (!ownedHolder && !visibleAltarHolder))
				throw std::invalid_argument("Artifact " + label + " holder is unknown, hidden, or unavailable to scripted AI");
			const bool firstAvailableDestination = location.slot == ArtifactPosition::FIRST_AVAILABLE && allowFirstAvailable;
			if(!firstAvailableDestination && !ArtifactUtils::checkIfSlotValid(*holder, location.slot))
				throw std::invalid_argument("Artifact " + label + " slot is invalid for holder");
			return object;
		};
		const CGObjectInstance * srcObject = validateArtifactHolder(src, "source", false);
		const CGObjectInstance * dstObject = validateArtifactHolder(dst, "destination", true);

		const IMarket * srcMarket = dynamic_cast<const IMarket *>(srcObject);
		const IMarket * dstMarket = dynamic_cast<const IMarket *>(dstObject);
		if(srcMarket || dstMarket)
		{
			if(srcMarket && dstMarket)
				throw std::invalid_argument("Artifact exchange cannot move directly between two market altars");
			const CGObjectInstance * marketObject = srcMarket ? srcObject : dstObject;
			const CGHeroInstance * hero = dynamic_cast<const CGHeroInstance *>(srcMarket ? dstObject : srcObject);
			if(!hero || hero->tempOwner != playerID)
				throw std::invalid_argument("Artifact altar exchange requires an owned hero");
			if(!heroCanUseMarketAltar(hero, marketObject))
				throw std::invalid_argument("Hero must be visiting the market altar for artifact exchange");
		}

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

	if(type == "erase_transition_artifact")
	{
		const CGHeroInstance * hero = cc->getHero(ObjectInstanceID(readInteger(action, "hero_id")));
		if(!hero || hero->tempOwner != playerID)
			throw std::invalid_argument("Unknown hero or hero is not owned by scripted AI");
		const CArtifactInstance * artifact = hero->getArt(ArtifactPosition::TRANSITION_POS);
		if(!artifact)
			throw std::invalid_argument("Hero has no artifact in transition slot");
		if(artifact->canBePutAt(hero))
			throw std::invalid_argument("Only illegal transition-slot artifacts can be erased");

		const ArtifactLocation location(hero->id, ArtifactPosition::TRANSITION_POS);
		const RequestWaitResult request = submitAndWaitForRequest(typeid(EraseArtifactByClient), CTypeList::getInstance().getTypeID<EraseArtifactByClient>(nullptr), [&]
		{
			cc->eraseArtifactByClient(location);
		});
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["location"] = jsonArtifactLocation(location);
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Erase transition artifact request was rejected by server" : "Erase transition artifact request was not realized by server");
		return true;
	}

	if(type == "bulk_move_artifacts")
	{
		const ObjectInstanceID srcID(hasField(action, "src_id") ? readInteger(action, "src_id") : readInteger(action, "src_hero_id"));
		const ObjectInstanceID dstID(hasField(action, "dst_id") ? readInteger(action, "dst_id") : readInteger(action, "dst_hero_id"));
		const CGObjectInstance * srcObject = cc->getObj(srcID, false);
		const CGObjectInstance * dstObject = cc->getObj(dstID, false);
		if(!srcObject || !dstObject || !cc->getArtSet(ArtifactLocation(srcID)) || !cc->getArtSet(ArtifactLocation(dstID)))
			throw std::invalid_argument("Artifact bulk move source or destination is unknown, hidden, or not an artifact holder");

		const CGHeroInstance * srcHero = dynamic_cast<const CGHeroInstance *>(srcObject);
		const CGHeroInstance * dstHero = dynamic_cast<const CGHeroInstance *>(dstObject);
		const IMarket * srcMarket = dynamic_cast<const IMarket *>(srcObject);
		const IMarket * dstMarket = dynamic_cast<const IMarket *>(dstObject);
		const bool swap = readBool(action, "swap", false);
		const bool equipped = readBool(action, "equipped", true);
		const bool backpack = readBool(action, "backpack", true);

		if(srcMarket || dstMarket)
		{
			if(srcMarket && dstMarket)
				throw std::invalid_argument("Artifact bulk move cannot move directly between two market altars");
			if(swap)
				throw std::invalid_argument("Artifact altar bulk move does not support swapping");
			const CGObjectInstance * marketObject = srcMarket ? srcObject : dstObject;
			const IMarket * market = srcMarket ? srcMarket : dstMarket;
			const CGHeroInstance * hero = srcMarket ? dstHero : srcHero;
			if(!market->allowsTrade(EMarketMode::ARTIFACT_EXP))
				throw std::invalid_argument("Market does not provide an artifact sacrifice altar");
			if(!hero || hero->tempOwner != playerID)
				throw std::invalid_argument("Artifact altar bulk move requires an owned hero");
			if(!heroCanUseMarketAltar(hero, marketObject))
				throw std::invalid_argument("Hero must be visiting the market altar for artifact bulk movement");
		}
		else
		{
			if(!srcHero || !dstHero || srcHero->tempOwner != playerID || dstHero->tempOwner != playerID)
				throw std::invalid_argument("Artifact bulk move heroes must both be owned by scripted AI");
			if(srcHero->visitablePos() != dstHero->visitablePos())
				throw std::invalid_argument("Heroes must be co-located for artifact bulk movement");
		}

		const RequestWaitResult request = submitAndWaitForRequest(typeid(BulkExchangeArtifacts), CTypeList::getInstance().getTypeID<BulkExchangeArtifacts>(nullptr), [&]
		{
			cc->bulkMoveArtifacts(srcID, dstID, swap, equipped, backpack);
		});
		actionResult["src_id"] = JsonNode(srcID.getNum());
		actionResult["dst_id"] = JsonNode(dstID.getNum());
		if(srcHero)
			actionResult["src_hero_id"] = JsonNode(srcHero->id.getNum());
		if(dstHero)
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

	if(type == "cancel_query")
	{
		const QueryID queryID(readInteger(action, "query_id"));
		const auto query = getScriptQuery(queryID);
		if(!query || !hasField(*query, "cancelAction") || readString((*query)["cancelAction"], "type") != "cancel_query")
			throw std::invalid_argument("Query does not support optional cancel reply");

		const RequestWaitResult request = submitAndWaitForRequest(typeid(QueryReply), CTypeList::getInstance().getTypeID<QueryReply>(nullptr), [&]
		{
			cc->sendQueryReply(std::nullopt, queryID);
		});
		actionResult["query_id"] = JsonNode(queryID.getNum());
		actionResult["request"] = jsonRequestWaitResult(request);
		if(!waitTillFreeForScriptAction(actionResult, type))
			return false;
		if(request.applied)
			removeScriptQuery(queryID);
		actionResult["ok"] = JsonNode(request.applied);
		if(!request.applied)
			actionResult["error"] = JsonNode(request.realized ? "Cancel query request was rejected by server" : "Cancel query request was not realized by server");
		return true;
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
		removeScriptQuery(queryID);
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
		if(request.applied)
			status.madeTurn();
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

	const Calendar calendar = cc->getCalendar();
	state["day"] = JsonNode(calendar.getCurrentDay());
	state["dayOfWeek"] = JsonNode(calendar.getDayOfWeek());
	state["week"] = JsonNode(calendar.getWeek());
	state["month"] = JsonNode(calendar.getMonth());
	state["calendar"] = jsonCalendar(calendar);
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
	state["grail"] = jsonGrailInfo(cc);

	if(const PlayerState * playerState = cc->getPlayerState(playerID, false))
	{
		const auto ownStatus = playerState->status;
		state["player"]["team"] = JsonNode(playerState->team.getNum());
		state["player"]["teamId"] = JsonNode(playerState->team.getNum());
		state["player"]["status"] = JsonNode(static_cast<int32_t>(ownStatus));
		state["player"]["statusId"] = JsonNode(static_cast<int32_t>(ownStatus));
		state["player"]["statusName"] = JsonNode(playerStatusName(ownStatus));
		state["resources"] = jsonResources(playerState->resources);
	}
	else
	{
		state["resources"] = jsonResources(cc->getResourceAmount());
	}

	state["players"].Vector();
	for(PlayerColor color : PlayerColor::ALL_PLAYERS())
	{
		const auto playerStatus = cc->getPlayerStatus(color, false);
		if(playerStatus == EPlayerStatus::WRONG)
			continue;

		const auto relation = cc->getPlayerRelations(playerID, color);
		const PlayerState * playerState = cc->getPlayerState(color, false);
		JsonNode playerNode;
		playerNode["id"] = JsonNode(color.getNum());
		playerNode["color"] = JsonNode(jsonPlayerColor(color));
		playerNode["statusId"] = JsonNode(static_cast<int32_t>(playerStatus));
		playerNode["status"] = JsonNode(playerStatusName(playerStatus));
		playerNode["relationId"] = JsonNode(static_cast<int32_t>(relation));
		playerNode["relation"] = JsonNode(playerRelationName(relation));
		playerNode["self"] = JsonNode(relation == PlayerRelations::SAME_PLAYER);
		playerNode["ally"] = JsonNode(relation == PlayerRelations::ALLIES || relation == PlayerRelations::SAME_PLAYER);
		playerNode["enemy"] = JsonNode(relation == PlayerRelations::ENEMIES);
		playerNode["detailsVisible"] = JsonNode(playerState != nullptr);
		if(playerState)
		{
			playerNode["teamId"] = JsonNode(playerState->team.getNum());
			playerNode["human"] = JsonNode(playerState->human);
		}
		state["players"].Vector().push_back(playerNode);
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
			state["towns"].Vector().push_back(jsonTown(town, resources, true));
	}

	state["ownedObjects"].Vector();
	for(const CGObjectInstance * object : cc->getMyObjects())
	{
		if(object && object->tempOwner == playerID)
			state["ownedObjects"].Vector().push_back(jsonMapObject(object, playerID, nullptr));
	}
	state["ownedObjectCount"] = JsonNode(static_cast<int32_t>(state["ownedObjects"].Vector().size()));

	state["quests"].Vector();
	for(const QuestInfo & questInfo : cc->getMyQuests())
		state["quests"].Vector().push_back(jsonQuestInfo(questInfo, cc.get(), playerID));
	state["questCount"] = JsonNode(static_cast<int32_t>(state["quests"].Vector().size()));

	const std::vector<int3> visibleTiles = visibleMapTiles(cc, playerID);
	const size_t totalMapTiles = static_cast<size_t>(mapSize.x) * static_cast<size_t>(mapSize.y) * static_cast<size_t>(mapSize.z);
	const size_t tilesPerLevel = static_cast<size_t>(mapSize.x) * static_cast<size_t>(mapSize.y);
	std::vector<int32_t> exploredTilesByLevel(static_cast<size_t>(mapSize.z), 0);
	int32_t exploredLandTiles = 0;
	int32_t exploredWaterTiles = 0;
	int32_t exploredRockTiles = 0;
	int32_t exploredPassableTiles = 0;
	int32_t exploredBlockedTiles = 0;
	int32_t exploredVisitableTiles = 0;
	int32_t exploredRoadTiles = 0;
	std::map<int32_t, int32_t> visibleObjectKindCounts;
	std::map<int32_t, int32_t> visibleObjectControlCounts;
	std::map<int32_t, int32_t> visibleObjectOwnerCounts;

	state["map"]["totalTiles"] = JsonNode(static_cast<int32_t>(totalMapTiles));
	state["map"]["visibleTilesCount"] = JsonNode(static_cast<int32_t>(visibleTiles.size()));
	state["map"]["visibleTileSampleLimit"] = JsonNode(static_cast<int32_t>(visibleTiles.size()));
	state["map"]["visibleTilesComplete"] = JsonNode(true);
	state["map"]["visibleTiles"].Vector();
	state["map"]["visibleObjects"].Vector();

	std::set<int32_t> seenVisibleObjects;
	size_t visibleObjectCount = 0;
	for(const int3 & position : visibleTiles)
	{
		const TerrainTile * tile = cc->getTile(position, false);
		if(!tile)
			continue;

		if(position.z >= 0 && position.z < mapSize.z)
			++exploredTilesByLevel[static_cast<size_t>(position.z)];
		const TerrainType * terrain = tile->getTerrain();
		if(terrain)
		{
			if(terrain->isRock())
				++exploredRockTiles;
			else if(terrain->isWater())
				++exploredWaterTiles;
			else
				++exploredLandTiles;
			if(terrain->isPassable())
				++exploredPassableTiles;
		}
		if(tile->blocked())
			++exploredBlockedTiles;
		if(tile->visitable())
			++exploredVisitableTiles;
		if(tile->hasRoad())
			++exploredRoadTiles;

		state["map"]["visibleTiles"].Vector().push_back(jsonVisibleTile(position, *tile, cc, playerID));

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
			const ScriptObjectKind objectKind = scriptObjectKind(object->ID);
			const ScriptObjectControlKind controlKind = scriptObjectControlKind(cc, playerID, object->tempOwner);
			++visibleObjectKindCounts[static_cast<int32_t>(objectKind)];
			++visibleObjectControlCounts[static_cast<int32_t>(controlKind)];
			++visibleObjectOwnerCounts[object->tempOwner.getNum()];
			state["map"]["visibleObjects"].Vector().push_back(jsonMapObject(object, playerID, nullptr));
		};

		for(ObjectInstanceID objectID : tile->visitableObjects)
			appendVisibleObject(objectID);
		for(ObjectInstanceID objectID : tile->blockingObjects)
			appendVisibleObject(objectID);
	}
	state["map"]["visibleObjectsCount"] = JsonNode(static_cast<int32_t>(visibleObjectCount));
	state["map"]["visibleObjectLimit"] = JsonNode(static_cast<int32_t>(visibleObjectCount));
	state["map"]["visibleObjectsComplete"] = JsonNode(true);
	state["map"]["visibleObjectsTruncated"] = JsonNode(false);
	state["map"]["visibleTilesTruncated"] = JsonNode(false);
	state["map"]["exploredTilesCount"] = JsonNode(static_cast<int32_t>(visibleTiles.size()));
	state["map"]["exploredRatio"].Float() = totalMapTiles > 0 ? static_cast<double>(visibleTiles.size()) / static_cast<double>(totalMapTiles) : 0.0;
	state["map"]["exploredLandTilesCount"] = JsonNode(exploredLandTiles);
	state["map"]["exploredWaterTilesCount"] = JsonNode(exploredWaterTiles);
	state["map"]["exploredRockTilesCount"] = JsonNode(exploredRockTiles);
	state["map"]["exploredPassableTilesCount"] = JsonNode(exploredPassableTiles);
	state["map"]["exploredBlockedTilesCount"] = JsonNode(exploredBlockedTiles);
	state["map"]["exploredVisitableTilesCount"] = JsonNode(exploredVisitableTiles);
	state["map"]["exploredRoadTilesCount"] = JsonNode(exploredRoadTiles);
	state["map"]["exploredByLevel"].Vector();
	for(int32_t z = 0; z < mapSize.z; ++z)
	{
		JsonNode levelNode;
		levelNode["level"] = JsonNode(z);
		levelNode["totalTiles"] = JsonNode(static_cast<int32_t>(tilesPerLevel));
		levelNode["exploredTilesCount"] = JsonNode(exploredTilesByLevel[static_cast<size_t>(z)]);
		levelNode["exploredRatio"].Float() = tilesPerLevel > 0 ? static_cast<double>(exploredTilesByLevel[static_cast<size_t>(z)]) / static_cast<double>(tilesPerLevel) : 0.0;
		state["map"]["exploredByLevel"].Vector().push_back(levelNode);
	}

	JsonNode & control = state["map"]["visibleControl"];
	control["objectCountsByKind"].Vector();
	for(const auto & [kindID, count] : visibleObjectKindCounts)
	{
		JsonNode countNode;
		countNode["kindId"] = JsonNode(kindID);
		countNode["kind"] = JsonNode(scriptObjectKindName(static_cast<ScriptObjectKind>(kindID)));
		countNode["count"] = JsonNode(count);
		control["objectCountsByKind"].Vector().push_back(countNode);
	}
	control["objectCountsByControl"].Vector();
	for(const auto & [controlID, count] : visibleObjectControlCounts)
	{
		JsonNode countNode;
		countNode["controlId"] = JsonNode(controlID);
		countNode["control"] = JsonNode(scriptObjectControlKindName(static_cast<ScriptObjectControlKind>(controlID)));
		countNode["count"] = JsonNode(count);
		control["objectCountsByControl"].Vector().push_back(countNode);
	}
	control["objectCountsByOwner"].Vector();
	for(const auto & [ownerID, count] : visibleObjectOwnerCounts)
	{
		const PlayerColor owner(ownerID);
		JsonNode countNode;
		countNode["ownerId"] = JsonNode(ownerID);
		countNode["owner"] = JsonNode(jsonPlayerColor(owner));
		countNode["count"] = JsonNode(count);
		control["objectCountsByOwner"].Vector().push_back(countNode);
	}
	return state;
}

JsonNode CScriptedAdventureAI::makeScriptActionSpace() const
{
	JsonNode actionSpace;
	actionSpace["acceptedActionTypes"].Vector();
	actionSpace["acceptedActions"].Vector();
	for(const auto & [typeId, type] : scriptActionTypeRegistry())
	{
		actionSpace["acceptedActionTypes"].Vector().push_back(JsonNode(type));
		JsonNode accepted;
		accepted["type"] = JsonNode(type);
		accepted["typeId"] = JsonNode(typeId);
		actionSpace["acceptedActions"].Vector().push_back(accepted);
	}

	actionSpace["buildOptions"].Vector();
	actionSpace["recruitOptions"].Vector();
	actionSpace["hireHeroOptions"].Vector();
	actionSpace["prepareHeroOptions"].Vector();
	actionSpace["armyTransferOptions"].Vector();
	actionSpace["upgradeCreatureOptions"].Vector();
	actionSpace["reachableObjects"].Vector();
	actionSpace["movementOptions"].Vector();
	actionSpace["shipyardOptions"].Vector();
	actionSpace["castleTeleportOptions"].Vector();
	actionSpace["digOptions"].Vector();
	actionSpace["adventureSpellOptions"].Vector();
	actionSpace["buyArtifactOptions"].Vector();
	actionSpace["spellResearchOptions"].Vector();
	actionSpace["visitTownBuildingOptions"].Vector();
	actionSpace["questObjectOptions"].Vector();
	actionSpace["questObjectOptionsTruncated"] = JsonNode(false);
	actionSpace["nullkillerSubroutineOptions"].Vector();
	actionSpace["nullkillerHelperOptions"].Vector();
	actionSpace["recommendedActions"].Vector();

	for(const NK2AI::ScriptTaskSearchMode mode : {
		NK2AI::ScriptTaskSearchMode::STARTUP,
		NK2AI::ScriptTaskSearchMode::PRIORITY,
		NK2AI::ScriptTaskSearchMode::ADVENTURE,
		NK2AI::ScriptTaskSearchMode::RECRUIT_HERO,
		NK2AI::ScriptTaskSearchMode::BUY_ARMY,
		NK2AI::ScriptTaskSearchMode::BUILDING,
		NK2AI::ScriptTaskSearchMode::CAPTURE,
		NK2AI::ScriptTaskSearchMode::CLUSTER,
		NK2AI::ScriptTaskSearchMode::DEFENSE,
		NK2AI::ScriptTaskSearchMode::ESCAPE,
		NK2AI::ScriptTaskSearchMode::GATHER_ARMY,
		NK2AI::ScriptTaskSearchMode::EXPLORATION,
		NK2AI::ScriptTaskSearchMode::ALL
	})
		actionSpace["nullkillerSubroutineOptions"].Vector().push_back(jsonNullkillerSubroutineOption(mode, 4, 16, 16));

	actionSpace["nullkillerHelperOptions"].Vector().push_back(jsonNullkillerPriorityPassOption());
	actionSpace["nullkillerHelperOptions"].Vector().push_back(jsonNullkillerResourceTradeOption());
	actionSpace["nullkillerHelperOptions"].Vector().push_back(jsonNullkillerOptimizeArtifactsOption());
	actionSpace["nullkillerHelperOptions"].Vector().push_back(jsonNullkillerTurnSliceOption());
	actionSpace["nullkillerHelperOptions"].Vector().push_back(jsonNullkillerResetOption());

	std::shared_lock gameStateLock(CGameState::mutex);
	const ResourceSet resources = cc->getResourceAmount();
	const std::vector<int3> visibleTiles = visibleMapTiles(cc, playerID);

	std::set<int32_t> seenNullkillerUpgradeArmies;
	std::set<int32_t> seenNullkillerRecruitSources;
	std::set<std::tuple<int32_t, int32_t, int32_t>> seenNullkillerFormationHelpers;
	auto appendNullkillerBuildArmyHelperOption = [&](const CGTownInstance * town)
	{
		if(!town || town->tempOwner != playerID || !cc->isVisibleFor(town, playerID))
			return;

		JsonNode option;
		option["helperKindId"] = JsonNode(3);
		option["helperKind"] = JsonNode("town_army");
		option["bounded"] = JsonNode(true);
		option["delegatesRestOfDay"] = JsonNode(false);
		option["town_id"] = JsonNode(town->id.getNum());
		option["town"] = jsonTown(town, resources, true);
		setScriptActionType(option["planAction"], "nullkiller_build_army");
		option["planAction"]["town_id"] = option["town_id"];
		actionSpace["nullkillerHelperOptions"].Vector().push_back(option);
	};
	auto appendNullkillerUpgradeHelperOption = [&](const CArmedInstance * army)
	{
		if(!army || army->tempOwner != playerID || !cc->isVisibleFor(army, playerID))
			return;
		if(!seenNullkillerUpgradeArmies.insert(army->id.getNum()).second)
			return;

		JsonNode option;
		option["helperKindId"] = JsonNode(4);
		option["helperKind"] = JsonNode("upgrade_army");
		option["bounded"] = JsonNode(true);
		option["delegatesRestOfDay"] = JsonNode(false);
		option["army_id"] = JsonNode(army->id.getNum());
		option["ownedArmy"] = jsonOwnedArmySnapshot(army);
		setScriptActionType(option["planAction"], "nullkiller_upgrade_army");
		option["planAction"]["army_id"] = option["army_id"];
		actionSpace["nullkillerHelperOptions"].Vector().push_back(option);
	};
	auto appendNullkillerRecruitHelperOption = [&](const CGDwelling * source, const CArmedInstance * destination)
	{
		if(!source || source->tempOwner != playerID || !cc->isVisibleFor(source, playerID))
			return;
		if(!seenNullkillerRecruitSources.insert(source->id.getNum()).second)
			return;
		if(destination && (destination->tempOwner != playerID || !cc->isVisibleFor(destination, playerID)))
			return;

		JsonNode option;
		option["helperKindId"] = JsonNode(5);
		option["helperKind"] = JsonNode("recruit_creatures");
		option["bounded"] = JsonNode(true);
		option["delegatesRestOfDay"] = JsonNode(false);
		option["source_id"] = JsonNode(source->id.getNum());
		option["sourceObject"] = jsonMapObject(source, playerID, nullptr);
		if(destination)
		{
			option["destination_id"] = JsonNode(destination->id.getNum());
			option["destinationArmy"] = jsonOwnedArmySnapshot(destination);
		}
		setScriptActionType(option["planAction"], "nullkiller_recruit_creatures");
		option["planAction"]["source_id"] = option["source_id"];
		if(destination)
			option["planAction"]["destination_id"] = option["destination_id"];
		actionSpace["nullkillerHelperOptions"].Vector().push_back(option);
	};
	auto appendNullkillerMoveCreaturesToHeroHelperOption = [&](const CGTownInstance * town)
	{
		if(!town || town->tempOwner != playerID || !cc->isVisibleFor(town, playerID))
			return;

		const CGHeroInstance * visitingHero = town->getVisitingHero();
		if(!visitingHero || visitingHero->tempOwner != playerID || !town->armedGarrison())
			return;

		JsonNode option;
		option["helperKindId"] = JsonNode(9);
		option["helperKind"] = JsonNode("town_creatures_to_hero");
		option["bounded"] = JsonNode(true);
		option["delegatesRestOfDay"] = JsonNode(false);
		option["town_id"] = JsonNode(town->id.getNum());
		option["hero_id"] = JsonNode(visitingHero->id.getNum());
		option["town"] = jsonTown(town, resources, true);
		option["hero"] = jsonHero(visitingHero);
		setScriptActionType(option["planAction"], "nullkiller_move_creatures_to_hero");
		option["planAction"]["town_id"] = option["town_id"];
		actionSpace["nullkillerHelperOptions"].Vector().push_back(option);
	};
	auto appendNullkillerDismissWeakHeroHelperOption = [&]()
	{
		if(!nullkiller || !nullkiller->heroManager)
			return;

		ObjectInstanceID selectedHeroID = ObjectInstanceID::NONE;
		{
			std::unique_lock aiLock(nullkiller->aiStateMutex);
			nullkiller->heroManager->update();
			if(!nullkiller->heroManager->heroCapReached())
				return;
			if(const CGHeroInstance * selectedHero = nullkiller->heroManager->findWeakHeroToDismiss(std::numeric_limits<uint64_t>::max()))
				selectedHeroID = selectedHero->id;
		}
		if(selectedHeroID == ObjectInstanceID::NONE)
			return;

		const CGHeroInstance * hero = cc->getHero(selectedHeroID);
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			return;

		JsonNode option;
		option["helperKindId"] = JsonNode(10);
		option["helperKind"] = JsonNode("dismiss_weak_hero");
		option["bounded"] = JsonNode(true);
		option["delegatesRestOfDay"] = JsonNode(false);
		option["hero_id"] = JsonNode(hero->id.getNum());
		option["hero"] = jsonHero(hero);
		setScriptActionType(option["planAction"], "nullkiller_dismiss_weak_hero");
		actionSpace["nullkillerHelperOptions"].Vector().push_back(option);
	};
	auto appendNullkillerFormationHelperOption = [&](
		const CGHeroInstance * hero,
		const CGTownInstance * town,
		int32_t helperKindID,
		const std::string & helperKind,
		const std::string & actionType)
	{
		if(!hero || hero->tempOwner != playerID || !cc->isVisibleFor(hero, playerID))
			return;
		if(town)
		{
			if(!cc->isVisibleFor(town, playerID))
				return;
			if(cc->getPlayerRelations(playerID, town->tempOwner) != PlayerRelations::ENEMIES)
				return;
		}

		const int32_t townID = town ? town->id.getNum() : -1;
		if(!seenNullkillerFormationHelpers.emplace(hero->id.getNum(), townID, helperKindID).second)
			return;

		JsonNode option;
		option["helperKindId"] = JsonNode(helperKindID);
		option["helperKind"] = JsonNode(helperKind);
		option["bounded"] = JsonNode(true);
		option["delegatesRestOfDay"] = JsonNode(false);
		option["hero_id"] = JsonNode(hero->id.getNum());
		option["hero"] = jsonHero(hero);
		if(town)
		{
			option["town_id"] = JsonNode(town->id.getNum());
			option["townObject"] = jsonMapObject(town, playerID, nullptr);
		}
		setScriptActionType(option["planAction"], actionType);
		option["planAction"]["hero_id"] = option["hero_id"];
		if(town)
			option["planAction"]["town_id"] = option["town_id"];
		actionSpace["nullkillerHelperOptions"].Vector().push_back(option);
	};

	std::set<int32_t> seenUpgradeArmies;
	auto appendUpgradeOptions = [&](const CArmedInstance * army)
	{
		if(!army || army->tempOwner != playerID || !seenUpgradeArmies.insert(army->id.getNum()).second)
			return;

		appendNullkillerUpgradeHelperOption(army);

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

	std::set<std::tuple<int32_t, int32_t, int32_t, int32_t, bool, bool>> seenPrepareHeroOptions;
	auto appendPrepareHeroOption = [&](
		const CGHeroInstance * hero,
		const CArmedInstance * source,
		const CGHeroInstance * otherHero,
		int32_t kindID,
		const std::string & kind,
		bool includeArtifacts,
		bool includeCreatures)
	{
		if(!hero || hero->tempOwner != playerID || (!includeArtifacts && !includeCreatures))
			return;
		if(includeCreatures)
		{
			if(!source || source == hero || source->tempOwner != playerID || source->visitablePos() != hero->visitablePos())
				return;
		}
		if(otherHero)
		{
			if(otherHero == hero || otherHero->tempOwner != playerID || otherHero->visitablePos() != hero->visitablePos())
				return;
		}

		const int32_t sourceID = source ? source->id.getNum() : -1;
		const int32_t otherHeroID = otherHero ? otherHero->id.getNum() : -1;
		if(!seenPrepareHeroOptions.emplace(hero->id.getNum(), sourceID, otherHeroID, kindID, includeArtifacts, includeCreatures).second)
			return;

		JsonNode option;
		option["hero_id"] = JsonNode(hero->id.getNum());
		option["hero"] = jsonHero(hero);
		option["preparationKindId"] = JsonNode(kindID);
		option["preparationKind"] = JsonNode(kind);
		option["includeArtifacts"] = JsonNode(includeArtifacts);
		option["includeCreatures"] = JsonNode(includeCreatures);
		if(source)
		{
			option["source_id"] = JsonNode(source->id.getNum());
			option["sourceArmy"] = jsonOwnedArmySnapshot(source);
		}
		if(otherHero)
		{
			option["other_hero_id"] = JsonNode(otherHero->id.getNum());
			option["otherHero"] = jsonHero(otherHero);
		}
		setScriptActionType(option["planAction"], "prepare_hero");
		option["planAction"]["hero_id"] = option["hero_id"];
		option["planAction"]["include_artifacts"] = JsonNode(includeArtifacts);
		option["planAction"]["include_creatures"] = JsonNode(includeCreatures);
		if(source)
			option["planAction"]["source_id"] = option["source_id"];
		if(otherHero)
			option["planAction"]["other_hero_id"] = option["other_hero_id"];

		actionSpace["prepareHeroOptions"].Vector().push_back(option);
		if(includeCreatures)
			actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
	};

	for(const CGTownInstance * town : cc->getTownsInfo(true))
	{
		if(!town || town->tempOwner != playerID)
			continue;

		appendNullkillerBuildArmyHelperOption(town);
		appendNullkillerRecruitHelperOption(town, nullptr);
		appendNullkillerMoveCreaturesToHeroHelperOption(town);
		appendUpgradeOptions(town);
		appendUpgradeOptions(town->getVisitingHero());
		appendUpgradeOptions(town->getGarrisonHero());

		if(const CGHeroInstance * visitingHero = town->getVisitingHero())
		{
			if(visitingHero->tempOwner == playerID)
			{
				appendPrepareHeroOption(visitingHero, town, nullptr, 1, "town_army", true, true);
				if(const CGHeroInstance * garrisonHero = town->getGarrisonHero())
				{
					if(garrisonHero->tempOwner == playerID)
					{
						appendPrepareHeroOption(visitingHero, garrisonHero, garrisonHero, 2, "garrison_hero", true, true);
						appendPrepareHeroOption(garrisonHero, visitingHero, visitingHero, 3, "visiting_hero", true, true);
					}
				}
			}
		}

		if(town->tempOwner == playerID)
		{
			for(size_t levelIndex = 0; levelIndex < town->spells.size(); ++levelIndex)
			{
				const int32_t spellLevel = static_cast<int32_t>(levelIndex + 1);
				if(spellLevel > town->mageGuildLevel())
					continue;

				const int32_t visibleSpellCount = std::clamp<int32_t>(
					town->spellsAtLevel(static_cast<int32_t>(levelIndex), false),
					0,
					static_cast<int32_t>(town->spells[levelIndex].size()));
				for(size_t spellIndex = 0; spellIndex < static_cast<size_t>(visibleSpellCount); ++spellIndex)
				{
					if(auto option = jsonSpellResearchOption(town, levelIndex, town->spells[levelIndex][spellIndex], resources, cc->getSettings()))
					{
						actionSpace["spellResearchOptions"].Vector().push_back(*option);
						if((*option)["affordable"].Bool())
							actionSpace["recommendedActions"].Vector().push_back((*option)["planAction"]);
					}
				}
			}
		}

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

		for(const BuildingID & buildingID : town->getBuildings())
		{
			const auto buildingIter = town->getTown()->buildings.find(buildingID);
			if(buildingIter == town->getTown()->buildings.end() || !buildingIter->second)
				continue;

			const CBuilding * building = buildingIter->second.get();
			const bool visitsBank = building->subId == BuildingSubID::BANK;
			const bool visitsManualReward = town->rewardableBuildings.count(buildingID) && town->getVisitingHero() && building->manualHeroVisit;
			if(!visitsBank && !visitsManualReward)
				continue;

			JsonNode option = jsonTownBuilding(town, buildingID);
			option["town_id"] = JsonNode(town->id.getNum());
			option["visitKindId"] = JsonNode(visitsBank ? 1 : 2);
			option["visitKind"] = JsonNode(visitsBank ? "bank" : "manual_reward");
			setScriptActionType(option["planAction"], "visit_town_building");
			option["planAction"]["town_id"] = option["town_id"];
			option["planAction"]["building_id"] = option["building_id"];
			actionSpace["visitTownBuildingOptions"].Vector().push_back(option);
			actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
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

				JsonNode option = jsonAvailableHeroOption(town, hero, playerID);
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
				if(town->hasBuilt(BuildingSubID::CASTLE_GATE))
				{
					for(const CGTownInstance * destination : cc->getTownsInfo(true))
					{
						if(!destination || destination == town || destination->tempOwner != playerID)
							continue;
						if(destination->getVisitingHero() || destination->getFactionID() != town->getFactionID() || !destination->hasBuilt(BuildingSubID::CASTLE_GATE))
							continue;

						JsonNode option;
						option["hero_id"] = JsonNode(visitingHero->id.getNum());
						option["source_town_id"] = JsonNode(town->id.getNum());
						option["destination_town_id"] = JsonNode(destination->id.getNum());
						option["sourceTown"] = jsonTown(town, resources, true);
						option["destinationTown"] = jsonTown(destination, resources, true);
						setScriptActionType(option["planAction"], "castle_teleport");
						option["planAction"]["hero_id"] = option["hero_id"];
						option["planAction"]["destination_town_id"] = option["destination_town_id"];
						actionSpace["castleTeleportOptions"].Vector().push_back(option);
						actionSpace["recommendedActions"].Vector().push_back(option["planAction"]);
					}
				}

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

	appendNullkillerDismissWeakHeroHelperOption();

	std::vector<const CGHeroInstance *> ownedHeroes;
	for(const CGHeroInstance * hero : cc->getHeroesInfo())
	{
		appendUpgradeOptions(hero);
		if(hero && hero->tempOwner == playerID)
		{
			ownedHeroes.push_back(hero);
			appendPrepareHeroOption(hero, nullptr, nullptr, 0, "self_artifacts", true, false);
			appendNullkillerFormationHelperOption(hero, nullptr, 6, "single_creature_stacks", "nullkiller_add_single_creature_stacks");
			appendNullkillerFormationHelperOption(hero, nullptr, 7, "whirlpool_formation", "nullkiller_rearrange_for_whirlpool");
		}
	}

	std::set<int32_t> seenQuestObjects;
	constexpr size_t maxQuestObjectOptions = 64;
	auto appendQuestObjectOption = [&](const CGObjectInstance * object)
	{
		if(!object || !cc->isVisibleFor(object, playerID))
			return;
		const auto * questObject = dynamic_cast<const IQuestObject *>(object);
		if(!questObject || !seenQuestObjects.insert(object->id.getNum()).second)
			return;
		if(actionSpace["questObjectOptions"].Vector().size() >= maxQuestObjectOptions)
		{
			actionSpace["questObjectOptionsTruncated"] = JsonNode(true);
			return;
		}

		const CQuest & quest = questObject->getQuest();
		const bool active = quest.activeForPlayers.count(playerID) != 0;
		JsonNode option;
		option["object_id"] = JsonNode(object->id.getNum());
		option["object"] = jsonMapObject(object, playerID, nullptr);
		option["quest"] = jsonQuestObject(questObject, playerID, nullptr);
		option["heroCandidates"].Vector();
		option["canCompleteWithAnyOwnedHero"] = JsonNode(false);
		for(const CGHeroInstance * hero : ownedHeroes)
		{
			if(!hero || hero->tempOwner != playerID)
				continue;

			const bool canComplete = active && questObject->checkQuest(hero);
			JsonNode candidate;
			candidate["hero_id"] = JsonNode(hero->id.getNum());
			candidate["hero"] = jsonHero(hero);
			candidate["canComplete"] = JsonNode(canComplete);
			if(canComplete)
				option["canCompleteWithAnyOwnedHero"] = JsonNode(true);
			option["heroCandidates"].Vector().push_back(candidate);
		}
		actionSpace["questObjectOptions"].Vector().push_back(option);
	};

	for(const CGHeroInstance * hero : ownedHeroes)
	{
		for(const CGHeroInstance * otherHero : ownedHeroes)
		{
			if(hero == otherHero || hero->visitablePos() != otherHero->visitablePos())
				continue;
			appendPrepareHeroOption(hero, otherHero, otherHero, 4, "co_located_hero", true, true);
		}
	}

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
		{
			const CGObjectInstance * object = cc->getObj(objectID, false);
			appendShipyardOption(objectID);
			appendQuestObjectOption(object);
			if(const auto * town = dynamic_cast<const CGTownInstance *>(object))
			{
				for(const CGHeroInstance * hero : ownedHeroes)
					appendNullkillerFormationHelperOption(hero, town, 8, "siege_formation", "nullkiller_rearrange_for_siege");
			}
			if(const CGDwelling * dwelling = dynamic_cast<const CGDwelling *>(object))
				appendNullkillerRecruitHelperOption(dwelling, nullptr);
		}
		for(ObjectInstanceID objectID : tile->blockingObjects)
		{
			const CGObjectInstance * object = cc->getObj(objectID, false);
			appendShipyardOption(objectID);
			appendQuestObjectOption(object);
			if(const auto * town = dynamic_cast<const CGTownInstance *>(object))
			{
				for(const CGHeroInstance * hero : ownedHeroes)
					appendNullkillerFormationHelperOption(hero, town, 8, "siege_formation", "nullkiller_rearrange_for_siege");
			}
			if(const CGDwelling * dwelling = dynamic_cast<const CGDwelling *>(object))
				appendNullkillerRecruitHelperOption(dwelling, nullptr);
		}
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

				JsonNode option = jsonAdventureSpellOption(cc, playerID, hero, spell, target, targetKindID, targetKind);
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
			setScriptActionType(option["planAction"], "move_hero");
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
				setScriptActionType(target["planAction"], "visit_object");
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

	setScriptActionType(actionSpace["endTurnAction"], "end_turn");
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
	analysis["candidateLimits"]["maxQuestObjectOptions"] = JsonNode(64);
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
	for(const char * field : { "reason", "value", "riskId", "risk", "safe", "danger", "dangerRatio", "estimatedLoss", "blockedBy", "typeId", "subtypeId", "kindId", "buildingKindId", "transferKindId", "preparationKindId", "pathActionId", "levelId", "statusId", "targetKindId", "spell_id", "task_id", "goalTypeId", "priorityTier", "heroRoleId", "nullkillerRoleId", "nullkillerArtifactScore", "nullkillerPotentialArtifactScore", "outcomeId", "failureActionId" })
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

	if(nullkiller && nullkiller->settings)
	{
		const auto & settings = *nullkiller->settings;
		analysis["nullkiller"]["settings"]["maxPass"] = JsonNode(settings.getMaxPass());
		analysis["nullkiller"]["settings"]["maxPriorityPass"] = JsonNode(settings.getMaxPriorityPass());
		analysis["nullkiller"]["settings"]["maxGoldPressure"].Float() = settings.getMaxGoldPressure();
		analysis["nullkiller"]["settings"]["retreatThresholdRelative"].Float() = settings.getRetreatThresholdRelative();
		analysis["nullkiller"]["settings"]["retreatThresholdAbsolute"].Float() = settings.getRetreatThresholdAbsolute();
		analysis["nullkiller"]["settings"]["safeAttackRatio"].Float() = settings.getSafeAttackRatio();
		analysis["nullkiller"]["settings"]["maxArmyLossTarget"].Float() = settings.getMaxArmyLossTarget();
		analysis["nullkiller"]["settings"]["maxRoamingHeroes"] = JsonNode(settings.getMaxRoamingHeroes());
		analysis["nullkiller"]["settings"]["maxRoamingHeroesPerTown"] = JsonNode(settings.getMaxRoamingHeroesPerTown());
		analysis["nullkiller"]["settings"]["mainHeroTurnDistanceLimit"] = JsonNode(settings.getMainHeroTurnDistanceLimit());
		analysis["nullkiller"]["settings"]["scoutHeroTurnDistanceLimit"] = JsonNode(settings.getScoutHeroTurnDistanceLimit());
		analysis["nullkiller"]["settings"]["threatTurnDistanceLimit"] = JsonNode(settings.getThreatTurnDistanceLimit());
		analysis["nullkiller"]["settings"]["pathfinderBucketsCount"] = JsonNode(settings.getPathfinderBucketsCount());
		analysis["nullkiller"]["settings"]["pathfinderBucketSize"] = JsonNode(settings.getPathfinderBucketSize());
		analysis["nullkiller"]["settings"]["objectGraphAllowed"] = JsonNode(settings.isObjectGraphAllowed());
		analysis["nullkiller"]["settings"]["garrisonTroopsUsageAllowed"] = JsonNode(settings.isGarrisonTroopsUsageAllowed());
		analysis["nullkiller"]["settings"]["oneWayMonolithUsageAllowed"] = JsonNode(settings.isOneWayMonolithUsageAllowed());
		analysis["nullkiller"]["settings"]["updateHitmapOnTileReveal"] = JsonNode(settings.isUpdateHitmapOnTileReveal());
		analysis["nullkiller"]["settings"]["openMap"] = JsonNode(settings.isOpenMap());

		std::unique_lock aiLock(nullkiller->aiStateMutex);
		const NK2AI::ScanDepth scanDepth = nullkiller->getScanDepth();
		analysis["nullkiller"]["state"]["scanDepthId"] = JsonNode(static_cast<int32_t>(scanDepth));
		analysis["nullkiller"]["state"]["scanDepth"] = JsonNode(nullkillerScanDepthName(scanDepth));
		analysis["nullkiller"]["state"]["openMap"] = JsonNode(nullkiller->isOpenMap());
		analysis["nullkiller"]["state"]["objectGraphAllowed"] = JsonNode(nullkiller->isObjectGraphAllowed());
		analysis["nullkiller"]["state"]["pathfinderTurnStorageMisses"] = JsonNode(nullkiller->pathfinderTurnStorageMisses.load());
		analysis["nullkiller"]["state"]["lockedResources"] = jsonResources(nullkiller->getLockedResources());
		analysis["nullkiller"]["state"]["freeResources"] = jsonResources(nullkiller->getFreeResources());
		if(nullkiller->heroManager)
		{
			nullkiller->heroManager->update();
			const int32_t heroCount = cc->getHeroCount(playerID, false);
			const int32_t heroCountIncludingGarrisoned = cc->getHeroCount(playerID, true);
			const int32_t townCount = cc->howManyTowns();
			int32_t roamingPolicyCap = settings.getMaxRoamingHeroes();
			if(settings.getMaxRoamingHeroesPerTown() > 0)
				roamingPolicyCap += townCount * settings.getMaxRoamingHeroesPerTown();

			JsonNode & recruitment = analysis["nullkiller"]["heroRecruitment"];
			recruitment["heroCount"] = JsonNode(heroCount);
			recruitment["heroCountIncludingGarrisoned"] = JsonNode(heroCountIncludingGarrisoned);
			recruitment["townCount"] = JsonNode(townCount);
			recruitment["roamingPolicyCap"] = JsonNode(roamingPolicyCap);
			recruitment["onMapCap"] = JsonNode(cc->getSettings().getInteger(EGameSettings::HEROES_PER_PLAYER_ON_MAP_CAP));
			recruitment["totalCap"] = JsonNode(cc->getSettings().getInteger(EGameSettings::HEROES_PER_PLAYER_TOTAL_CAP));
			recruitment["heroCapReached"] = JsonNode(nullkiller->heroManager->heroCapReached(false));
			recruitment["heroCapReachedIncludingGarrisoned"] = JsonNode(nullkiller->heroManager->heroCapReached(true));
			recruitment["canRecruitAnyHero"] = JsonNode(nullkiller->heroManager->canRecruitHero());
			recruitment["towns"].Vector();
			recruitment["recruitableTownIds"].Vector();

			for(const CGTownInstance * town : cc->getTownsInfo())
			{
				if(!town || town->tempOwner != playerID)
					continue;

				const bool hasFreeTavern = NK2AI::townHasFreeTavern(town);
				const bool enoughGold = cc->getResourceAmount(EGameResID::GOLD) >= GameConstants::HERO_GOLD_COST;
				const bool heroCapReached = nullkiller->heroManager->heroCapReached();
				std::vector<const CGHeroInstance *> availableHeroes = cc->getAvailableHeroes(town);
				const bool canRecruitHero = nullkiller->heroManager->canRecruitHero(town);

				int32_t blockedReasonId = 0;
				if(!hasFreeTavern)
					blockedReasonId = 1;
				else if(!enoughGold)
					blockedReasonId = 2;
				else if(heroCapReached)
					blockedReasonId = 3;
				else if(availableHeroes.empty())
					blockedReasonId = 4;

				JsonNode townNode;
				townNode["town_id"] = JsonNode(town->id.getNum());
				townNode["hasFreeTavern"] = JsonNode(hasFreeTavern);
				townNode["availableHeroCount"] = JsonNode(static_cast<int32_t>(availableHeroes.size()));
				townNode["availableHeroTypeIds"].Vector();
				for(const CGHeroInstance * availableHero : availableHeroes)
				{
					if(availableHero)
						townNode["availableHeroTypeIds"].Vector().push_back(JsonNode(availableHero->getHeroTypeID().getNum()));
				}
				townNode["canRecruitHero"] = JsonNode(canRecruitHero);
				townNode["blockedReasonId"] = JsonNode(canRecruitHero ? 0 : blockedReasonId);
				if(canRecruitHero)
					recruitment["recruitableTownIds"].Vector().push_back(JsonNode(town->id.getNum()));
				recruitment["towns"].Vector().push_back(townNode);
			}
			recruitment["recruitableTownCount"] = JsonNode(static_cast<int32_t>(recruitment["recruitableTownIds"].Vector().size()));
		}
		if(nullkiller->buildAnalyzer)
		{
			nullkiller->buildAnalyzer->update();
			analysis["nullkiller"]["economy"]["dailyIncome"] = jsonResources(nullkiller->buildAnalyzer->getDailyIncome());
			analysis["nullkiller"]["economy"]["goldPressure"].Float() = nullkiller->buildAnalyzer->getGoldPressure();
			analysis["nullkiller"]["economy"]["goldPressureOverMax"] = JsonNode(nullkiller->buildAnalyzer->isGoldPressureOverMax());
			analysis["nullkiller"]["economy"]["missingResourcesNow"] = jsonResources(nullkiller->buildAnalyzer->getMissingResourcesNow());
			analysis["nullkiller"]["economy"]["missingResourcesTotal"] = jsonResources(nullkiller->buildAnalyzer->getMissingResourcesInTotal());
			analysis["nullkiller"]["economy"]["freeResourcesAfterMissingTotal"] = jsonResources(nullkiller->buildAnalyzer->getFreeResourcesAfterMissingTotal());
			analysis["nullkiller"]["townDevelopment"].Vector();
			for(const NK2AI::TownDevelopmentInfo & developmentInfo : nullkiller->buildAnalyzer->getDevelopmentInfo())
				analysis["nullkiller"]["townDevelopment"].Vector().push_back(jsonNullkillerTownDevelopmentInfo(developmentInfo));
		}
	}

	for(const CGObjectInstance * object : cc->getAllVisitableObjs())
	{
		if(!object || !object->tempOwner.isValidPlayer() || !isOpponent(object->tempOwner))
			continue;

		if(const auto * enemyHero = dynamic_cast<const CGHeroInstance *>(object))
		{
			enemyHeroes.push_back(enemyHero);
			JsonNode enemyHeroJson = jsonHero(enemyHero, false);
			enemyHeroJson["visibleObject"] = jsonMapObject(enemyHero, playerID, nullptr);
			analysis["visibleEnemyHeroes"].Vector().push_back(enemyHeroJson);
		}
		else if(const auto * enemyTown = dynamic_cast<const CGTownInstance *>(object))
		{
			analysis["visibleEnemyTowns"].Vector().push_back(jsonTown(enemyTown, resources, false));
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
				cachedRunner.reset();
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
		cachedRunner.reset();
	}

	const bool previousReloadScriptEachTurn = scriptConfig.reloadScriptEachTurn;
	scriptConfig.reloadScriptEachTurn = readBool(config, "reloadScriptEachTurn", scriptConfig.reloadScriptEachTurn);
	if(scriptConfig.reloadScriptEachTurn != previousReloadScriptEachTurn || scriptConfig.reloadScriptEachTurn)
		cachedRunner.reset();
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
