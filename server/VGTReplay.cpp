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

#include "../lib/CConfigHandler.h"
#include "../lib/LoadProgress.h"
#include "../lib/StartInfo.h"
#include "../lib/constants/StringConstants.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/json/JsonBonus.h"
#include "../lib/json/JsonNode.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/networkPacks/PacksForClient.h"
#include "../lib/networkPacks/PacksForClientBattle.h"
#include "../lib/networkPacks/PacksForServer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
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
	if(value == "neutral")
		return PlayerColor::NEUTRAL;
	if(value == "spectator")
		return PlayerColor::SPECTATOR;
	return decodePlayerColor(value);
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
	result.turnTimer = static_cast<int>(requireInteger(node, "turn"));
	result.baseTimer = static_cast<int>(requireInteger(node, "base"));
	result.battleTimer = static_cast<int>(requireInteger(node, "battle"));
	result.unitTimer = static_cast<int>(requireInteger(node, "unit"));
	result.accumulatingTurnTimer = requireBool(node, "accumulatingTurn");
	result.accumulatingUnitTimer = requireBool(node, "accumulatingUnit");
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

StartInfo decodeStartInfo(const JsonNode & header)
{
	StartInfo result;
	const auto & map = requireField(header, "map");
	const auto & settingsNode = requireField(header, "settings");
	const auto & players = requireField(header, "players");

	result.mode = decodeStartMode(requireString(settingsNode, "start"));
	result.startTime = static_cast<time_t>(requireInteger(settingsNode, "startTime"));
	result.difficulty = decodeDifficulty(requireString(settingsNode, "difficulty"));
	result.fileURI = requireString(map, "uri");
	result.mapname = requireString(map, "name");
	result.simturnsInfo = decodeSimturns(requireField(settingsNode, "simturns"));
	result.turnTimerInfo = decodeTimer(requireField(settingsNode, "timer"));
	result.extraOptionsInfo = decodeExtraOptions(requireField(settingsNode, "extraOptions"));

	if(!players.isStruct())
		throw std::runtime_error("VGT replay players field is not a mapping");

	result.playerInfos.clear();
	for(const auto & entry : players.Struct())
	{
		const PlayerColor color = decodePlayerColor(entry.first);
		result.playerInfos[color] = decodePlayerSettings(entry.second, color);
	}
	return result;
}

void setReplaySeed(const JsonNode & header)
{
	const auto & settingsNode = requireField(header, "settings");
	const auto seed = requireInteger(settingsNode, "randomSeed");
	Settings serverSettings = settings.write["server"];
	serverSettings["seed"].Integer() = seed;
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

void applyBattleEffect(CGameHandler & gameHandler, const JsonNode & node)
{
	const std::string event = requireString(node, "event");
	if(event == "resultAccepted")
	{
		if(!hasField(node, "attacker") || !hasField(node, "defender"))
			return;

		BattleResultAccepted pack;
		pack.battleID = decodeBattleAlias(requireString(node, "id"));
		pack.winnerSide = decodeBattleSide(requireString(node, "winner"));
		pack.heroResult[BattleSide::ATTACKER] = decodeBattleHeroResult(gameHandler.gameState(), requireField(node, "attacker"));
		pack.heroResult[BattleSide::DEFENDER] = decodeBattleHeroResult(gameHandler.gameState(), requireField(node, "defender"));
		applyEffectPack(gameHandler, pack);
	}
}

void applyEffectRecord(CGameHandler & gameHandler, const std::string & kind, const JsonNode & node)
{
	if(kind == "decision" || kind == "query" || kind == "info")
		return;

	if(kind == "battle")
	{
		applyBattleEffect(gameHandler, node);
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

	if(kind == "availableArtifacts")
	{
		SetAvailableArtifacts pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(node, "object"));
		pack.arts = decodeArtifactList(requireField(node, "artifacts"));
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
		return;
	}

	if(kind == "turnEnd")
	{
		PlayerEndsTurn pack;
		pack.player = decodePlayerColor(requireString(node, "player"));
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

		for(const auto & record : records->Vector())
		{
			if(!record.isStruct() || record.Struct().size() != 1)
				throw std::runtime_error("VGT replay record is not a one-key mapping");

			const auto & entry = *record.Struct().begin();
			applyEffectRecord(gameHandler, entry.first, entry.second);
		}
	}
}
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
	replayTranscriptDocuments(gameHandler, documents);
	gameHandler.saveToFile(options.outputSave);
	return 0;
}
