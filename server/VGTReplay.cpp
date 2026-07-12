/*
 * VGTReplay.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"
#include "VGTReplay.h"

#include "CGameHandler.h"
#include "IGameServer.h"

#include "battles/BattleProcessor.h"
#include "processors/HeroPoolProcessor.h"
#include "processors/PlayerMessageProcessor.h"
#include "processors/TurnOrderProcessor.h"
#include "TurnTimerHandler.h"

#include "../lib/CPlayerState.h"
#include "../lib/CConfigHandler.h"
#include "../lib/LoadProgress.h"
#include "../lib/StartInfo.h"
#include "../lib/callback/GameRandomizer.h"
#include "../lib/campaign/CampaignState.h"
#include "../lib/constants/StringConstants.h"
#include "../lib/entities/artifact/CArtifactInstance.h"
#include "../lib/entities/artifact/CArtifactSet.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/json/JsonBonus.h"
#include "../lib/json/JsonNode.h"
#include "../lib/mapping/CMapHeader.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/mapObjects/CGCreature.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGTownInstance.h"
#include "../lib/mapObjects/CRewardableObject.h"
#include "../lib/mapObjects/MiscObjects.h"
#include "../lib/mapObjects/ObjectTemplate.h"
#include "../lib/mapObjects/army/CArmedInstance.h"
#include "../lib/networkPacks/PacksForClient.h"
#include "../lib/networkPacks/PacksForClientBattle.h"
#include "../lib/networkPacks/PacksForServer.h"
#include "../lib/networkPacks/SetRewardableConfiguration.h"
#include "../lib/rmg/CMapGenOptions.h"
#include "../lib/serializer/CLoadFile.h"
#include "../lib/serializer/CSaveFile.h"
#include "../lib/serializer/JsonDeserializer.h"
#include "../lib/serializer/JsonSerializer.h"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>

namespace
{
class ReplayGameServer : public IGameServer
{
	CGameHandler * gameHandler = nullptr;
	EServerState state = EServerState::GAMEPLAY;

public:
	void attach(CGameHandler & handler)
	{
		gameHandler = &handler;
	}

	void setState(EServerState value) override
	{
		state = value;
	}

	EServerState getState() const override
	{
		return state;
	}

	bool isPlayerHost(const PlayerColor & color) const override
	{
		return color.isValidPlayer();
	}

	bool hasPlayerAt(PlayerColor player, GameConnectionID) const override
	{
		return player.isValidPlayer();
	}

	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override
	{
		return left.isValidPlayer() && right.isValidPlayer();
	}

	void applyPack(CPackForClient & pack) override
	{
		if(!gameHandler || !gameHandler->gs)
			throw std::runtime_error("VGT replay cannot apply a pack before game state initialization");
		gameHandler->gs->apply(pack);
	}

	void sendPack(CPackForClient &, GameConnectionID) override
	{
	}
};

JsonNode readJsonFile(const std::string & path)
{
	std::ifstream input(path);
	if(!input)
		throw std::runtime_error("Unable to read VGT replay JSON: " + path);

	std::ostringstream buffer;
	buffer << input.rdbuf();
	const std::string data = buffer.str();

	JsonParsingSettings parserSettings;
	parserSettings.mode = JsonParsingSettings::JsonFormatMode::JSON;
	parserSettings.strict = true;
	return JsonNode(data.c_str(), data.size(), parserSettings, path);
}

const JsonNode & requireField(const JsonNode & node, const char * field)
{
	const std::string fieldName(field);
	if(!node.isStruct())
		throw std::runtime_error("VGT replay parent is not a mapping while reading: " + fieldName);

	const auto iter = node.Struct().find(fieldName);
	if(iter == node.Struct().end() || iter->second.isNull())
		throw std::runtime_error("Missing VGT replay field: " + fieldName);
	return iter->second;
}

const JsonNode * findField(const JsonNode & node, const char * field)
{
	if(!node.isStruct())
		return nullptr;

	const auto iter = node.Struct().find(field);
	if(iter == node.Struct().end() || iter->second.isNull())
		return nullptr;
	return &iter->second;
}

const JsonNode * findNullableField(const JsonNode & node, const char * field)
{
	if(!node.isStruct())
		return nullptr;

	const auto iter = node.Struct().find(field);
	if(iter == node.Struct().end())
		return nullptr;
	return &iter->second;
}

bool hasField(const JsonNode & node, const char * field)
{
	return findField(node, field) != nullptr;
}

std::string requireString(const JsonNode & node, const char * field)
{
	const auto & child = requireField(node, field);
	if(!child.isString())
		throw std::runtime_error(std::string("VGT replay field is not a string: ") + field);
	return child.String();
}

int64_t requireInteger(const JsonNode & node, const char * field)
{
	const auto & child = requireField(node, field);
	if(!child.isNumber())
		throw std::runtime_error(std::string("VGT replay field is not a number: ") + field);
	return child.Integer();
}

bool requireBool(const JsonNode & node, const char * field)
{
	const auto & child = requireField(node, field);
	if(!child.isBool())
		throw std::runtime_error(std::string("VGT replay field is not a bool: ") + field);
	return child.Bool();
}

EStartMode decodeStartMode(const std::string & value)
{
	if(value == "newGame")
		return EStartMode::NEW_GAME;
	if(value == "loadGame")
		return EStartMode::LOAD_GAME;
	if(value == "campaign")
		return EStartMode::CAMPAIGN;
	throw std::runtime_error("Unsupported VGT start mode: " + value);
}

ui8 decodeDifficulty(const std::string & value)
{
	for(size_t index = 0; index < std::size(GameConstants::DIFFICULTY_NAMES); ++index)
	{
		if(GameConstants::DIFFICULTY_NAMES[index] == value)
			return static_cast<ui8>(index);
	}
	throw std::runtime_error("Unsupported VGT difficulty: " + value);
}

PlayerStartingBonus decodeStartingBonus(const std::string & value)
{
	if(value == "random")
		return PlayerStartingBonus::RANDOM;
	if(value == "artifact")
		return PlayerStartingBonus::ARTIFACT;
	if(value == "gold")
		return PlayerStartingBonus::GOLD;
	if(value == "resource")
		return PlayerStartingBonus::RESOURCE;
	throw std::runtime_error("Unsupported VGT starting bonus: " + value);
}

PlayerColor decodePlayerColor(const std::string & value)
{
	const auto result = PlayerColor::decode(value);
	if(result < 0 || result >= PlayerColor::PLAYER_LIMIT_I)
		throw std::runtime_error("Unsupported VGT player color: " + value);
	return PlayerColor(result);
}

PlayerColor decodeColor(const std::string & value)
{
	if(value == "cannotDetermine")
		return PlayerColor::CANNOT_DETERMINE;
	if(value == "unflaggable")
		return PlayerColor::UNFLAGGABLE;
	if(value == "invalid")
		return PlayerColor::CANNOT_DETERMINE;
	if(value == "neutral")
		return PlayerColor::NEUTRAL;
	if(value == "spectator")
		return PlayerColor::SPECTATOR;
	return decodePlayerColor(value);
}

MapObjectID decodeMapObject(const std::string & value)
{
	std::string identifier = value;
	const std::string corePrefix = "core:";
	if(identifier.starts_with(corePrefix))
		identifier.erase(0, corePrefix.size());
	return MapObjectID(MapObjectID::decode(identifier));
}

HeroTypeID decodeHeroType(const std::string & value)
{
	if(value == "core:none")
		return HeroTypeID::NONE;
	if(value == "random")
		return HeroTypeID::RANDOM;
	if(value == "campaignStrongest")
		return HeroTypeID::CAMP_STRONGEST;
	if(value == "campaignGenerated")
		return HeroTypeID::CAMP_GENERATED;
	if(value == "campaignRandom")
		return HeroTypeID::CAMP_RANDOM;
	return HeroTypeID(HeroTypeID::decode(value));
}

FactionID decodeFaction(const std::string & value)
{
	if(value == "core:none")
		return FactionID::NONE;
	if(value == "random")
		return FactionID::RANDOM;
	return FactionID(FactionID::decode(value));
}

GameResID decodeResource(const std::string & value)
{
	if(value == "core:none")
		return GameResID::NONE;
	return GameResID(GameResID::decode(value));
}

SpellID decodeSpell(const std::string & value)
{
	if(value == "core:none")
		return SpellID::NONE;
	return SpellID(SpellID::decode(value));
}

PrimarySkill decodePrimarySkill(const std::string & value)
{
	if(value == "core:none")
		return PrimarySkill::NONE;
	return PrimarySkill(PrimarySkill::decode(value));
}

SecondarySkill decodeSecondarySkill(const std::string & value)
{
	if(value == "core:none")
		return SecondarySkill::NONE;
	return SecondarySkill(SecondarySkill::decode(value));
}

CreatureID decodeCreature(const std::string & value)
{
	if(value == "core:none")
		return CreatureID::NONE;
	return CreatureID(CreatureID::decode(value));
}

CGCreature::Character decodeCreatureCharacter(const std::string & value)
{
	if(value == "compliant") return CGCreature::Character::COMPLIANT;
	if(value == "friendly") return CGCreature::Character::FRIENDLY;
	if(value == "aggressive") return CGCreature::Character::AGGRESSIVE;
	if(value == "hostile") return CGCreature::Character::HOSTILE;
	if(value == "savage") return CGCreature::Character::SAVAGE;
	if(value == "custom") return CGCreature::Character::CUSTOM;
	throw std::runtime_error("Unsupported VGT creature character: " + value);
}

CGCreature::UpgradedStackPresence decodeUpgradedStackPresence(const std::string & value)
{
	if(value == "random") return CGCreature::UpgradedStackPresence::RANDOM;
	if(value == "never") return CGCreature::UpgradedStackPresence::NEVER;
	if(value == "always") return CGCreature::UpgradedStackPresence::ALWAYS;
	throw std::runtime_error("Unsupported VGT upgraded stack presence: " + value);
}

ArtifactID decodeArtifact(const std::string & value)
{
	if(value == "core:none")
		return ArtifactID::NONE;
	return ArtifactID(ArtifactID::decode(value));
}

BuildingID decodeBuilding(const std::string & value)
{
	if(value == "none")
		return BuildingID::NONE;

	std::string identifier = value;
	const std::string corePrefix = "core:";
	if(identifier.starts_with(corePrefix))
		identifier.erase(0, corePrefix.size());

	for(size_t index = 0; index < std::size(EBuildingType::names); ++index)
	{
		if(EBuildingType::names[index] == identifier)
			return BuildingID(static_cast<int>(index));
	}

	return BuildingID(BuildingID::decode(identifier));
}

ui8 decodeArrangeMode(const std::string & value)
{
	if(value == "swap")
		return 1;
	if(value == "merge")
		return 2;
	if(value == "split")
		return 3;
	throw std::runtime_error("Unsupported VGT arrange mode: " + value);
}

ChangeValueMode decodeChangeMode(const std::string & value)
{
	if(value == "absolute")
		return ChangeValueMode::ABSOLUTE;
	if(value == "relative")
		return ChangeValueMode::RELATIVE;
	throw std::runtime_error("Unsupported VGT change mode: " + value);
}

TryMoveHero::EResult decodeMovementResult(const std::string & value)
{
	if(value == "failed")
		return TryMoveHero::FAILED;
	if(value == "success")
		return TryMoveHero::SUCCESS;
	if(value == "teleportation")
		return TryMoveHero::TELEPORTATION;
	if(value == "blockingVisit")
		return TryMoveHero::BLOCKING_VISIT;
	if(value == "embark")
		return TryMoveHero::EMBARK;
	if(value == "disembark")
		return TryMoveHero::DISEMBARK;
	throw std::runtime_error("Unsupported VGT movement result: " + value);
}

EVictoryLossCheckResult decodeVictoryLossResult(const std::string & value)
{
	if(value == "victory")
		return EVictoryLossCheckResult::victory(MetaString(), MetaString());
	if(value == "loss")
		return EVictoryLossCheckResult::defeat(MetaString(), MetaString());
	if(value == "ingame")
		return EVictoryLossCheckResult();
	throw std::runtime_error("Unsupported VGT player end result: " + value);
}

EWeekType decodeWeekType(const std::string & value)
{
	if(value == "firstWeek")
		return EWeekType::FIRST_WEEK;
	if(value == "normal")
		return EWeekType::NORMAL;
	if(value == "doubleGrowth")
		return EWeekType::DOUBLE_GROWTH;
	if(value == "bonusGrowth")
		return EWeekType::BONUS_GROWTH;
	if(value == "deityOfFire")
		return EWeekType::DEITYOFFIRE;
	if(value == "plague")
		return EWeekType::PLAGUE;
	throw std::runtime_error("Unsupported VGT week type: " + value);
}

EPlayerType decodeRandomMapPlayerType(const std::string & value)
{
	if(value == "human")
		return EPlayerType::HUMAN;
	if(value == "ai")
		return EPlayerType::AI;
	if(value == "computerOnly")
		return EPlayerType::COMP_ONLY;
	throw std::runtime_error("Unsupported VGT random map player type: " + value);
}

EWaterContent::EWaterContent decodeRandomMapWater(const std::string & value)
{
	if(value == "random")
		return EWaterContent::RANDOM;
	if(value == "none")
		return EWaterContent::NONE;
	if(value == "normal")
		return EWaterContent::NORMAL;
	if(value == "islands")
		return EWaterContent::ISLANDS;
	throw std::runtime_error("Unsupported VGT random map water setting: " + value);
}

EMonsterStrength::EMonsterStrength decodeRandomMapMonsterStrength(const std::string & value)
{
	if(value == "random")
		return EMonsterStrength::RANDOM;
	if(value == "weak")
		return EMonsterStrength::GLOBAL_WEAK;
	if(value == "normal")
		return EMonsterStrength::GLOBAL_NORMAL;
	if(value == "strong")
		return EMonsterStrength::GLOBAL_STRONG;
	throw std::runtime_error("Unsupported VGT random map monster strength: " + value);
}

TeamID decodeTeam(const JsonNode & node)
{
	if(node.isString())
	{
		const auto value = node.String();
		if(value == "none")
			return TeamID::NO_TEAM;
		return TeamID(std::stoi(value));
	}

	if(node.isNumber())
		return TeamID(static_cast<int>(node.Integer()));

	throw std::runtime_error("VGT replay team id is not a string or number");
}

RoadId decodeRoad(const std::string & value)
{
	if(value == "core:none")
		return RoadId::NO_ROAD;

	const std::string fallbackPrefix = "road:";
	if(value.starts_with(fallbackPrefix))
		return RoadId(std::stoi(value.substr(fallbackPrefix.size())));

	return RoadId(RoadId::decode(value));
}

RumorState::ERumorType decodeRumorType(const std::string & value)
{
	if(value == "none")
		return RumorState::TYPE_NONE;
	if(value == "random")
		return RumorState::TYPE_RAND;
	if(value == "special")
		return RumorState::TYPE_SPECIAL;
	if(value == "map")
		return RumorState::TYPE_MAP;
	throw std::runtime_error("Unsupported VGT rumor type: " + value);
}

ETileVisibility decodeVisibility(const std::string & value)
{
	if(value == "hidden")
		return ETileVisibility::HIDDEN;
	if(value == "revealed")
		return ETileVisibility::REVEALED;
	throw std::runtime_error("Unsupported VGT visibility mode: " + value);
}

ObjProperty decodeObjProperty(const JsonNode & node)
{
	if(node.isNumber())
		return static_cast<ObjProperty>(node.Integer());

	if(!node.isString())
		throw std::runtime_error("VGT replay object property is not a string or number");

	const std::string value = node.String();
	if(value == "invalid") return ObjProperty::INVALID;
	if(value == "owner") return ObjProperty::OWNER;
	if(value == "unused") return ObjProperty::UNUSED;
	if(value == "primaryStackCount") return ObjProperty::PRIMARY_STACK_COUNT;
	if(value == "visitors") return ObjProperty::VISITORS;
	if(value == "visited") return ObjProperty::VISITED;
	if(value == "id") return ObjProperty::ID;
	if(value == "availableCreature") return ObjProperty::AVAILABLE_CREATURE;
	if(value == "monsterCount") return ObjProperty::MONSTER_COUNT;
	if(value == "monsterPower") return ObjProperty::MONSTER_POWER;
	if(value == "monsterExperience") return ObjProperty::MONSTER_EXP;
	if(value == "monsterRestoreType") return ObjProperty::MONSTER_RESTORE_TYPE;
	if(value == "monsterRefusedJoin") return ObjProperty::MONSTER_REFUSED_JOIN;
	if(value == "structureAddVisitingHero") return ObjProperty::STRUCTURE_ADD_VISITING_HERO;
	if(value == "structureClearVisitors") return ObjProperty::STRUCTURE_CLEAR_VISITORS;
	if(value == "structureAddGarrisonedHero") return ObjProperty::STRUCTURE_ADD_GARRISONED_HERO;
	if(value == "bonusValueFirst") return ObjProperty::BONUS_VALUE_FIRST;
	if(value == "bonusValueSecond") return ObjProperty::BONUS_VALUE_SECOND;
	if(value == "seerHutVisited") return ObjProperty::SEERHUT_VISITED;
	if(value == "seerHutComplete") return ObjProperty::SEERHUT_COMPLETE;
	if(value == "obeliskVisited") return ObjProperty::OBELISK_VISITED;
	if(value == "bankDayCounter") return ObjProperty::BANK_DAYCOUNTER;
	if(value == "bankClear") return ObjProperty::BANK_CLEAR;
	if(value == "rewardSelect") return ObjProperty::REWARD_SELECT;
	if(value == "rewardCleared") return ObjProperty::REWARD_CLEARED;
	throw std::runtime_error("Unsupported VGT object property: " + value);
}

ChangeObjectVisitors::VisitMode decodeObjectVisitMode(const std::string & value)
{
	if(value == "addHero")
		return ChangeObjectVisitors::VISITOR_ADD_HERO;
	if(value == "addPlayer")
		return ChangeObjectVisitors::VISITOR_ADD_PLAYER;
	if(value == "scouted")
		return ChangeObjectVisitors::VISITOR_SCOUTED;
	if(value == "clear")
		return ChangeObjectVisitors::VISITOR_CLEAR;
	throw std::runtime_error("Unsupported VGT object visitor mode: " + value);
}

TavernHeroSlot decodeTavernSlot(const std::string & value)
{
	if(value == "none")
		return TavernHeroSlot::NONE;
	if(value == "native")
		return TavernHeroSlot::NATIVE;
	if(value == "random")
		return TavernHeroSlot::RANDOM;
	throw std::runtime_error("Unsupported VGT tavern slot: " + value);
}

TavernSlotRole decodeTavernRole(const std::string & value)
{
	if(value == "none")
		return TavernSlotRole::NONE;
	if(value == "singleUnit")
		return TavernSlotRole::SINGLE_UNIT;
	if(value == "fullArmy")
		return TavernSlotRole::FULL_ARMY;
	if(value == "retreated")
		return TavernSlotRole::RETREATED;
	if(value == "surrendered")
		return TavernSlotRole::SURRENDERED;
	throw std::runtime_error("Unsupported VGT tavern role: " + value);
}

BattleSide decodeBattleSide(const std::string & value)
{
	if(value == "attacker")
		return BattleSide::ATTACKER;
	if(value == "defender")
		return BattleSide::DEFENDER;
	if(value == "none")
		return BattleSide::NONE;
	if(value == "invalid")
		return BattleSide::INVALID;
	if(value == "allKnowing")
		return BattleSide::ALL_KNOWING;
	throw std::runtime_error("Unsupported VGT battle side: " + value);
}

GiveBonus::ETarget decodeBonusTargetKind(const std::string & value)
{
	if(value == "object")
		return GiveBonus::ETarget::OBJECT;
	if(value == "player")
		return GiveBonus::ETarget::PLAYER;
	if(value == "battle")
		return GiveBonus::ETarget::BATTLE;
	if(value == "heroCommander")
		return GiveBonus::ETarget::HERO_COMMANDER;
	throw std::runtime_error("Unsupported VGT bonus target kind: " + value);
}

EActionType decodeActionType(const std::string & value)
{
	if(value == "none")
		return EActionType::NO_ACTION;
	if(value == "endTactics")
		return EActionType::END_TACTIC_PHASE;
	if(value == "retreat")
		return EActionType::RETREAT;
	if(value == "surrender")
		return EActionType::SURRENDER;
	if(value == "heroSpell")
		return EActionType::HERO_SPELL;
	if(value == "walk")
		return EActionType::WALK;
	if(value == "wait")
		return EActionType::WAIT;
	if(value == "defend")
		return EActionType::DEFEND;
	if(value == "walkAndAttack")
		return EActionType::WALK_AND_ATTACK;
	if(value == "shoot")
		return EActionType::SHOOT;
	if(value == "catapult")
		return EActionType::CATAPULT;
	if(value == "monsterSpell")
		return EActionType::MONSTER_SPELL;
	if(value == "badMorale")
		return EActionType::BAD_MORALE;
	if(value == "stackHeal")
		return EActionType::STACK_HEAL;
	if(value == "walkAndCast")
		return EActionType::WALK_AND_CAST;
	throw std::runtime_error("Unsupported VGT battle action: " + value);
}

int3 decodePosition(const JsonNode & node)
{
	if(!node.isVector() || node.Vector().size() != 3)
		throw std::runtime_error("VGT replay position must be a three-value list");

	return int3(
		static_cast<int>(node.Vector()[0].Integer()),
		static_cast<int>(node.Vector()[1].Integer()),
		static_cast<int>(node.Vector()[2].Integer()));
}

std::vector<int3> decodePath(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay path must be a list");

	std::vector<int3> result;
	for(const auto & entry : node.Vector())
		result.push_back(decodePosition(entry));
	return result;
}

FowTilesType decodeFowRuns(const JsonNode & node)
{
	if(node.isNull())
		return {};
	if(!node.isVector())
		throw std::runtime_error("VGT replay fog runs must be a list");

	FowTilesType result;
	for(const auto & run : node.Vector())
	{
		const int y = static_cast<int>(requireInteger(run, "y"));
		const int z = static_cast<int>(requireInteger(run, "z"));
		const auto & xRange = requireField(run, "x");
		if(!xRange.isVector() || xRange.Vector().size() != 2)
			throw std::runtime_error("VGT replay fog x range must have two entries");
		const int firstX = static_cast<int>(xRange.Vector()[0].Integer());
		const int lastX = static_cast<int>(xRange.Vector()[1].Integer());
		for(int x = firstX; x <= lastX; ++x)
			result.insert(int3(x, y, z));
	}
	return result;
}

std::string sanitizedAliasName(std::string name)
{
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character)
	{
		return static_cast<char>(std::tolower(character));
	});

	for(char & character : name)
	{
		if(!std::isalnum(static_cast<unsigned char>(character)))
			character = '-';
	}

	while(!name.empty() && name.front() == '-')
		name.erase(name.begin());
	while(!name.empty() && name.back() == '-')
		name.pop_back();
	return name;
}

std::vector<std::string> splitAlias(const std::string & value)
{
	std::vector<std::string> result;
	std::stringstream stream(value);
	std::string token;
	while(std::getline(stream, token, '/'))
		result.push_back(token);
	return result;
}

std::optional<int3> positionFromAlias(const std::string & alias)
{
	const auto marker = alias.rfind("/at-");
	if(marker == std::string::npos)
		return std::nullopt;

	std::vector<int> values;
	std::stringstream stream(alias.substr(marker + 4));
	std::string token;
	while(std::getline(stream, token, '-'))
		values.push_back(std::stoi(token));
	if(values.size() != 3)
		return std::nullopt;
	return int3(values[0], values[1], values[2]);
}

ObjectInstanceID resolveObjectAlias(const CGameState & gameState, const std::string & alias)
{
	if(alias == "object/none")
		return ObjectInstanceID::NONE;
	if(alias.starts_with("object/id-"))
		return ObjectInstanceID(std::stoi(alias.substr(std::string("object/id-").size())));

	const auto parts = splitAlias(alias);
	if(const auto position = positionFromAlias(alias))
	{
		std::string expectedType;
		std::optional<std::string> expectedOwner;
		std::optional<std::string> expectedName;
		if(parts.size() >= 4 && parts[0] == "object")
		{
			expectedType = parts[1];
			const bool hasOwner = parts.size() >= 5 && parts[2] != "at-" + std::to_string(position->x) + "-" + std::to_string(position->y) + "-" + std::to_string(position->z);
			if(hasOwner)
			{
				expectedOwner = parts[2];
				expectedName = parts[3];
			}
			else
				expectedName = parts[2];
		}

		std::optional<ObjectInstanceID> positionOnlyMatch;
		for(const auto & object : gameState.getMap().getObjects())
		{
			if(object && object->visitablePos() == *position)
			{
				if(!positionOnlyMatch)
					positionOnlyMatch = object->id;

				if(!expectedType.empty() && MapObjectID::encode(object->ID.getNum()) != expectedType)
					continue;
				if(expectedOwner && object->tempOwner.toString() != *expectedOwner)
					continue;
				if(expectedName)
				{
					std::string objectName = object->instanceName.empty() ? object->getObjectName() : object->instanceName;
					if(sanitizedAliasName(objectName) != *expectedName)
						continue;
				}
				return object->id;
			}
		}
		if(expectedName)
		{
			for(const auto & object : gameState.getMap().getObjects())
			{
				if(!object)
					continue;
				if(!expectedType.empty() && MapObjectID::encode(object->ID.getNum()) != expectedType)
					continue;
				if(expectedOwner && object->tempOwner.toString() != *expectedOwner)
					continue;

				std::string objectName = object->instanceName.empty() ? object->getObjectName() : object->instanceName;
				if(sanitizedAliasName(objectName) == *expectedName)
					return object->id;
			}

			throw std::runtime_error("Unable to resolve named VGT object alias: " + alias);
		}
		if(positionOnlyMatch)
			return *positionOnlyMatch;
	}

	if(parts.size() >= 3 && parts[0] == "hero")
	{
		const auto owner = parts[1];
		const auto name = parts[2];
		for(const auto & object : gameState.getMap().getObjects())
		{
			if(!object)
				continue;
			if(object->tempOwner.toString() != owner)
				continue;

			std::string objectName = object->instanceName.empty() ? object->getObjectName() : object->instanceName;
			if(sanitizedAliasName(objectName) == name)
				return object->id;
		}
	}

	throw std::runtime_error("Unable to resolve VGT object alias: " + alias);
}

PlayerColor playerFromActor(const std::string & actor)
{
	const std::string prefix = "player/";
	if(!actor.starts_with(prefix))
		throw std::runtime_error("Unsupported VGT actor: " + actor);
	return decodePlayerColor(actor.substr(prefix.size()));
}

QueryID decodeQuery(const std::string & value)
{
	const std::string prefix = "query/";
	if(!value.starts_with(prefix))
		throw std::runtime_error("Unsupported VGT query alias: " + value);
	if(value == "query/none")
		return QueryID::NONE;
	return QueryID(std::stoi(value.substr(prefix.size())));
}

SlotID decodeSlot(const JsonNode & node)
{
	if(!node.isNumber())
		throw std::runtime_error("VGT replay slot is not numeric");
	return SlotID(static_cast<int>(node.Integer()));
}

std::optional<SlotID> decodeOptionalSlot(const JsonNode & node)
{
	if(node.isNull())
		return std::nullopt;
	return decodeSlot(node);
}

ArtifactPosition decodeArtifactPosition(const JsonNode & node)
{
	if(!node.isNumber())
		throw std::runtime_error("VGT replay artifact position is not numeric");
	return ArtifactPosition(static_cast<int>(node.Integer()));
}

ArtifactLocation decodeArtifactLocation(CGameHandler & gameHandler, const JsonNode & node)
{
	ArtifactLocation result;
	result.artHolder = resolveObjectAlias(gameHandler.gameState(), requireString(node, "holder"));
	if(const auto * creatureSlot = findNullableField(node, "creatureSlot"))
		result.creature = decodeOptionalSlot(*creatureSlot);
	result.slot = decodeArtifactPosition(requireField(node, "slot"));
	return result;
}

std::vector<MoveArtifactInfo> decodeArtifactMoves(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay artifact moves must be a list");

	std::vector<MoveArtifactInfo> result;
	result.reserve(node.Vector().size());
	for(const auto & entry : node.Vector())
	{
		result.emplace_back(
			decodeArtifactPosition(requireField(entry, "from")),
			decodeArtifactPosition(requireField(entry, "to")),
			requireBool(entry, "askAssemble")
		);
	}
	return result;
}

std::string artifactSetSummary(const CArtifactSet & artifactSet)
{
	std::vector<std::string> entries;
	for(const auto & [slot, slotInfo] : artifactSet.artifactsWorn)
	{
		const auto * artifact = slotInfo.getArt();
		if(!artifact)
			continue;
		entries.push_back(std::to_string(slot.getNum()) + ":" + ArtifactID::encode(artifact->getTypeId().getNum()) + "#" + std::to_string(artifact->getId().getNum()));
	}
	return "[" + boost::algorithm::join(entries, ", ") + "]";
}

std::string bytesFingerprint(const std::vector<std::byte> & bytes)
{
	uint64_t hash = 14695981039346656037ULL;
	for(const auto byte : bytes)
	{
		hash ^= static_cast<uint8_t>(byte);
		hash *= 1099511628211ULL;
	}
	return std::to_string(bytes.size()) + ":" + std::to_string(hash);
}

std::string bytesHex(const std::vector<std::byte> & bytes)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(bytes.size() * 2);
	for(const auto byte : bytes)
	{
		const auto value = static_cast<uint8_t>(byte);
		result.push_back(digits[value >> 4]);
		result.push_back(digits[value & 0x0f]);
	}
	return result;
}

class ByteVectorWriter final : public IBinaryWriter
{
public:
	std::vector<std::byte> bytes;

	int write(const std::byte * data, unsigned size) override
	{
		bytes.insert(bytes.end(), data, data + size);
		return size;
	}
};

template<typename T>
std::string serializedValueFingerprint(T & value)
{
	ByteVectorWriter writer;
	BinarySerializer serializer(&writer);
	serializer & value;
	return bytesFingerprint(writer.bytes);
}

std::vector<std::byte> serializedObjectBytes(const std::shared_ptr<CGObjectInstance> & object)
{
	if(!object)
		return {};

	ByteVectorWriter writer;
	BinarySerializer serializer(&writer);
	serializer & object;
	return writer.bytes;
}

std::string serializedObjectFingerprint(const std::shared_ptr<CGObjectInstance> & object)
{
	if(!object)
		return "null";

	return bytesFingerprint(serializedObjectBytes(object));
}

void validateBulkArtifactMove(const CGameState & gameState, const BulkMoveArtifacts & pack)
{
	const auto * sourceObject = gameState.getObjInstance(pack.srcArtHolder);
	const auto * destinationObject = gameState.getObjInstance(pack.dstArtHolder);
	const auto * sourceSet = dynamic_cast<const CArtifactSet *>(sourceObject);
	const auto * destinationSet = dynamic_cast<const CArtifactSet *>(destinationObject);
	if(!sourceSet || !destinationSet)
		throw std::runtime_error("VGT replay artifact move references an object without artifacts");

	for(const auto & move : pack.artsPack0)
	{
		if(!sourceSet->getArt(move.srcPos))
			throw std::runtime_error("VGT replay artifact move source slot is empty: holder=" + std::to_string(pack.srcArtHolder.getNum()) +
				", slot=" + std::to_string(move.srcPos.getNum()) + ", worn=" + artifactSetSummary(*sourceSet));
	}
	for(const auto & move : pack.artsPack1)
	{
		if(!destinationSet->getArt(move.srcPos))
			throw std::runtime_error("VGT replay artifact move destination slot is empty: holder=" + std::to_string(pack.dstArtHolder.getNum()) +
				", slot=" + std::to_string(move.srcPos.getNum()) + ", worn=" + artifactSetSummary(*destinationSet));
	}
}

std::string armySummary(const CCreatureSet & army)
{
	std::vector<std::string> entries;
	for(const auto & [slot, stack] : army.Slots())
	{
		if(!stack)
			continue;
		entries.push_back(std::to_string(slot.getNum()) + ":" + CreatureID::encode(stack->getCreatureID().getNum()) +
			"x" + std::to_string(stack->getCount()) +
			"#xp" + std::to_string(stack->getTotalExperience()) +
			"#bonuses" + std::to_string(stack->getExportedBonusList().size()));
	}
	return "[" + boost::algorithm::join(entries, ", ") + "]";
}

std::string objectDebugSummary(const CGameState & gameState, ObjectInstanceID objectID)
{
	const auto * object = gameState.getMap().getObject(objectID);
	if(!object)
		return "missing";

	std::vector<std::string> parts;
	parts.push_back("id=" + std::to_string(objectID.getNum()));
	parts.push_back("type=" + MapObjectID::encode(object->ID.getNum()));
	parts.push_back("subtype=" + std::to_string(object->subID.getNum()));
	parts.push_back("owner=" + object->tempOwner.toString());
	parts.push_back("name=" + object->instanceName);
	parts.push_back("pos=" + object->visitablePos().toString());

	if(const auto * hero = dynamic_cast<const CGHeroInstance *>(object))
	{
		parts.push_back("hero=" + HeroTypeID::encode(hero->getHeroTypeID().getNum()));
		parts.push_back("artifacts=" + artifactSetSummary(*hero));
	}
	if(const auto * army = dynamic_cast<const CArmedInstance *>(object))
		parts.push_back("stacks=" + armySummary(*army));

	return "{" + boost::algorithm::join(parts, ", ") + "}";
}

std::string ownedHeroesSummary(const CGameState & gameState, PlayerColor player)
{
	std::vector<std::string> entries;
	for(const auto & object : gameState.getMap().getObjects())
	{
		const auto * hero = dynamic_cast<const CGHeroInstance *>(object);
		if(hero && hero->tempOwner == player)
			entries.push_back(objectDebugSummary(gameState, hero->id));
	}
	return "[" + boost::algorithm::join(entries, ", ") + "]";
}

void validateArmyHasStack(CGameHandler & gameHandler, ObjectInstanceID armyID, SlotID slot, const std::string & context)
{
	const auto * army = gameHandler.gs->getArmyInstance(armyID);
	if(!army)
		throw std::runtime_error("VGT replay army move references invalid army: " + std::to_string(armyID.getNum()));
	if(!army->hasStackAtSlot(slot))
		throw std::runtime_error("VGT replay army move source slot is empty: " + context +
			", army=" + std::to_string(armyID.getNum()) +
			", slot=" + std::to_string(slot.getNum()) +
			", object=" + objectDebugSummary(gameHandler.gameState(), armyID) +
			", ownedHeroes=" + ownedHeroesSummary(gameHandler.gameState(), army->getOwner()));
}

BulkMoveArtifacts decodeBulkArtifactMove(CGameHandler & gameHandler, const JsonNode & node)
{
	BulkMoveArtifacts pack;
	pack.interfaceOwner = decodeColor(requireString(node, "owner"));
	pack.srcArtHolder = resolveObjectAlias(gameHandler.gameState(), requireString(node, "from"));
	pack.dstArtHolder = resolveObjectAlias(gameHandler.gameState(), requireString(node, "to"));
	if(const auto * srcCreature = findNullableField(node, "fromCreatureSlot"))
		pack.srcCreature = decodeOptionalSlot(*srcCreature);
	if(const auto * dstCreature = findNullableField(node, "toCreatureSlot"))
		pack.dstCreature = decodeOptionalSlot(*dstCreature);
	pack.artsPack0 = decodeArtifactMoves(requireField(node, "movesFromSource"));
	pack.artsPack1 = decodeArtifactMoves(requireField(node, "movesFromDestination"));
	return pack;
}

std::vector<BulkMoveArtifacts> decodeBulkArtifactMoves(CGameHandler & gameHandler, const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay battle artifactMoves must be a list");

	std::vector<BulkMoveArtifacts> result;
	result.reserve(node.Vector().size());
	for(const auto & entry : node.Vector())
		result.push_back(decodeBulkArtifactMove(gameHandler, entry));
	return result;
}

struct DecodedStackLocation
{
	ObjectInstanceID owner;
	SlotID slot;
};

DecodedStackLocation decodeStackLocation(const CGameState & gameState, const JsonNode & node)
{
	return {
		resolveObjectAlias(gameState, requireString(node, "owner")),
		decodeSlot(requireField(node, "slot"))
	};
}

ObjPropertyID decodeObjPropertyValue(const CGameState & gameState, ObjProperty property, const JsonNode & node)
{
	if(node.isString())
	{
		const std::string value = node.String();
		switch(property)
		{
			case ObjProperty::OWNER:
			case ObjProperty::VISITED:
			case ObjProperty::SEERHUT_VISITED:
				return ObjPropertyID(decodeColor(value));
			case ObjProperty::VISITORS:
				return ObjPropertyID(resolveObjectAlias(gameState, value));
			case ObjProperty::ID:
				return ObjPropertyID(MapObjectID(MapObjectID::decode(value)));
			case ObjProperty::AVAILABLE_CREATURE:
				return ObjPropertyID(decodeCreature(value));
			default:
				return ObjPropertyID(NumericID(NumericID::decode(value)));
		}
	}

	if(!node.isNumber())
		throw std::runtime_error("VGT replay object property value is not a string or number");

	const auto value = static_cast<int32_t>(node.Integer());
	switch(property)
	{
		case ObjProperty::OWNER:
		case ObjProperty::VISITED:
		case ObjProperty::SEERHUT_VISITED:
			return ObjPropertyID(PlayerColor(value));
		case ObjProperty::VISITORS:
			return ObjPropertyID(ObjectInstanceID(value));
		case ObjProperty::ID:
			return ObjPropertyID(MapObjectID(value));
		case ObjProperty::AVAILABLE_CREATURE:
			return ObjPropertyID(CreatureID(value));
		case ObjProperty::OBELISK_VISITED:
			return ObjPropertyID(TeamID(value));
		default:
			return ObjPropertyID(NumericID(value));
	}
}

int64_t decodeStackAlias(const std::string & value)
{
	const std::string prefix = "stack/";
	if(!value.starts_with(prefix))
		throw std::runtime_error("Unsupported VGT stack alias: " + value);
	return std::stoll(value.substr(prefix.size()));
}

[[maybe_unused]] BattleID decodeBattleAlias(const std::string & value)
{
	const std::string prefix = "battle/";
	if(!value.starts_with(prefix))
	throw std::runtime_error("Unsupported VGT battle alias: " + value);
	return BattleID(std::stoi(value.substr(prefix.size())));
}

GiveBonus::VariantType decodeBonusTarget(const CGameState & gameState, GiveBonus::ETarget targetKind, std::string value)
{
	switch(targetKind)
	{
		case GiveBonus::ETarget::OBJECT:
			return GiveBonus::VariantType(resolveObjectAlias(gameState, value));
		case GiveBonus::ETarget::PLAYER:
			return GiveBonus::VariantType(decodePlayerColor(value));
		case GiveBonus::ETarget::BATTLE:
			return GiveBonus::VariantType(decodeBattleAlias(value));
		case GiveBonus::ETarget::HERO_COMMANDER:
		{
			const std::string suffix = "/commander";
			if(!value.ends_with(suffix))
				throw std::runtime_error("VGT hero commander bonus target lacks /commander suffix: " + value);
			value.erase(value.size() - suffix.size());
			return GiveBonus::VariantType(resolveObjectAlias(gameState, value));
		}
	}
	throw std::runtime_error("Unsupported VGT bonus target kind");
}

BattleResultAccepted::HeroBattleResults decodeBattleHeroResult(const CGameState & gameState, const JsonNode & node)
{
	BattleResultAccepted::HeroBattleResults result;
	result.heroID = resolveObjectAlias(gameState, requireString(node, "hero"));
	result.armyID = resolveObjectAlias(gameState, requireString(node, "army"));
	result.exp = static_cast<TExpType>(requireInteger(node, "experience"));
	return result;
}

[[maybe_unused]] BattleAction decodeBattleAction(const JsonNode & node)
{
	BattleAction result;
	result.side = decodeBattleSide(requireString(node, "side"));
	result.stackNumber = static_cast<ui32>(decodeStackAlias(requireString(node, "stack")));
	result.actionType = decodeActionType(requireString(node, "action"));
	result.spell = decodeSpell(requireString(node, "spell"));
	result.target.clear();

	const auto & targets = requireField(node, "target");
	if(!targets.isVector())
		throw std::runtime_error("VGT replay battle action target must be a list");

	for(const auto & target : targets.Vector())
	{
		BattleAction::DestinationInfo destination;
		destination.unitValue = -1;
		destination.hexValue = BattleHex();
		if(target.isStruct())
		{
			const auto unitIter = target.Struct().find("unit");
			if(unitIter != target.Struct().end() && unitIter->second.isString())
				destination.unitValue = static_cast<int32_t>(decodeStackAlias(unitIter->second.String()));

			const auto hexIter = target.Struct().find("hex");
			if(hexIter != target.Struct().end() && hexIter->second.isNumber())
				destination.hexValue = BattleHex(static_cast<si16>(hexIter->second.Integer()));
		}
		result.target.push_back(destination);
	}
	return result;
}

ResourceSet decodeResources(const JsonNode & values)
{
	if(!values.isVector())
		throw std::runtime_error("VGT replay resources field is not a list");

	ResourceSet result;
	for(const auto & entry : values.Vector())
	{
		const auto resource = decodeResource(requireString(entry, "resource"));
		if(resource != GameResID::NONE)
			result[resource] = static_cast<TResource>(requireInteger(entry, "amount"));
	}
	return result;
}

std::set<SpellID> decodeSpellSet(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay spell list must be a list");

	std::set<SpellID> result;
	for(const auto & entry : node.Vector())
	{
		if(!entry.isString())
			throw std::runtime_error("VGT replay spell entry is not a string");
		result.insert(decodeSpell(entry.String()));
	}
	return result;
}

std::vector<ArtifactID> decodeArtifactList(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay artifact list must be a list");

	std::vector<ArtifactID> result;
	for(const auto & entry : node.Vector())
	{
		if(!entry.isString())
			throw std::runtime_error("VGT replay artifact entry is not a string");
		result.push_back(decodeArtifact(entry.String()));
	}
	return result;
}

std::set<BuildingID> decodeBuildingSet(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay building list must be a list");

	std::set<BuildingID> result;
	for(const auto & entry : node.Vector())
	{
		if(!entry.isString())
			throw std::runtime_error("VGT replay building entry is not a string");
		result.insert(decodeBuilding(entry.String()));
	}
	return result;
}

std::vector<CreatureID> decodeCreatureList(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay creature list must be a list");

	std::vector<CreatureID> result;
	for(const auto & entry : node.Vector())
	{
		if(!entry.isString())
			throw std::runtime_error("VGT replay creature entry is not a string");
		result.push_back(decodeCreature(entry.String()));
	}
	return result;
}

std::vector<SecondarySkill> decodeSecondarySkillList(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay secondary skill list must be a list");

	std::vector<SecondarySkill> result;
	for(const auto & entry : node.Vector())
	{
		if(!entry.isString())
			throw std::runtime_error("VGT replay secondary skill entry is not a string");
		result.push_back(decodeSecondarySkill(entry.String()));
	}
	return result;
}

CSimpleArmy decodeSimpleArmy(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay army must be a list");

	CSimpleArmy result;
	result.clearSlots();
	for(const auto & entry : node.Vector())
	{
		const auto slot = SlotID(static_cast<int>(requireInteger(entry, "slot")));
		const auto creature = decodeCreature(requireString(entry, "creature"));
		const auto count = static_cast<TQuantity>(requireInteger(entry, "count"));
		result.army[slot] = std::make_pair(creature, count);
	}
	return result;
}

std::vector<std::pair<ui32, std::vector<CreatureID>>> decodeAvailableCreatures(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay available creatures must be a list");

	std::vector<std::pair<ui32, std::vector<CreatureID>>> result;
	for(const auto & entry : node.Vector())
	{
		result.emplace_back(
			static_cast<ui32>(requireInteger(entry, "available")),
			decodeCreatureList(requireField(entry, "creatures")));
	}
	return result;
}

std::vector<SetMovePoints> decodeNewTurnMovement(const CGameState & gameState, const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay newTurn movement must be a list");

	std::vector<SetMovePoints> result;
	for(const auto & entry : node.Vector())
	{
		SetMovePoints pack;
		pack.hid = resolveObjectAlias(gameState, requireString(entry, "hero"));
		pack.val = static_cast<si32>(requireInteger(entry, "value"));
		result.push_back(pack);
	}
	return result;
}

std::vector<SetMana> decodeNewTurnMana(const CGameState & gameState, const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay newTurn mana must be a list");

	std::vector<SetMana> result;
	for(const auto & entry : node.Vector())
	{
		SetMana pack;
		pack.hid = resolveObjectAlias(gameState, requireString(entry, "hero"));
		pack.mode = decodeChangeMode(requireString(entry, "mode"));
		pack.val = static_cast<si32>(requireInteger(entry, "value"));
		result.push_back(pack);
	}
	return result;
}

std::vector<SetAvailableCreatures> decodeNewTurnAvailableCreatures(const CGameState & gameState, const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay newTurn available creatures must be a list");

	std::vector<SetAvailableCreatures> result;
	for(const auto & entry : node.Vector())
	{
		SetAvailableCreatures pack;
		pack.tid = resolveObjectAlias(gameState, requireString(entry, "object"));
		pack.creatures = decodeAvailableCreatures(requireField(entry, "levels"));
		result.push_back(pack);
	}
	return result;
}

RumorState decodeRumorState(const JsonNode & node)
{
	RumorState result;
	result.type = decodeRumorType(requireString(node, "type"));
	result.last.clear();

	const auto & last = requireField(node, "last");
	if(!last.isVector())
		throw std::runtime_error("VGT replay rumor last must be a list");
	for(const auto & entry : last.Vector())
	{
		result.last[decodeRumorType(requireString(entry, "type"))] = {
			static_cast<int>(requireInteger(entry, "id")),
			static_cast<int>(requireInteger(entry, "extra"))
		};
	}
	return result;
}

Handicap decodeHandicap(const JsonNode & node)
{
	Handicap result;
	result.startBonus = decodeResources(requireField(node, "resources"));
	result.percentIncome = static_cast<int>(requireInteger(node, "incomePercent"));
	result.percentGrowth = static_cast<int>(requireInteger(node, "growthPercent"));
	return result;
}

std::set<PlayerConnectionID> decodeConnections(const JsonNode & node)
{
	if(!node.isVector())
		throw std::runtime_error("VGT replay connections field is not a list");

	std::set<PlayerConnectionID> result;
	for(const auto & entry : node.Vector())
	{
		if(!entry.isNumber())
			throw std::runtime_error("VGT replay connection id is not a number");
		result.insert(static_cast<PlayerConnectionID>(entry.Integer()));
	}
	return result;
}

SimturnsInfo decodeSimturns(const JsonNode & node)
{
	SimturnsInfo result;
	result.requiredTurns = static_cast<int>(requireInteger(node, "requiredTurns"));
	result.optionalTurns = static_cast<int>(requireInteger(node, "optionalTurns"));
	result.allowHumanWithAI = requireBool(node, "allowHumanWithAI");
	result.ignoreAlliedContacts = requireBool(node, "ignoreAlliedContacts");
	return result;
}

TurnTimerInfo decodeTimer(const JsonNode & node)
{
	TurnTimerInfo result;
	if(node.isString() && node.String() == "none")
		return result;

	result.turnTimer = static_cast<int>(requireInteger(node, "turn"));
	result.baseTimer = static_cast<int>(requireInteger(node, "base"));
	result.battleTimer = static_cast<int>(requireInteger(node, "battle"));
	result.unitTimer = static_cast<int>(requireInteger(node, "unit"));
	result.accumulatingTurnTimer = requireBool(node, "accumulatingTurn");
	result.accumulatingUnitTimer = requireBool(node, "accumulatingUnit");
	return result;
}

TurnTimerInfo decodeTurnTimerState(const JsonNode & node)
{
	TurnTimerInfo result = decodeTimer(node);
	result.isActive = requireBool(node, "active");
	result.isBattle = requireBool(node, "battleMode");
	result.remainingMovementPointsPercent = static_cast<int>(requireInteger(node, "movementPercent"));
	result.isTurnStart = requireBool(node, "turnStart");
	result.isTurnEnded = requireBool(node, "turnEnded");
	return result;
}

ExtraOptionsInfo decodeExtraOptions(const JsonNode & node)
{
	ExtraOptionsInfo result;
	result.cheatsAllowed = requireBool(node, "cheatsAllowed");
	result.unlimitedReplay = requireBool(node, "unlimitedReplay");
	return result;
}

PlayerSettings decodePlayerSettings(const JsonNode & node, PlayerColor color)
{
	PlayerSettings result;
	result.color = color;
	result.castle = decodeFaction(requireString(node, "faction"));
	result.hero = decodeHeroType(requireString(node, "hero"));
	result.heroPortrait = decodeHeroType(requireString(node, "heroPortrait"));
	result.heroNameTextId = requireString(node, "heroNameTextId");
	result.bonus = decodeStartingBonus(requireString(node, "startingBonus"));
	result.handicap = decodeHandicap(requireField(node, "handicap"));
	result.name = requireString(node, "name");
	result.connectedPlayerIDs = decodeConnections(requireField(node, "connections"));
	result.compOnly = requireBool(node, "computerOnly");
	return result;
}

std::map<PlayerColor, PlayerSettings> decodePlayerSettingsMap(const JsonNode & node)
{
	if(!node.isStruct())
		throw std::runtime_error("VGT replay players field is not a mapping");

	std::map<PlayerColor, PlayerSettings> result;
	for(const auto & entry : node.Struct())
	{
		const PlayerColor color = decodePlayerColor(entry.first);
		result[color] = decodePlayerSettings(entry.second, color);
	}
	return result;
}

std::map<PlayerColor, PlayerSettings> decodeGeneratedMapInitializationPlayerSettings(const JsonNode & players, const JsonNode * initialPlayers)
{
	auto result = decodePlayerSettingsMap(players);
	if(!initialPlayers)
		return result;

	const auto initial = decodePlayerSettingsMap(*initialPlayers);
	for(const auto & [color, settings] : initial)
	{
		auto iter = result.find(color);
		if(iter == result.end())
			continue;

		iter->second.hero = settings.hero;
		iter->second.heroPortrait = settings.heroPortrait;
		iter->second.heroNameTextId = settings.heroNameTextId;
		iter->second.bonus = settings.bonus;
	}
	return result;
}

CMapGenOptions::CPlayerSettings decodeRandomMapPlayerSettings(const JsonNode & node, PlayerColor color)
{
	CMapGenOptions::CPlayerSettings result;
	result.setColor(color);
	result.setPlayerType(decodeRandomMapPlayerType(requireString(node, "type")));
	result.setStartingTown(decodeFaction(requireString(node, "faction")));
	result.setStartingHero(decodeHeroType(requireString(node, "hero")));
	result.setTeam(decodeTeam(requireField(node, "team")));
	return result;
}

std::shared_ptr<CMapGenOptions> decodeRandomMapGenerator(const JsonNode & node)
{
	auto result = std::make_shared<CMapGenOptions>();
	result->setWidth(static_cast<si32>(requireInteger(node, "width")));
	result->setHeight(static_cast<si32>(requireInteger(node, "height")));
	result->setLevels(static_cast<int>(requireInteger(node, "levels")));
	result->setHumanOrCpuPlayerCount(static_cast<si8>(requireInteger(node, "humanOrComputerPlayers")));
	result->setTeamCount(static_cast<si8>(requireInteger(node, "teams")));
	result->setCompOnlyPlayerCount(static_cast<si8>(requireInteger(node, "computerOnlyPlayers")));
	result->setCompOnlyTeamCount(static_cast<si8>(requireInteger(node, "computerOnlyTeams")));
	result->setWaterContent(decodeRandomMapWater(requireString(node, "water")));
	result->setMonsterStrength(decodeRandomMapMonsterStrength(requireString(node, "monsters")));

	const auto & templateNode = requireField(node, "template");
	if(templateNode.isString() && !templateNode.String().empty())
		result->setMapTemplate(templateNode.String());

	for(const auto roadId : { RoadId::DIRT_ROAD, RoadId::GRAVEL_ROAD, RoadId::COBBLESTONE_ROAD })
		result->setRoadEnabled(roadId, false);

	const auto & roads = requireField(node, "roads");
	if(!roads.isVector())
		throw std::runtime_error("VGT replay random map roads must be a list");
	for(const auto & road : roads.Vector())
	{
		if(!road.isString())
			throw std::runtime_error("VGT replay random map road is not a string");
		result->setRoadEnabled(decodeRoad(road.String()), true);
	}

	const auto & players = requireField(node, "players");
	if(!players.isStruct())
		throw std::runtime_error("VGT replay random map players must be a mapping");

	std::map<PlayerColor, CMapGenOptions::CPlayerSettings> playerSettings;
	for(const auto & entry : players.Struct())
	{
		const auto color = decodePlayerColor(entry.first);
		playerSettings[color] = decodeRandomMapPlayerSettings(entry.second, color);
	}
	result->setPlayerSettings(playerSettings);
	return result;
}

bool isGeneratedMapFile(const JsonNode & map)
{
	if(hasField(map, "generator"))
		return true;

	if(const auto * source = findField(map, "source"))
		return source->isString() && source->String() == "generated-map-file";

	return false;
}

StartInfo decodeStartInfo(const JsonNode & header)
{
	StartInfo result;
	const auto & map = requireField(header, "map");
	const auto & settingsNode = requireField(header, "settings");
	const auto & players = requireField(header, "players");
	const auto initialPlayersIter = header.Struct().find("initialPlayers");
	const auto generatedMapFile = isGeneratedMapFile(map);

	result.mode = decodeStartMode(requireString(settingsNode, "start"));
	result.startTime = static_cast<time_t>(requireInteger(settingsNode, "startTime"));
	result.difficulty = decodeDifficulty(requireString(settingsNode, "difficulty"));
	result.fileURI = requireString(map, "uri");
	result.mapname = requireString(map, "name");
	result.simturnsInfo = decodeSimturns(requireField(settingsNode, "simturns"));
	result.turnTimerInfo = decodeTimer(requireField(settingsNode, "timer"));
	result.extraOptionsInfo = decodeExtraOptions(requireField(settingsNode, "extraOptions"));
	if(generatedMapFile)
		result.playerInfos = decodeGeneratedMapInitializationPlayerSettings(players, initialPlayersIter == header.Struct().end() ? nullptr : &initialPlayersIter->second);
	else
		result.playerInfos = decodePlayerSettingsMap(initialPlayersIter == header.Struct().end() ? players : initialPlayersIter->second);
	if(generatedMapFile)
		result.mapGenOptions = decodeRandomMapGenerator(requireField(map, "generator"));
	return result;
}

void setReplaySeed(const JsonNode & header)
{
	const auto & settingsNode = requireField(header, "settings");
	const auto seed = requireInteger(settingsNode, "randomSeed");
	Settings serverSettings = settings.write["server"];
	serverSettings["seed"].Integer() = seed;
}

void applyGameSettingsOverrides(CGameHandler & gameHandler, const JsonNode & header)
{
	const auto & settingsNode = requireField(header, "settings");
	const auto iter = settingsNode.Struct().find("gameSettingsOverrides");
	if(iter == settingsNode.Struct().end())
		return;

	const auto actualOverrides = gameHandler.gs->getMap().getGameSettingsOverrides();
	const auto isEmptyOverrides = [](const JsonNode & node)
	{
		return node.isNull() || (node.isStruct() && node.Struct().empty());
	};

	if(isEmptyOverrides(actualOverrides) && isEmptyOverrides(iter->second))
		return;

	if(actualOverrides.toCompactString() != iter->second.toCompactString())
		throw std::runtime_error("VGT replay game settings overrides do not match loaded map");
}

void applyMapEngineState(CGameHandler & gameHandler, const JsonNode & header)
{
	const auto & mapNode = requireField(header, "map");
	if(isGeneratedMapFile(mapNode))
	{
		if(auto * startInfo = gameHandler.gs->getStartInfo())
			startInfo->playerInfos = decodePlayerSettingsMap(requireField(header, "players"));
		if(auto * initialStartInfo = gameHandler.gs->getInitialStartInfo())
		{
			if(const auto * initialPlayers = findField(header, "initialPlayers"))
				initialStartInfo->playerInfos = decodePlayerSettingsMap(*initialPlayers);
			initialStartInfo->fileURI.clear();
			initialStartInfo->mapname.clear();
		}
	}

	if(const auto * generatorNode = findField(mapNode, "generator"))
	{
		if(auto * startInfo = gameHandler.gs->getStartInfo())
			startInfo->mapGenOptions = decodeRandomMapGenerator(*generatorNode);
	}
	if(const auto * initialGeneratorNode = findField(mapNode, "initialGenerator"))
	{
		if(auto * initialStartInfo = gameHandler.gs->getInitialStartInfo())
			initialStartInfo->mapGenOptions = decodeRandomMapGenerator(*initialGeneratorNode);
	}

	const auto * counterNode = findField(mapNode, "objectNameCounter");
	if(!counterNode)
		return;

	if(!counterNode->isNumber())
		throw std::runtime_error("VGT replay map objectNameCounter is not numeric");

	const auto counter = counterNode->Integer();
	if(counter < std::numeric_limits<si32>::min() || counter > std::numeric_limits<si32>::max())
		throw std::runtime_error("VGT replay map objectNameCounter is out of range");

	gameHandler.gs->getMap().setUniqueInstanceNameCounter(static_cast<si32>(counter));
}

void replayPack(CGameHandler & gameHandler, CPackForServer & pack, PlayerColor player)
{
	pack.player = player;
	gameHandler.handleReceivedPack(GameConnectionID::FIRST_CONNECTION, pack);
}

void applyEffectPack(CGameHandler & gameHandler, CPackForClient & pack)
{
	if(!gameHandler.gs)
		throw std::runtime_error("VGT replay cannot apply effects before game state initialization");
	gameHandler.gs->apply(pack);
}

ObjectInstanceID resolveInitialHeroState(const CGameState & gameState, const JsonNode & node)
{
	if(const auto * id = findField(node, "id"); id && id->isString())
	{
		try
		{
			return resolveObjectAlias(gameState, id->String());
		}
		catch(const std::exception &)
		{
		}
	}

	const auto owner = decodeColor(requireString(node, "owner"));
	const auto heroType = decodeHeroType(requireString(node, "type"));
	const auto position = decodePosition(requireField(node, "position"));

	for(const auto * object : gameState.getMap().getObjects())
	{
		const auto * hero = dynamic_cast<const CGHeroInstance *>(object);
		if(!hero)
			continue;
		if(hero->tempOwner != owner)
			continue;
		if(hero->getHeroTypeID() != heroType)
			continue;
		if(hero->visitablePos() != position)
			continue;
		return hero->id;
	}

	throw std::runtime_error("Unable to resolve VGT initial hero state: " + node.toCompactString());
}

void applyInitialHeroArmyState(CGameHandler & gameHandler, const JsonNode & node)
{
	const auto heroID = resolveInitialHeroState(gameHandler.gameState(), node);
	auto * army = gameHandler.gs->getArmyInstance(heroID);
	if(!army)
		throw std::runtime_error("VGT initial hero state references non-army object: " + std::to_string(heroID.getNum()));

	if(const auto * experienceNode = findField(node, "experience"))
	{
		SetHeroExperience pack;
		pack.id = heroID;
		pack.mode = ChangeValueMode::ABSOLUTE;
		pack.val = experienceNode->Integer();
		applyEffectPack(gameHandler, pack);
	}
	if(const auto * manaNode = findField(node, "mana"))
	{
		SetMana pack;
		pack.hid = heroID;
		pack.mode = ChangeValueMode::ABSOLUTE;
		pack.val = static_cast<si32>(manaNode->Integer());
		applyEffectPack(gameHandler, pack);
	}
	if(const auto * movementNode = findField(node, "movement"))
	{
		SetMovePoints pack;
		pack.hid = heroID;
		pack.val = static_cast<si32>(movementNode->Integer());
		applyEffectPack(gameHandler, pack);
	}

	if(const auto * artifactsNode = findField(node, "artifacts"))
	{
		if(!artifactsNode->isVector())
			throw std::runtime_error("VGT initial hero artifacts must be a list");
		auto * hero = gameHandler.gs->getHero(heroID);
		if(!hero)
			throw std::runtime_error("VGT initial hero artifacts reference missing hero: " + std::to_string(heroID.getNum()));

		for(const auto & artifactNode : artifactsNode->Vector())
		{
			const auto position = decodeArtifactPosition(requireField(artifactNode, "position"));
			const auto artifactType = decodeArtifact(requireString(artifactNode, "artifact"));
			const auto spellID = decodeSpell(requireString(artifactNode, "spell"));
			const bool locked = findField(artifactNode, "locked") && findField(artifactNode, "locked")->Bool();

			const auto * currentArtifact = hero->getArt(position, false);
			if(currentArtifact && currentArtifact->getTypeId() == artifactType && currentArtifact->getScrollSpellID() == spellID)
			{
				hero->artifactsWorn.find(position)->second.locked = locked;
				continue;
			}
			if(currentArtifact)
				gameHandler.gs->getMap().removeArtifactInstance(*hero, position);

			auto * artifactInstance = gameHandler.gs->getMap().createArtifact(artifactType, spellID);
			gameHandler.gs->getMap().putArtifactInstance(*hero, artifactInstance->getId(), position);
			hero->artifactsWorn.find(position)->second.locked = locked;
		}
	}

	const CSimpleArmy desiredArmy = decodeSimpleArmy(requireField(node, "army"));
	std::set<int> desiredSlots;
	for(const auto & [slotID, stack] : desiredArmy.army)
	{
		desiredSlots.insert(slotID.getNum());
		const auto desiredCreature = stack.first;
		const auto desiredCount = stack.second;

		if(!army->hasStackAtSlot(slotID))
		{
			InsertNewStack pack;
			pack.army = heroID;
			pack.slot = slotID;
			pack.type = desiredCreature;
			pack.count = desiredCount;
			applyEffectPack(gameHandler, pack);
			continue;
		}

		const auto * current = army->getStackPtr(slotID);
		if(current->getCreatureID() != desiredCreature)
		{
			SetStackType pack;
			pack.army = heroID;
			pack.slot = slotID;
			pack.type = desiredCreature;
			applyEffectPack(gameHandler, pack);
		}
		if(current->getCount() != desiredCount)
		{
			ChangeStackCount pack;
			pack.army = heroID;
			pack.slot = slotID;
			pack.mode = ChangeValueMode::ABSOLUTE;
			pack.count = desiredCount;
			applyEffectPack(gameHandler, pack);
		}
	}

	std::vector<SlotID> slotsToErase;
	for(const auto & [slotID, stack] : army->Slots())
	{
		if(stack && !vstd::contains(desiredSlots, slotID.getNum()))
			slotsToErase.push_back(slotID);
	}
	for(const auto & slotID : slotsToErase)
	{
		EraseStack pack;
		pack.army = heroID;
		pack.slot = slotID;
		applyEffectPack(gameHandler, pack);
	}
}

void applyInitialState(CGameHandler & gameHandler, const JsonNode & header)
{
	const auto * initialStateNode = findField(header, "initialState");
	if(!initialStateNode)
		return;

	const auto * heroesNode = findField(*initialStateNode, "heroes");
	if(!heroesNode)
		return;
	if(!heroesNode->isVector())
		throw std::runtime_error("VGT initialState.heroes must be a list");

	for(const auto & heroNode : heroesNode->Vector())
		applyInitialHeroArmyState(gameHandler, heroNode);
}

void applyCreatureObjectState(const std::shared_ptr<CGObjectInstance> & object, const JsonNode & node)
{
	auto creature = std::dynamic_pointer_cast<CGCreature>(object);
	if(!creature)
		throw std::runtime_error("VGT creatureState provided for non-creature object");

	if(const auto * character = findField(node, "character"))
		creature->initialCharacter = decodeCreatureCharacter(character->String());
	if(const auto * aggression = findField(node, "aggression"))
		creature->agression = static_cast<int8_t>(aggression->Integer());
	if(const auto * temppower = findField(node, "temppower"))
		creature->temppower = temppower->Integer();
	if(const auto * stacksCount = findField(node, "stacksCount"))
		creature->stacksCount = stacksCount->Integer();
	if(const auto * upgradedStackPresence = findField(node, "upgradedStackPresence"))
		creature->upgradedStackPresence = decodeUpgradedStackPresence(upgradedStackPresence->String());
	if(const auto * joiningPercentage = findField(node, "joiningPercentage"))
		creature->joiningPercentage = static_cast<int8_t>(joiningPercentage->Integer());
	if(const auto * joinOnlyForMoney = findField(node, "joinOnlyForMoney"))
		creature->joinOnlyForMoney = joinOnlyForMoney->Bool();
	if(const auto * refusedJoining = findField(node, "refusedJoining"))
		creature->refusedJoining = refusedJoining->Bool();
	if(const auto * neverFlees = findField(node, "neverFlees"))
		creature->neverFlees = neverFlees->Bool();
	if(const auto * noGrowing = findField(node, "noGrowing"))
		creature->notGrowingTeam = noGrowing->Bool();
}

[[maybe_unused]] void replayDecision(CGameHandler & gameHandler, const JsonNode & decision)
{
	const PlayerColor player = playerFromActor(requireString(decision, "actor"));
	const std::string kind = requireString(decision, "kind");

	if(kind == "endTurn")
	{
		EndTurn pack;
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "moveHero")
	{
		MoveHero pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.path = decodePath(requireField(decision, "path"));
		pack.transit = requireBool(decision, "transit");
		pack.layer = EPathfindingLayer::AUTO;
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "buildStructure")
	{
		BuildStructure pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "town"));
		pack.bid = decodeBuilding(requireString(decision, "building"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "recruitCreatures")
	{
		RecruitCreatures pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "source"));
		pack.dst = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "destination"));
		pack.crid = decodeCreature(requireString(decision, "creature"));
		pack.amount = static_cast<ui32>(requireInteger(decision, "amount"));
		pack.level = static_cast<si32>(requireInteger(decision, "level"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "arrangeStacks")
	{
		const auto & from = requireField(decision, "from");
		const auto & to = requireField(decision, "to");

		ArrangeStacks pack;
		pack.what = decodeArrangeMode(requireString(decision, "mode"));
		pack.id1 = resolveObjectAlias(gameHandler.gameState(), requireString(from, "army"));
		pack.p1 = decodeSlot(requireField(from, "slot"));
		pack.id2 = resolveObjectAlias(gameHandler.gameState(), requireString(to, "army"));
		pack.p2 = decodeSlot(requireField(to, "slot"));
		pack.val = static_cast<si32>(requireInteger(decision, "count"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "queryAnswer")
	{
		QueryReply pack;
		pack.qid = decodeQuery(requireString(decision, "query"));
		const auto answerIter = decision.Struct().find("answer");
		if(answerIter == decision.Struct().end())
			throw std::runtime_error("Missing VGT replay query answer");
		if(answerIter->second.isNull())
			pack.reply = std::nullopt;
		else
			pack.reply = static_cast<int32_t>(answerIter->second.Integer());
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "battleAction")
	{
		throw std::runtime_error("VGT battle decision replay is not implemented yet; replay must apply recorded battle effects instead");
	}

	throw std::runtime_error("Unsupported VGT decision kind: " + kind);
}

void applyArmyEffect(CGameHandler & gameHandler, const JsonNode & node)
{
	if(const auto * move = findField(node, "move"))
	{
		const auto from = decodeStackLocation(gameHandler.gameState(), requireField(*move, "from"));
		const auto to = decodeStackLocation(gameHandler.gameState(), requireField(*move, "to"));

		RebalanceStacks pack;
		pack.srcArmy = from.owner;
		pack.srcSlot = from.slot;
		pack.dstArmy = to.owner;
		pack.dstSlot = to.slot;
		pack.count = static_cast<TQuantity>(requireInteger(*move, "count"));
		validateArmyHasStack(gameHandler, pack.srcArmy, pack.srcSlot, "move");
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(const auto * swap = findField(node, "swap"))
	{
		const auto from = decodeStackLocation(gameHandler.gameState(), requireField(*swap, "from"));
		const auto to = decodeStackLocation(gameHandler.gameState(), requireField(*swap, "to"));

		SwapStacks pack;
		pack.srcArmy = from.owner;
		pack.srcSlot = from.slot;
		pack.dstArmy = to.owner;
		pack.dstSlot = to.slot;
		validateArmyHasStack(gameHandler, pack.srcArmy, pack.srcSlot, "swap source");
		validateArmyHasStack(gameHandler, pack.dstArmy, pack.dstSlot, "swap destination");
		applyEffectPack(gameHandler, pack);
		return;
	}

	const auto location = decodeStackLocation(gameHandler.gameState(), node);

	if(const auto * insert = findField(node, "insert"))
	{
		InsertNewStack pack;
		pack.army = location.owner;
		pack.slot = location.slot;
		pack.type = decodeCreature(requireString(*insert, "creature"));
		pack.count = static_cast<TQuantity>(requireInteger(*insert, "count"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(hasField(node, "erase"))
	{
		EraseStack pack;
		pack.army = location.owner;
		pack.slot = location.slot;
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(hasField(node, "creature"))
	{
		SetStackType pack;
		pack.army = location.owner;
		pack.slot = location.slot;
		pack.type = decodeCreature(requireString(node, "creature"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	ChangeStackCount pack;
	pack.army = location.owner;
	pack.slot = location.slot;
	pack.mode = decodeChangeMode(requireString(node, "mode"));
	pack.count = static_cast<TQuantity>(requireInteger(node, "count"));
	applyEffectPack(gameHandler, pack);
}

BattleID decodeBattleRecordID(const JsonNode & node, const std::optional<std::string> & parentBattleID)
{
	if(hasField(node, "id"))
		return decodeBattleAlias(requireString(node, "id"));
	if(parentBattleID)
		return decodeBattleAlias(*parentBattleID);
	throw std::runtime_error("VGT battle record has no battle id");
}

void applyBattleEffect(CGameHandler & gameHandler, const JsonNode & node, const std::optional<std::string> & parentBattleID = std::nullopt)
{
	const std::string event = requireString(node, "event");
	if(event == "resultAccepted")
	{
		if(!hasField(node, "attacker") || !hasField(node, "defender"))
			return;

		BattleResultAccepted pack;
		pack.battleID = decodeBattleRecordID(node, parentBattleID);
		pack.winnerSide = decodeBattleSide(requireString(node, "winner"));
		pack.heroResult[BattleSide::ATTACKER] = decodeBattleHeroResult(gameHandler.gameState(), requireField(node, "attacker"));
		pack.heroResult[BattleSide::DEFENDER] = decodeBattleHeroResult(gameHandler.gameState(), requireField(node, "defender"));
		applyEffectPack(gameHandler, pack);
	}
	else if(event == "resultsApplied")
	{
		if(const auto * artifactMoves = findField(node, "artifactMoves"))
		{
			if(artifactMoves->isVector())
			{
				for(auto & move : decodeBulkArtifactMoves(gameHandler, *artifactMoves))
				{
					validateBulkArtifactMove(gameHandler.gameState(), move);
					applyEffectPack(gameHandler, move);
				}
			}
		}
	}
}

void applyBattleBlock(CGameHandler & gameHandler, const JsonNode & node)
{
	const std::string battleID = requireString(node, "id");
	const JsonNode & events = requireField(node, "events");
	if(!events.isVector())
		throw std::runtime_error("VGT battle block events field is not a list");

	for(const JsonNode & record : events.Vector())
	{
		if(!record.isStruct())
			throw std::runtime_error("VGT battle block event is not a mapping");
		if(hasField(record, "event"))
		{
			applyBattleEffect(gameHandler, record, battleID);
			continue;
		}
		if(record.Struct().size() == 1 && record.Struct().begin()->first == "decision")
			continue;
		throw std::runtime_error("Unsupported VGT battle block event");
	}
}

void applyLocalState(CGameHandler & gameHandler, const JsonNode & node)
{
	const PlayerColor player = decodePlayerColor(requireString(node, "player"));
	auto * playerState = gameHandler.gs->getPlayerState(player);
	if(!playerState)
		throw std::runtime_error("VGT replay localState references unknown player: " + requireString(node, "player"));
	*playerState->playerLocalSettings = requireField(node, "data");
}

bool shouldSynthesizeDisabledTimerState(CGameHandler & gameHandler)
{
	const auto * startInfo = gameHandler.gameInfo().getStartInfo();
	return startInfo && !startInfo->turnTimerInfo.isEnabled();
}

void synthesizeDisabledTimerTurnStart(CGameHandler & gameHandler, PlayerColor player)
{
	if(!shouldSynthesizeDisabledTimerState(gameHandler))
		return;

	auto * playerState = gameHandler.gs->getPlayerState(player);
	if(!playerState)
		return;

	if(!playerState->turnTimer.isActive && !playerState->turnTimer.isTurnEnded)
		gameHandler.turnTimerHandler->onGameplayStart(player);
}

void synthesizeDisabledTimerTurnEnd(CGameHandler & gameHandler, PlayerColor player)
{
	if(!shouldSynthesizeDisabledTimerState(gameHandler))
		return;

	auto * playerState = gameHandler.gs->getPlayerState(player);
	if(!playerState)
		return;

	gameHandler.turnTimerHandler->onEndTurn(player);
}

void synthesizeDisabledTimerGameplayStart(CGameHandler & gameHandler)
{
	if(!shouldSynthesizeDisabledTimerState(gameHandler))
		return;

	for(const auto & [player, state] : gameHandler.gameState().players)
		gameHandler.turnTimerHandler->onGameplayStart(player);
}

void applyEffectRecord(CGameHandler & gameHandler, const std::string & kind, const JsonNode & node)
{
	if(kind == "decision" || kind == "query" || kind == "info")
		return;

	if(kind == "battle")
	{
		if(hasField(node, "events"))
		{
			applyBattleBlock(gameHandler, node);
			return;
		}
		applyBattleEffect(gameHandler, node);
		return;
	}

	if(kind == "localState")
	{
		applyLocalState(gameHandler, node);
		return;
	}

	if(kind == "availableHero")
	{
		SetAvailableHero pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.slotID = decodeTavernSlot(requireString(node, "slot"));
		pack.roleID = decodeTavernRole(requireString(node, "role"));
		pack.hid = decodeHeroType(requireString(node, "hero"));
		pack.army = decodeSimpleArmy(requireField(node, "army"));
		pack.replenishPoints = requireBool(node, "replenishMovement");
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "timer")
	{
		TurnTimeUpdate pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.turnTimer = decodeTurnTimerState(requireField(node, "state"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "availableArtifacts")
	{
		SetAvailableArtifacts pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.arts = decodeArtifactList(requireField(node, "artifacts"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "artifact")
	{
		if(const auto * createNode = findField(node, "create"))
		{
			NewArtifact pack;
			pack.artHolder = resolveObjectAlias(gameHandler.gameState(), requireString(*createNode, "holder"));
			pack.artId = decodeArtifact(requireString(*createNode, "artifact"));
			pack.spellId = decodeSpell(requireString(*createNode, "spell"));
			pack.pos = decodeArtifactPosition(requireField(*createNode, "position"));
			applyEffectPack(gameHandler, pack);
			return;
		}

		if(const auto * putNode = findField(node, "put"))
		{
			PutArtifact pack;
			pack.id = ArtifactInstanceID(static_cast<int>(requireInteger(*putNode, "artifactInstance")));
			pack.al = decodeArtifactLocation(gameHandler, requireField(*putNode, "to"));
			pack.askAssemble = requireBool(*putNode, "askAssemble");
			applyEffectPack(gameHandler, pack);
			return;
		}
	}

	if(kind == "rewardable")
	{
		SetRewardableConfiguration pack;
		pack.objectID = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.buildingID = decodeBuilding(requireString(node, "building"));
		JsonDeserializer handler(nullptr, requireField(node, "configuration"));
		pack.configuration.serializeJson(handler);
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "artifacts" && hasField(node, "owner"))
	{
		BulkMoveArtifacts pack = decodeBulkArtifactMove(gameHandler, node);
		validateBulkArtifactMove(gameHandler.gameState(), pack);
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "heroRecruited")
	{
		HeroRecruited pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.hid = decodeHeroType(requireString(node, "hero"));
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "town"));
		pack.tile = decodePosition(requireField(node, "tile"));
		pack.boatId = resolveObjectAlias(gameHandler.gameState(), requireString(node, "boat"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "heroOwner")
	{
		GiveHero pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.boatId = resolveObjectAlias(gameHandler.gameState(), requireString(node, "boat"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "bonus")
	{
		GiveBonus pack;
		pack.who = decodeBonusTargetKind(requireString(node, "targetKind"));
		if(pack.who == GiveBonus::ETarget::BATTLE)
			return;

		pack.id = decodeBonusTarget(gameHandler.gameState(), pack.who, requireString(node, "target"));
		const auto parsedBonus = JsonUtils::parseBonus(requireField(node, "value"));
		if(!parsedBonus)
			throw std::runtime_error("Unable to parse VGT bonus value");
		pack.bonus = *parsedBonus;
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "newTurn")
	{
		NewTurn pack;
		pack.day = static_cast<ui32>(requireInteger(node, "day"));
		pack.specialWeek = decodeWeekType(requireString(node, "week"));
		pack.creatureid = decodeCreature(requireString(node, "creature"));

		const auto & income = requireField(node, "income");
		if(!income.isVector())
			throw std::runtime_error("VGT replay newTurn income must be a list");
		for(const auto & entry : income.Vector())
			pack.playerIncome[decodePlayerColor(requireString(entry, "player"))] = decodeResources(requireField(entry, "resources"));

		if(const auto * movement = findField(node, "movement"))
			pack.heroesMovement = decodeNewTurnMovement(gameHandler.gameState(), *movement);
		if(const auto * mana = findField(node, "mana"))
			pack.heroesMana = decodeNewTurnMana(gameHandler.gameState(), *mana);
		if(const auto * available = findField(node, "availableCreatures"))
			pack.availableCreatures = decodeNewTurnAvailableCreatures(gameHandler.gameState(), *available);
		if(const auto * rumor = findField(node, "rumor"))
			pack.newRumor = decodeRumorState(*rumor);

		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "turnStart")
	{
		PlayerStartsTurn pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.queryID = decodeQuery(requireString(node, "query"));
		applyEffectPack(gameHandler, pack);
		synthesizeDisabledTimerTurnStart(gameHandler, pack.player);
		return;
	}

	if(kind == "turnEnd")
	{
		PlayerEndsTurn pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		applyEffectPack(gameHandler, pack);
		synthesizeDisabledTimerTurnEnd(gameHandler, pack.player);
		return;
	}

	if(kind == "playerEnd")
	{
		PlayerEndsGame pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.victoryLossCheckResult = decodeVictoryLossResult(requireString(node, "result"));
		pack.silentEnd = requireBool(node, "silent");
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "resources")
	{
		SetResources pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.mode = decodeChangeMode(requireString(node, "mode"));
		pack.res = decodeResources(requireField(node, "values"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "primarySkill")
	{
		SetPrimarySkill pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.which = decodePrimarySkill(requireString(node, "skill"));
		pack.mode = decodeChangeMode(requireString(node, "mode"));
		pack.val = requireInteger(node, "value");
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "experience")
	{
		SetHeroExperience pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.mode = decodeChangeMode(requireString(node, "mode"));
		pack.val = requireInteger(node, "value");
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "stackExperience")
	{
		GiveStackExperience pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "army"));
		const auto & values = requireField(node, "values");
		if(!values.isVector())
			throw std::runtime_error("VGT replay stack experience values must be a list");
		for(const auto & entry : values.Vector())
			pack.val[decodeSlot(requireField(entry, "slot"))] = requireInteger(entry, "amount");
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "secondarySkill")
	{
		SetSecSkill pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.which = decodeSecondarySkill(requireString(node, "skill"));
		pack.mode = decodeChangeMode(requireString(node, "mode"));
		pack.val = static_cast<ui16>(requireInteger(node, "value"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "townVisit")
	{
		HeroVisitCastle pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "town"));
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.startVisit = requireBool(node, "start");
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "spells")
	{
		ChangeSpells pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		const std::string mode = requireString(node, "mode");
		if(mode == "learn")
			pack.learn = 1;
		else if(mode == "forget")
			pack.learn = 0;
		else
			throw std::runtime_error("Unsupported VGT spells mode: " + mode);
		pack.spells = decodeSpellSet(requireField(node, "spells"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "mana")
	{
		SetMana pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.mode = decodeChangeMode(requireString(node, "mode"));
		pack.val = static_cast<si32>(requireInteger(node, "value"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "movementPoints")
	{
		SetMovePoints pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.val = static_cast<si32>(requireInteger(node, "value"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "visibility")
	{
		FoWChange pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.mode = decodeVisibility(requireString(node, "mode"));
		pack.tiles = decodeFowRuns(requireField(node, "runs"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "objectPosition")
	{
		ChangeObjPos pack;
		pack.objid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.nPos = decodePosition(requireField(node, "to"));
		pack.initiator = decodeColor(requireString(node, "initiator"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "remove")
	{
		RemoveObject pack;
		pack.objectID = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.initiator = decodeColor(requireString(node, "initiator"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "newObject")
	{
		const auto objectType = decodeMapObject(requireString(node, "type"));
		const auto objectSubtype = MapObjectSubID(static_cast<si32>(requireInteger(node, "subtype")));
		const auto position = decodePosition(requireField(node, "position"));
		auto object = gameHandler.createNewObject(position, objectType, objectSubtype);
		object->tempOwner = decodeColor(requireString(node, "owner"));
		if(const auto * name = findField(node, "name"); name && name->isString())
			object->instanceName = name->String();
		if(const auto * blockVisit = findField(node, "blockVisit"))
			object->blockVisit = blockVisit->Bool();
		if(const auto * removable = findField(node, "removable"))
			object->removable = removable->Bool();

		if(const auto * armyNode = findField(node, "army"))
		{
			const auto army = decodeSimpleArmy(*armyNode);
			auto armed = std::dynamic_pointer_cast<CArmedInstance>(object);
			if(!armed)
				throw std::runtime_error("VGT newObject army provided for non-army object");
			armed->clearSlots();
			for(const auto & [slotID, stack] : army.army)
				armed->setCreature(slotID, stack.first, stack.second);
		}
		if(const auto * creatureState = findField(node, "creatureState"))
			applyCreatureObjectState(object, *creatureState);

		NewObject pack;
		pack.newObject = object;
		pack.initiator = decodeColor(requireString(node, "initiator"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "move")
	{
		TryMoveHero pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.start = decodePosition(requireField(node, "from"));
		pack.end = decodePosition(requireField(node, "to"));
		pack.result = decodeMovementResult(requireString(node, "result"));
		pack.movePoints = static_cast<ui32>(requireInteger(node, "movement"));
		if(const auto * revealed = findField(node, "revealed"))
			pack.fowRevealed = decodeFowRuns(*revealed);
		if(const auto * attackedFrom = findField(node, "attackedFrom"))
			pack.attackedFrom = decodePosition(*attackedFrom);
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "town")
	{
		if(hasField(node, "build"))
		{
			NewStructures pack;
			pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "id"));
			pack.bid = decodeBuildingSet(requireField(node, "build"));
			pack.built = static_cast<si16>(requireInteger(node, "builtThisTurn"));
			applyEffectPack(gameHandler, pack);
			return;
		}

		if(hasField(node, "raze"))
		{
			RazeStructures pack;
			pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "id"));
			pack.bid = decodeBuildingSet(requireField(node, "raze"));
			pack.destroyed = static_cast<si16>(requireInteger(node, "destroyed"));
			applyEffectPack(gameHandler, pack);
			return;
		}
	}

	if(kind == "availableCreatures")
	{
		SetAvailableCreatures pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.creatures = decodeAvailableCreatures(requireField(node, "levels"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "townHeroes")
	{
		SetHeroesInTown pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(node, "town"));
		pack.visiting = resolveObjectAlias(gameHandler.gameState(), requireString(node, "visiting"));
		pack.garrison = resolveObjectAlias(gameHandler.gameState(), requireString(node, "garrison"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "objectProperty")
	{
		SetObjectProperty pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.what = decodeObjProperty(requireField(node, "property"));
		pack.identifier = decodeObjPropertyValue(gameHandler.gameState(), pack.what, requireField(node, "value"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "objectVisitors")
	{
		ChangeObjectVisitors pack;
		pack.object = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.hero = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.mode = decodeObjectVisitMode(requireString(node, "mode"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "levelUp")
	{
		HeroLevelUp pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.heroId = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.primskill = decodePrimarySkill(requireString(node, "primary"));
		pack.skills = decodeSecondarySkillList(requireField(node, "choices"));
		pack.queryID = decodeQuery(requireString(node, "query"));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "quest")
	{
		AddQuest pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
		pack.quest = QuestInfo(resolveObjectAlias(gameHandler.gameState(), requireString(node, "object")));
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "army")
	{
		applyArmyEffect(gameHandler, node);
		return;
	}

	if(kind == "visit")
	{
		HeroVisit pack;
		pack.heroId = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
		pack.objId = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.starting = requireBool(node, "start");
		if(const auto * hero = gameHandler.gameState().getHero(pack.heroId))
			pack.player = hero->getOwner();
		else
			pack.player = PlayerColor::NEUTRAL;
		applyEffectPack(gameHandler, pack);
		return;
	}

	if(kind == "unmodelled")
		throw std::runtime_error("VGT replay encountered unmodelled material pack: " + requireString(node, "pack"));

	throw std::runtime_error("Unsupported VGT effect record: " + kind);
}

bool isArmyCountOnlyRecord(const JsonNode & node)
{
	return hasField(node, "owner") && hasField(node, "slot") && hasField(node, "mode") && hasField(node, "count") &&
		!hasField(node, "creature") && !hasField(node, "insert") && !hasField(node, "erase") && !hasField(node, "move") && !hasField(node, "swap");
}

bool isArmyCreatureOnlyRecord(const JsonNode & node)
{
	return hasField(node, "owner") && hasField(node, "slot") && hasField(node, "creature") &&
		!hasField(node, "mode") && !hasField(node, "count") && !hasField(node, "insert") && !hasField(node, "erase") && !hasField(node, "move") && !hasField(node, "swap");
}

bool tryReplayArmyInsertPair(CGameHandler & gameHandler, const JsonNode & currentRecord, const JsonNode & nextRecord)
{
	if(!currentRecord.isStruct() || currentRecord.Struct().size() != 1 || !nextRecord.isStruct() || nextRecord.Struct().size() != 1)
		return false;

	const auto & current = *currentRecord.Struct().begin();
	const auto & next = *nextRecord.Struct().begin();
	if(current.first != "army" || next.first != "army")
		return false;
	if(!isArmyCountOnlyRecord(current.second) || !isArmyCreatureOnlyRecord(next.second))
		return false;
	if(requireString(current.second, "owner") != requireString(next.second, "owner"))
		return false;
	if(requireInteger(current.second, "slot") != requireInteger(next.second, "slot"))
		return false;

	const auto army = resolveObjectAlias(gameHandler.gameState(), requireString(current.second, "owner"));
	const auto slot = decodeSlot(requireField(current.second, "slot"));
	const auto * armyInstance = gameHandler.gs->getArmyInstance(army);
	if(!armyInstance || armyInstance->hasStackAtSlot(slot))
		return false;

	InsertNewStack pack;
	pack.army = army;
	pack.slot = slot;
	pack.type = decodeCreature(requireString(next.second, "creature"));
	pack.count = static_cast<TQuantity>(requireInteger(current.second, "count"));
	applyEffectPack(gameHandler, pack);
	return true;
}

void replayTranscriptDocuments(CGameHandler & gameHandler, const JsonNode & documents)
{
	if(documents.Vector().size() == 1)
		return;

	for(size_t documentIndex = 1; documentIndex < documents.Vector().size(); ++documentIndex)
	{
		const JsonNode & document = documents.Vector()[documentIndex];
		const JsonNode * records = nullptr;
		const auto actionsIter = document.Struct().find("actions");
		if(actionsIter != document.Struct().end())
			records = &actionsIter->second;
		const auto eventsIter = document.Struct().find("events");
		if(!records && eventsIter != document.Struct().end())
			records = &eventsIter->second;
		if(!records || !records->isVector())
			throw std::runtime_error("VGT replay document has no actions/events list");

		for(size_t recordIndex = 0; recordIndex < records->Vector().size(); ++recordIndex)
		{
			const auto & record = records->Vector()[recordIndex];
			if(!record.isStruct() || record.Struct().size() != 1)
				throw std::runtime_error("VGT replay record is not a one-key mapping");

			if(recordIndex + 1 < records->Vector().size() && tryReplayArmyInsertPair(gameHandler, record, records->Vector()[recordIndex + 1]))
			{
				++recordIndex;
				continue;
			}

			const auto & entry = *record.Struct().begin();
			applyEffectRecord(gameHandler, entry.first, entry.second);
		}
	}
}

void writeGameStateSave(const CGameHandler & gameHandler, const std::string & path)
{
	if(path.empty())
		return;

	const boost::filesystem::path targetPath(path);
	if(!targetPath.parent_path().empty())
		boost::filesystem::create_directories(targetPath.parent_path());

	CSaveFile save;
	gameHandler.gameState().saveGame(save);
	save.write(targetPath);
}

std::string startingBonusSummary(PlayerStartingBonus bonus)
{
	switch(bonus)
	{
	case PlayerStartingBonus::RANDOM:
		return "random";
	case PlayerStartingBonus::ARTIFACT:
		return "artifact";
	case PlayerStartingBonus::GOLD:
		return "gold";
	case PlayerStartingBonus::RESOURCE:
		return "resource";
	}
	return "unknown";
}

std::string randomMapPlayerTypeSummary(EPlayerType type)
{
	switch(type)
	{
	case EPlayerType::HUMAN:
		return "human";
	case EPlayerType::AI:
		return "ai";
	case EPlayerType::COMP_ONLY:
		return "computerOnly";
	}
	return "unknown";
}

void writePlayerSettingsSummary(std::ostream & output, const std::string & prefix, const PlayerSettings & player)
{
	output << prefix << player.color.toString()
		<< " faction=" << (player.castle == FactionID::RANDOM ? "random" : FactionID::encode(player.castle.getNum()))
		<< " hero=" << (player.hero == HeroTypeID::RANDOM ? "random" : HeroTypeID::encode(player.hero.getNum()))
		<< " bonus=" << startingBonusSummary(player.bonus)
		<< " name=" << player.name
		<< " compOnly=" << (player.compOnly ? "true" : "false")
		<< " connections=" << player.connectedPlayerIDs.size()
		<< "\n";
}

void writeMapGenSummary(std::ostream & output, const std::string & label, const CMapGenOptions * options)
{
	output << label << "=" << (options ? "present" : "none") << "\n";
	if(!options)
		return;

	output << label << ".size=" << options->getWidth() << "x" << options->getHeight() << "x" << options->getLevels()
		<< " humanOrComputer=" << static_cast<int>(options->getHumanOrCpuPlayerCount())
		<< " teams=" << static_cast<int>(options->getTeamCount())
		<< " computerOnly=" << static_cast<int>(options->getCompOnlyPlayerCount())
		<< " computerOnlyTeams=" << static_cast<int>(options->getCompOnlyTeamCount())
		<< " water=" << static_cast<int>(options->getWaterContent())
		<< " monsters=" << static_cast<int>(options->getMonsterStrength())
		<< " template=" << (options->getMapTemplate() ? options->getMapTemplate()->getId() : "")
		<< " roads=";
	for(const auto roadId : { RoadId::DIRT_ROAD, RoadId::GRAVEL_ROAD, RoadId::COBBLESTONE_ROAD })
	{
		if(options->isRoadEnabled(roadId))
			output << RoadId::encode(roadId.getNum()) << ",";
	}
	output << "\n";

	for(const auto & [color, player] : options->getPlayersSettings())
	{
		output << label << ".player." << color.toString()
			<< " type=" << randomMapPlayerTypeSummary(player.getPlayerType())
			<< " faction=" << (player.getStartingTown() == FactionID::RANDOM ? "random" : FactionID::encode(player.getStartingTown().getNum()))
			<< " hero=" << (player.getStartingHero() == HeroTypeID::RANDOM ? "random" : HeroTypeID::encode(player.getStartingHero().getNum()))
			<< " team=" << player.getTeam().getNum()
			<< "\n";
	}
}

void writeStartInfoSummary(std::ostream & output, const std::string & label, const StartInfo * startInfo)
{
	output << label << "=" << (startInfo ? "present" : "none") << "\n";
	if(!startInfo)
		return;

	output << label << ".mode=" << static_cast<int>(startInfo->mode)
		<< " difficulty=" << static_cast<int>(startInfo->difficulty)
		<< " startTime=" << static_cast<int64_t>(startInfo->startTime)
		<< " fileURI=" << startInfo->fileURI
		<< " mapname=" << startInfo->mapname
		<< "\n";
	for(const auto & [color, player] : startInfo->playerInfos)
		writePlayerSettingsSummary(output, label + ".player.", player);
	writeMapGenSummary(output, label + ".mapGen", startInfo->mapGenOptions.get());
}

void writeResourceSummary(std::ostream & output, const PlayerColor & player, const TResources & resources)
{
	output << "player." << player.toString() << ".resources=";
	for(int index = 0; index < GameConstants::RESOURCE_QUANTITY; ++index)
	{
		if(index)
			output << ",";
		output << GameResID::encode(index) << ":" << resources[static_cast<size_t>(index)];
	}
	output << "\n";
}

std::string timerSummary(const TurnTimerInfo & timer)
{
	return "turn=" + std::to_string(timer.turnTimer) +
		" base=" + std::to_string(timer.baseTimer) +
		" battle=" + std::to_string(timer.battleTimer) +
		" unit=" + std::to_string(timer.unitTimer) +
		" accumulatingTurn=" + std::string(timer.accumulatingTurnTimer ? "true" : "false") +
		" accumulatingUnit=" + std::string(timer.accumulatingUnitTimer ? "true" : "false") +
		" active=" + std::string(timer.isActive ? "true" : "false") +
		" battleMode=" + std::string(timer.isBattle ? "true" : "false") +
		" movementPercent=" + std::to_string(timer.remainingMovementPointsPercent) +
		" turnStart=" + std::string(timer.isTurnStart ? "true" : "false") +
		" turnEnded=" + std::string(timer.isTurnEnded ? "true" : "false");
}

void writeGameStateSummary(const CGameState & gameState, std::ostream & output)
{
	std::optional<int> dumpObjectBytesID;
	if(const char * value = std::getenv("VCMI_VGT_DUMP_OBJECT_BYTES"))
		dumpObjectBytesID = std::stoi(value);

	writeStartInfoSummary(output, "scenario", gameState.getStartInfo());
	writeStartInfoSummary(output, "initial", gameState.getInitialStartInfo());
	output << "day=" << gameState.getCalendar().getCurrentDay() << "\n";
	output << "objectNameCounter=" << gameState.getMap().getUniqueInstanceNameCounter() << "\n";

	for(const auto & [channelID, channel] : gameState.getMap().teleportChannels)
	{
		output << "teleportChannel id=" << channelID.getNum()
			<< " passability=" << static_cast<int>(channel->passability)
			<< " entrances=";
		for(size_t index = 0; index < channel->entrances.size(); ++index)
		{
			if(index)
				output << ",";
			output << channel->entrances[index].getNum();
		}
		output << " exits=";
		for(size_t index = 0; index < channel->exits.size(); ++index)
		{
			if(index)
				output << ",";
			output << channel->exits[index].getNum();
		}
		output << "\n";
	}

	for(const auto & [color, player] : gameState.players)
	{
		writeResourceSummary(output, color, player.resources);
		output << "player." << color.toString() << ".timer=" << timerSummary(player.turnTimer) << "\n";
	}

	for(const auto & heroID : gameState.getMap().getHeroesOnMap())
	{
		const auto * hero = gameState.getHero(heroID);
		if(!hero)
			continue;
		output << "hero id=" << heroID.getNum()
			<< " name=" << hero->instanceName
			<< " type=" << HeroTypeID::encode(hero->getHeroTypeID().getNum())
			<< " owner=" << hero->tempOwner.toString()
			<< " pos=" << hero->visitablePos().toString()
			<< " mana=" << hero->mana
			<< " movement=" << hero->movementPointsRemaining()
			<< " artifacts=" << artifactSetSummary(*hero)
			<< " army=" << armySummary(*hero)
			<< "\n";
	}

	for(const auto * town : gameState.getMap().getObjects<CGTownInstance>())
	{
		output << "town id=" << town->id.getNum()
			<< " name=" << town->getNameTranslated()
			<< " owner=" << town->tempOwner.toString()
			<< " pos=" << town->visitablePos().toString()
			<< " built=" << town->built
			<< " destroyed=" << town->destroyed
			<< " buildings=";
		bool first = true;
		for(const auto & building : town->getBuildings())
		{
			if(!first)
				output << ",";
			first = false;
			output << BuildingID::encode(building.getNum());
		}
		output << "\n";
	}

	for(const auto * creature : gameState.getMap().getObjects<CGCreature>())
	{
		output << "creature id=" << creature->id.getNum()
			<< " name=" << creature->instanceName
			<< " type=" << CreatureID::encode(creature->getCreatureID().getNum())
			<< " owner=" << creature->tempOwner.toString()
			<< " pos=" << creature->visitablePos().toString()
			<< " character=" << static_cast<int>(creature->initialCharacter)
			<< " aggression=" << static_cast<int>(creature->agression)
			<< " neverFlees=" << creature->neverFlees
			<< " noGrowing=" << creature->notGrowingTeam
			<< " temppower=" << creature->temppower
			<< " stacksCount=" << creature->stacksCount
			<< " upgradedStackPresence=" << static_cast<int>(creature->upgradedStackPresence)
			<< " joiningPercentage=" << static_cast<int>(creature->joiningPercentage)
			<< " joinOnlyForMoney=" << creature->joinOnlyForMoney
			<< " refusedJoining=" << creature->refusedJoining
			<< " formation=" << static_cast<int>(creature->formation)
			<< " army=" << armySummary(*creature)
			<< "\n";
	}

	for(const auto * object : gameState.getMap().getObjects())
	{
		if(!object)
			continue;
		const auto objectIndex = static_cast<size_t>(object->id.getNum());
		const auto * bonusNode = dynamic_cast<const CBonusSystemNode *>(object);
		const auto * teleport = dynamic_cast<const CGTeleport *>(object);
		const auto * rewardable = dynamic_cast<const CRewardableObject *>(object);
		const auto * armed = dynamic_cast<const CArmedInstance *>(object);
		output << "object id=" << object->id.getNum()
			<< " name=" << object->instanceName
			<< " type=" << MapObjectID::encode(object->ID.getNum())
			<< " subtype=" << object->subID.getNum()
			<< " owner=" << object->tempOwner.toString()
			<< " pos=" << object->visitablePos().toString()
			<< " blockVisit=" << object->blockVisit
			<< " removable=" << object->removable
			<< " appearance=" << (object->appearance ? object->appearance->stringID : "")
			<< " exportedBonuses=" << (bonusNode ? bonusNode->getExportedBonusList().size() : 0)
			<< " teleportChannel=" << (teleport ? teleport->channel.getNum() : -1)
			<< " teleportEntrance=" << (teleport && teleport->isEntrance() ? "true" : "false")
			<< " teleportExit=" << (teleport && teleport->isExit() ? "true" : "false")
			<< " bonusNodeType=" << (bonusNode ? static_cast<int>(bonusNode->getNodeType()) : -1)
			<< " armedFormation=" << (armed ? static_cast<int>(armed->formation) : -1)
			<< " armedArmy=" << (armed ? armySummary(*armed) : "[]")
			<< "\n";
		if(objectIndex < gameState.getMap().objects.size())
		{
			output << "objectSerialized id=" << object->id.getNum()
				<< " bytes=" << serializedObjectFingerprint(gameState.getMap().objects[objectIndex])
				<< "\n";
			if(dumpObjectBytesID && *dumpObjectBytesID == object->id.getNum())
			{
				output << "objectSerializedBytes id=" << object->id.getNum()
					<< " hex=" << bytesHex(serializedObjectBytes(gameState.getMap().objects[objectIndex]))
					<< "\n";
			}
		}
		if(rewardable)
		{
			JsonNode configuration;
			JsonSerializer handler(nullptr, configuration);
			const_cast<CRewardableObject *>(rewardable)->configuration.serializeJson(handler);
			output << "rewardable id=" << object->id.getNum()
				<< " cleared=" << (rewardable->isOnceVisitableObjectCleared() ? "true" : "false")
				<< " configuration=" << configuration.toCompactString()
				<< "\n";
		}
	}
}

void writeFullSaveHandlerSummary(const std::string & inputSave, std::ostream & output)
{
	ReplayGameServer replayServer;
	CGameHandler gameHandler(replayServer);
	replayServer.attach(gameHandler);

	gameHandler.gs = std::make_shared<CGameState>();
	gameHandler.gs->preInit(LIBRARY);
	gameHandler.randomizer = std::make_unique<GameRandomizer>(*gameHandler.gs);

	CLoadFile loadFile(inputSave, gameHandler.gs.get());
	gameHandler.gs->loadGame(loadFile);
	loadFile.load(gameHandler);

	output << "handler=present\n";
	output << "handler.QID=" << gameHandler.QID.getNum() << "\n";
	output << "handler.randomizer=" << serializedValueFingerprint(*gameHandler.randomizer) << "\n";
	output << "handler.battles=" << serializedValueFingerprint(*gameHandler.battles) << "\n";
	output << "handler.heroPool=" << serializedValueFingerprint(*gameHandler.heroPool) << "\n";
	output << "handler.playerMessages=" << serializedValueFingerprint(*gameHandler.playerMessages) << "\n";
	output << "handler.turnOrder=" << serializedValueFingerprint(*gameHandler.turnOrder) << "\n";
	output << "handler.turnTimer=" << serializedValueFingerprint(*gameHandler.turnTimerHandler) << "\n";
	output << "handler.statistics=" << serializedValueFingerprint(*gameHandler.statistics) << "\n";
}
}

int dumpVGTGameStateSummary(const VGTGameStateSummaryOptions & options)
{
	CMapHeader savedHeader;
	StartInfo savedStartInfo;
	{
		CLoadFile preambleLoadFile(options.inputSave, nullptr);
		preambleLoadFile.load(savedHeader);
		if(preambleLoadFile.hasFeature(ESerializationVersion::NO_RAW_POINTERS_IN_SERIALIZER))
			preambleLoadFile.load(savedStartInfo);
		else
		{
			auto legacyStartInfo = std::make_shared<StartInfo>();
			preambleLoadFile.load(legacyStartInfo);
			savedStartInfo = *legacyStartInfo;
		}
	}

	CGameState gameState;
	gameState.preInit(LIBRARY);
	CLoadFile loadFile(options.inputSave, &gameState);
	gameState.loadGame(loadFile);
	gameState.preInit(LIBRARY);

	std::ofstream output(options.outputSummary);
	if(!output)
		throw std::runtime_error("Unable to write VGT game state summary: " + options.outputSummary);
	output << "preamble.mapName=" << savedHeader.name.toString() << "\n";
	output << "preamble.mapDescription=" << savedHeader.description.toString() << "\n";
	writeStartInfoSummary(output, "preamble.start", &savedStartInfo);
	writeGameStateSummary(gameState, output);
	try
	{
		writeFullSaveHandlerSummary(options.inputSave, output);
	}
	catch(const std::exception & e)
	{
		output << "handler=unavailable reason=" << e.what() << "\n";
	}
	return 0;
}

int normalizeVGTGameStateSave(const VGTGameStateNormalizeOptions & options)
{
	CGameState gameState;
	gameState.preInit(LIBRARY);
	CLoadFile loadFile(options.inputSave, &gameState);
	gameState.loadGame(loadFile);
	gameState.preInit(LIBRARY);

	const boost::filesystem::path targetPath(options.outputSave);
	if(!targetPath.parent_path().empty())
		boost::filesystem::create_directories(targetPath.parent_path());

	CSaveFile save;
	gameState.saveGame(save);
	save.write(targetPath);
	return 0;
}

int replayVGTJson(const VGTReplayOptions & options)
{
	const JsonNode documents = readJsonFile(options.inputJson);
	if(!documents.isVector() || documents.Vector().empty())
		throw std::runtime_error("VGT replay JSON must contain transcript documents");

	const JsonNode & header = documents.Vector().front();
	setReplaySeed(header);
	StartInfo startInfo = decodeStartInfo(header);

	ReplayGameServer replayServer;
	CGameHandler gameHandler(replayServer);
	replayServer.attach(gameHandler);

	Load::ProgressAccumulator progress;
	gameHandler.init(&startInfo, progress);
	applyMapEngineState(gameHandler, header);
	applyInitialState(gameHandler, header);
	applyGameSettingsOverrides(gameHandler, header);
	synthesizeDisabledTimerGameplayStart(gameHandler);
	replayTranscriptDocuments(gameHandler, documents);
	gameHandler.saveToFile(options.outputSave);
	writeGameStateSave(gameHandler, options.outputGameStateSave);
	return 0;
}
