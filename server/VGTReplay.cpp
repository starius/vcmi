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
#include "VGTDiscovery.h"

#include "battles/BattleProcessor.h"
#include "processors/HeroPoolProcessor.h"
#include "processors/PlayerMessageProcessor.h"
#include "processors/TurnOrderProcessor.h"
#include "queries/QueriesProcessor.h"
#include "queries/MapQueries.h"
#include "TurnTimerHandler.h"

#include "../lib/CPlayerState.h"
#include "../lib/CStack.h"
#include "../lib/CConfigHandler.h"
#include "../lib/LoadProgress.h"
#include "../lib/StartInfo.h"
#include "../lib/callback/GameRandomizer.h"
#include "../lib/campaign/CampaignState.h"
#include "../lib/constants/StringConstants.h"
#include "../lib/entities/artifact/CArtifactInstance.h"
#include "../lib/entities/artifact/CArtifactSet.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/gameState/GameStatistics.h"
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
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
std::string canonicalObjectAlias(const CGObjectInstance & object);

thread_local VGTDiscoveryTracker replayDiscoveryTracker;
thread_local std::vector<std::string> replayDerivedDiscoveries;

class ReplayGameServer : public IGameServer
{
	CGameHandler * gameHandler = nullptr;
	EServerState state = EServerState::GAMEPLAY;
	std::string expectedTurnStateDirectory;
	std::string outputTurnStateDirectory;
	std::string capturedBattleOutcomePath;
	JsonNode capturedBattleOutcomes;
	int replayedTurnStates = 0;

	std::string turnStateFileName(PlayerColor player) const
	{
		std::ostringstream result;
		result << "turn-" << std::setfill('0') << std::setw(6) << replayedTurnStates
			<< "-day-" << std::setw(4) << gameHandler->gameState().getCalendar().getCurrentDay()
			<< "-" << player.toString() << ".vsgm1";
		return result.str();
	}

	std::vector<std::byte> serializeTurnState() const
	{
		CSaveFile save;
		gameHandler->gameState().saveGame(save);
		save.save(*gameHandler);
		return save.currentContent();
	}

	std::vector<std::byte> readTurnState(const boost::filesystem::path & path) const
	{
		std::ifstream input(path.string(), std::ios::binary | std::ios::ate);
		if(!input)
			throw std::runtime_error("Missing expected VGT turn state: " + path.string());

		const auto size = input.tellg();
		if(size < 0)
			throw std::runtime_error("Unable to determine VGT turn state size: " + path.string());
		std::vector<std::byte> result(static_cast<size_t>(size));
		input.seekg(0);
		input.read(reinterpret_cast<char *>(result.data()), size);
		if(!input)
			throw std::runtime_error("Unable to read expected VGT turn state: " + path.string());
		return result;
	}

	void writeTurnState(const boost::filesystem::path & path, const std::vector<std::byte> & data) const
	{
		if(!path.parent_path().empty())
			boost::filesystem::create_directories(path.parent_path());
		std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char *>(data.data()), data.size());
		if(!output)
			throw std::runtime_error("Unable to write replayed VGT turn state: " + path.string());
	}

	void processTurnState(PlayerColor player)
	{
		++replayedTurnStates;
		if(expectedTurnStateDirectory.empty() && outputTurnStateDirectory.empty())
			return;
		if(const char * compareFrom = std::getenv("VCMI_VGT_REPLAY_COMPARE_FROM_TURN"))
		{
			if(replayedTurnStates < std::stoi(compareFrom))
				return;
		}

		const std::string fileName = turnStateFileName(player);
		const auto actual = serializeTurnState();

		if(!outputTurnStateDirectory.empty())
			writeTurnState(boost::filesystem::path(outputTurnStateDirectory) / fileName, actual);
		if(expectedTurnStateDirectory.empty())
			return;

		const boost::filesystem::path expectedPath = boost::filesystem::path(expectedTurnStateDirectory) / fileName;
		const auto expected = readTurnState(expectedPath);
		if(actual == expected)
		{
			logGlobal->info("VGT turn state matches '%s'", expectedPath.string());
			return;
		}
		if(const char * mismatchPath = std::getenv("VCMI_VGT_REPLAY_MISMATCH_SAVE"); mismatchPath && *mismatchPath)
			writeTurnState(boost::filesystem::path(mismatchPath), actual);

		const size_t commonSize = std::min(actual.size(), expected.size());
		size_t firstDifference = 0;
		while(firstDifference < commonSize && actual[firstDifference] == expected[firstDifference])
			++firstDifference;

		std::ostringstream message;
		message << "VGT turn state mismatch: " << expectedPath.string()
			<< ", expected size " << expected.size()
			<< ", replayed size " << actual.size()
			<< ", first differing byte " << firstDifference;
		if(firstDifference < commonSize)
		{
			message << " (expected " << std::to_integer<unsigned>(expected[firstDifference])
				<< ", replayed " << std::to_integer<unsigned>(actual[firstDifference]) << ")";
		}
		throw std::runtime_error(message.str());
	}

	void captureBattleOutcome(const BattleResult & result)
	{
		if(capturedBattleOutcomePath.empty())
			return;
		const auto * battle = gameHandler->gameState().getBattle(result.battleID);
		if(!battle || !gameHandler->randomizer)
			throw std::runtime_error("Unable to capture VGT battle outcome state");

		JsonNode snapshot;
		snapshot["battle"].Integer() = result.battleID.getNum();
		snapshot["survivors"].Struct();
		snapshot["createdUnits"].Struct();
		snapshot["mana"].Struct();
		for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		{
			const auto & battleSide = battle->getSide(side);
			const auto * hero = battle->getSideHero(side);
			if(!hero)
				continue;
			const int64_t change = std::min(hero->mana, battleSide.initialMana) - battleSide.initialMana;
			if(change != 0)
				snapshot["mana"][side == BattleSide::ATTACKER ? "attacker" : "defender"].Integer() = change;
		}
		for(const auto * stack : battle->battleGetAllStacks(true))
		{
			if(!stack || stack->summoned || stack->isTurret())
				continue;
			const int count = std::max(0, stack->getCount() - stack->health.getResurrected());
			if(count > 0)
			{
				snapshot["survivors"][std::to_string(stack->unitId())].Integer() = count;
				if(stack->unitSlot() == SlotID::SUMMONED_SLOT_PLACEHOLDER)
				{
					auto & created = snapshot["createdUnits"][std::to_string(stack->unitId())];
					created["creature"].String() = CreatureID::encode(stack->creatureId().getNum());
					created["count"].Integer() = stack->unitBaseAmount();
					created["hex"].Integer() = stack->getPosition().toInt();
				}
			}
		}

		std::set<ObjectInstanceID> participants;
		std::set<HeroTypeID> heroTypes;
		for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		{
			if(const auto * army = battle->getSideArmy(side))
				participants.insert(army->id);
			if(const auto * hero = battle->getSideHero(side))
			{
				participants.insert(hero->id);
				if(hero->getHeroTypeID().hasValue())
					heroTypes.insert(hero->getHeroTypeID());
			}
		}
		snapshot["continuation"] = gameHandler->randomizer->toVGTBattleJson(participants, heroTypes);
		capturedBattleOutcomes.Vector().push_back(std::move(snapshot));
	}

public:
	void configureTurnStates(const std::string & expectedDirectory, const std::string & outputDirectory)
	{
		expectedTurnStateDirectory = expectedDirectory;
		outputTurnStateDirectory = outputDirectory;
	}

	void configureBattleOutcomeCapture(const std::string & path)
	{
		capturedBattleOutcomePath = path;
		capturedBattleOutcomes.Vector();
	}

	void writeCapturedBattleOutcomes() const
	{
		if(capturedBattleOutcomePath.empty())
			return;
		const boost::filesystem::path path(capturedBattleOutcomePath);
		if(!path.parent_path().empty())
			boost::filesystem::create_directories(path.parent_path());
		std::ofstream output(path.string(), std::ios::out | std::ios::trunc);
		output << capturedBattleOutcomes.toCompactString() << '\n';
		if(!output)
			throw std::runtime_error("Unable to write captured VGT battle outcomes: " + path.string());
	}

	void verifyTurnStatesComplete() const
	{
		if(expectedTurnStateDirectory.empty())
			return;

		const boost::filesystem::path directory(expectedTurnStateDirectory);
		if(!boost::filesystem::is_directory(directory))
			throw std::runtime_error("Expected VGT turn-state directory does not exist: " + directory.string());

		int expectedFiles = 0;
		for(const auto & entry : boost::filesystem::directory_iterator(directory))
		{
			const std::string fileName = entry.path().filename().string();
			if(boost::filesystem::is_regular_file(entry.path()) && fileName.starts_with("turn-") && fileName.ends_with(".vsgm1"))
				++expectedFiles;
		}
		if(expectedFiles != replayedTurnStates)
		{
			throw std::runtime_error(
				"VGT turn-state count mismatch: expected directory contains " + std::to_string(expectedFiles) +
				", replay produced " + std::to_string(replayedTurnStates));
		}
	}

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
		if(auto * move = dynamic_cast<TryMoveHero *>(&pack))
		{
			const auto * hero = gameHandler->gameState().getHero(move->id);
			if(hero && hero->tempOwner.isValidPlayer())
			{
				for(const ObjectInstanceID objectID : replayDiscoveryTracker.discoverInTiles(
					gameHandler->gameState(), hero->tempOwner, move->fowRevealed))
				{
					if(const auto * object = gameHandler->gameState().getMap().getObject(objectID))
						replayDerivedDiscoveries.push_back(canonicalObjectAlias(*object));
				}
			}
		}
		else if(auto * fog = dynamic_cast<FoWChange *>(&pack);
			fog && fog->mode == ETileVisibility::REVEALED && fog->player.isValidPlayer())
		{
			replayDiscoveryTracker.discoverInTiles(gameHandler->gameState(), fog->player, fog->tiles);
		}
		if(auto * result = dynamic_cast<BattleResult *>(&pack))
			captureBattleOutcome(*result);
		gameHandler->gs->apply(pack);
		if(auto * end = dynamic_cast<PlayerEndsTurn *>(&pack))
			processTurnState(end->player);
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

std::string colorAlias(PlayerColor value)
{
	if(value == PlayerColor::CANNOT_DETERMINE)
		return "cannotDetermine";
	if(value == PlayerColor::UNFLAGGABLE)
		return "unflaggable";
	if(value == PlayerColor::NEUTRAL)
		return "neutral";
	if(value == PlayerColor::SPECTATOR)
		return "spectator";
	if(!value.isValidPlayer())
		return "invalid";
	return value.toString();
}

std::string normalizeScopedIdentifier(std::string value)
{
	std::replace(value.begin(), value.end(), '/', ':');
	return value;
}

bool isNoneIdentifier(const std::string & value)
{
	const auto identifier = normalizeScopedIdentifier(value);
	return identifier == "none" || identifier == "core:none";
}

HeroTypeID decodeHeroType(const std::string & value)
{
	if(isNoneIdentifier(value))
		return HeroTypeID::NONE;
	if(value == "random")
		return HeroTypeID::RANDOM;
	if(value == "campaignStrongest")
		return HeroTypeID::CAMP_STRONGEST;
	if(value == "campaignGenerated")
		return HeroTypeID::CAMP_GENERATED;
	if(value == "campaignRandom")
		return HeroTypeID::CAMP_RANDOM;
	return HeroTypeID(HeroTypeID::decode(normalizeScopedIdentifier(value)));
}

FactionID decodeFaction(const std::string & value)
{
	if(isNoneIdentifier(value))
		return FactionID::NONE;
	if(value == "random")
		return FactionID::RANDOM;
	return FactionID(FactionID::decode(normalizeScopedIdentifier(value)));
}

GameResID decodeResource(const std::string & value)
{
	if(isNoneIdentifier(value))
		return GameResID::NONE;
	return GameResID(GameResID::decode(normalizeScopedIdentifier(value)));
}

SpellID decodeSpell(const std::string & value)
{
	if(isNoneIdentifier(value))
		return SpellID::NONE;
	return SpellID(SpellID::decode(normalizeScopedIdentifier(value)));
}

SecondarySkill decodeSecondarySkill(const std::string & value)
{
	if(isNoneIdentifier(value))
		return SecondarySkill::NONE;
	return SecondarySkill(SecondarySkill::decode(normalizeScopedIdentifier(value)));
}

PrimarySkill decodePrimarySkill(const std::string & value)
{
	if(isNoneIdentifier(value))
		return PrimarySkill::NONE;
	return PrimarySkill(PrimarySkill::decode(normalizeScopedIdentifier(value)));
}

CreatureID decodeCreature(const std::string & value)
{
	if(isNoneIdentifier(value))
		return CreatureID::NONE;
	return CreatureID(CreatureID::decode(normalizeScopedIdentifier(value)));
}

ArtifactID decodeArtifact(const std::string & value)
{
	if(isNoneIdentifier(value))
		return ArtifactID::NONE;
	return ArtifactID(ArtifactID::decode(normalizeScopedIdentifier(value)));
}

BuildingID decodeBuilding(const std::string & value)
{
	if(isNoneIdentifier(value))
		return BuildingID::NONE;

	std::string identifier = normalizeScopedIdentifier(value);
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

EArmyFormation decodeArmyFormation(const std::string & value)
{
	if(value == "loose") return EArmyFormation::LOOSE;
	if(value == "tight") return EArmyFormation::TIGHT;
	throw std::runtime_error("Unsupported VGT army formation: " + value);
}

ManageBackpackArtifacts::ManageCmd decodeBackpackManageCommand(const std::string & value)
{
	if(value == "scrollLeft") return ManageBackpackArtifacts::ManageCmd::SCROLL_LEFT;
	if(value == "scrollRight") return ManageBackpackArtifacts::ManageCmd::SCROLL_RIGHT;
	if(value == "sortBySlot") return ManageBackpackArtifacts::ManageCmd::SORT_BY_SLOT;
	if(value == "sortByClass") return ManageBackpackArtifacts::ManageCmd::SORT_BY_CLASS;
	if(value == "sortByCost") return ManageBackpackArtifacts::ManageCmd::SORT_BY_COST;
	throw std::runtime_error("Unsupported VGT backpack command: " + value);
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
	if(isNoneIdentifier(value))
		return RoadId::NO_ROAD;

	const auto identifier = normalizeScopedIdentifier(value);
	const std::string fallbackPrefix = "road:";
	if(identifier.starts_with(fallbackPrefix))
		return RoadId(std::stoi(identifier.substr(fallbackPrefix.size())));

	return RoadId(RoadId::decode(identifier));
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

EBattleResult decodeBattleResult(const std::string & value)
{
	if(value == "normal")
		return EBattleResult::NORMAL;
	if(value == "escape")
		return EBattleResult::ESCAPE;
	if(value == "surrender")
		return EBattleResult::SURRENDER;
	throw std::runtime_error("Unsupported VGT battle result: " + value);
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
	if(!node.isVector() || (node.Vector().size() != 2 && node.Vector().size() != 3))
		throw std::runtime_error("VGT replay position must be a two- or three-value list");

	return int3(
		static_cast<int>(node.Vector()[0].Integer()),
		static_cast<int>(node.Vector()[1].Integer()),
		node.Vector().size() == 3 ? static_cast<int>(node.Vector()[2].Integer()) : 0);
}

int3 directionDelta(const std::string & direction)
{
	if(direction == "N") return int3(0, -1, 0);
	if(direction == "NE") return int3(1, -1, 0);
	if(direction == "E") return int3(1, 0, 0);
	if(direction == "SE") return int3(1, 1, 0);
	if(direction == "S") return int3(0, 1, 0);
	if(direction == "SW") return int3(-1, 1, 0);
	if(direction == "W") return int3(-1, 0, 0);
	if(direction == "NW") return int3(-1, -1, 0);
	throw std::runtime_error("Unsupported VGT movement direction: " + direction);
}

std::vector<int3> decodeMoveSteps(const int3 & start, const JsonNode & decision)
{
	const std::string encoded = requireString(decision, "steps");
	std::stringstream stream(encoded);
	std::string token;
	int3 current = start;
	std::vector<int3> result;
	while(stream >> token)
	{
		int count = 1;
		if(const auto marker = token.find('*'); marker != std::string::npos)
		{
			count = std::stoi(token.substr(marker + 1));
			token.erase(marker);
		}
		if(count <= 0)
			throw std::runtime_error("VGT movement run length must be positive");
		const auto delta = directionDelta(token);
		for(int index = 0; index < count; ++index)
		{
			current += delta;
			result.push_back(current);
		}
	}
	if(result.empty())
		throw std::runtime_error("VGT movement steps must not be empty");
	if(result.back() != decodePosition(requireField(decision, "to")))
		throw std::runtime_error("VGT movement steps do not end at the declared destination");
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
	name.erase(std::unique(name.begin(), name.end(), [](char left, char right)
	{
		return left == '-' && right == '-';
	}), name.end());

	while(!name.empty() && name.front() == '-')
		name.erase(name.begin());
	while(!name.empty() && name.back() == '-')
		name.pop_back();
	return name;
}

std::string objectAliasType(MapObjectID id)
{
	auto type = MapObjectID::encode(id.getNum());
	std::replace(type.begin(), type.end(), ':', '/');
	const std::string corePrefix = "core/";
	if(type.starts_with(corePrefix))
		type.erase(0, corePrefix.size());
	return type;
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
	const auto marker = alias.rfind('@');
	if(marker == std::string::npos)
		return std::nullopt;

	std::vector<int> values;
	std::stringstream stream(alias.substr(marker + 1));
	std::string token;
	while(std::getline(stream, token, '.'))
		values.push_back(std::stoi(token));
	if(values.size() != 2 && values.size() != 3)
		return std::nullopt;
	return int3(values[0], values[1], values.size() == 3 ? values[2] : 0);
}

std::string objectLocation(const int3 & position)
{
	std::string result = "@" + std::to_string(position.x) + "." + std::to_string(position.y);
	if(position.z != 0)
		result += "." + std::to_string(position.z);
	return result;
}

bool genericObjectAliasType(const std::string & type)
{
	return type == "creatureGeneratorCommon" || type.starts_with("shrineOfMagicLevel");
}

bool equivalentAliasWords(const std::string & left, const std::string & right)
{
	auto compact = [](std::string value)
	{
		value = sanitizedAliasName(std::move(value));
		value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
		return value;
	};
	return compact(left) == compact(right);
}

std::string canonicalObjectAlias(const CGObjectInstance & object)
{
	if(const auto * hero = dynamic_cast<const CGHeroInstance *>(&object))
	{
		const std::string owner = object.tempOwner.isValidPlayer() ? object.tempOwner.toString() : "neutral";
		if(hero->getHeroTypeID().hasValue())
		{
			auto type = HeroTypeID::encode(hero->getHeroTypeID().getNum());
			std::replace(type.begin(), type.end(), ':', '/');
			const std::string corePrefix = "core/";
			if(type.starts_with(corePrefix))
				type.erase(0, corePrefix.size());
			return owner + "/" + type;
		}
		std::string name = sanitizedAliasName(object.getObjectName());
		if(name.empty())
			name = sanitizedAliasName(object.instanceName);
		if(name.empty())
			name = "id-" + std::to_string(object.id.getNum());
		return owner + "/" + name;
	}

	std::string type = dynamic_cast<const CGTownInstance *>(&object) ? "town" : objectAliasType(object.ID);
	if(type.empty())
		type = "unknown";
	const std::string owner = object.tempOwner.isValidPlayer() ? object.tempOwner.toString() : "";
	std::string name = sanitizedAliasName(object.getObjectName());
	if(name.empty())
		name = sanitizedAliasName(object.instanceName);
	if(name.empty())
		name = "id-" + std::to_string(object.id.getNum());
	const auto typeName = type.substr(type.rfind('/') == std::string::npos ? 0 : type.rfind('/') + 1);
	std::string result;
	if(equivalentAliasWords(name, typeName))
		result = owner.empty() ? name : owner + "/" + name;
	else if(genericObjectAliasType(type))
		result = owner.empty() ? name : owner + "/" + name;
	else
	{
		result = type;
		if(!owner.empty())
			result += "/" + owner;
		result += "/" + name;
	}
	return result + objectLocation(object.visitablePos());
}

thread_local std::optional<PlayerColor> aliasDefaultPlayer;
using ObjectAliasCacheKey = std::pair<int, std::string>;
thread_local std::map<ObjectAliasCacheKey, ObjectInstanceID> objectAliasCache;

class ScopedAliasDefaultPlayer
{
	std::optional<PlayerColor> previous;

public:
	explicit ScopedAliasDefaultPlayer(PlayerColor player)
		: previous(aliasDefaultPlayer)
	{
		aliasDefaultPlayer = player;
	}

	~ScopedAliasDefaultPlayer()
	{
		aliasDefaultPlayer = previous;
	}
};

std::string relativeObjectAlias(std::string alias, PlayerColor player)
{
	const std::string playerName = player.toString();
	const std::string heroPrefix = playerName + "/";
	if(alias.starts_with(heroPrefix))
		alias.erase(0, heroPrefix.size());
	else
	{
		const std::string owner = "/" + playerName + "/";
		if(const auto position = alias.find(owner); position != std::string::npos)
			alias.erase(position + 1, playerName.size() + 1);
	}
	return alias;
}

void requireNoPendingDiscoveryCheck(const std::string & context)
{
	if(replayDerivedDiscoveries.empty())
		return;
	throw std::runtime_error(
		"VGT transcript omitted a discovery assertion before " + context + ": engine [" +
		boost::algorithm::join(replayDerivedDiscoveries, ", ") + "]");
}

void beginDiscoveryCheck(const CGameState & gameState, PlayerColor player)
{
	requireNoPendingDiscoveryCheck("the next discovery-producing action");
	replayDiscoveryTracker.observeVisible(gameState, player);
	replayDerivedDiscoveries.clear();
}

void verifyDiscoveryCheck(
	const JsonNode & node,
	const char * field,
	PlayerColor player,
	const std::string & action)
{
	std::vector<std::string> expected;
	if(const auto * discoveries = findField(node, field))
	{
		if(!discoveries->isVector())
			throw std::runtime_error("VGT " + action + " discoveries must be a list");
		for(const auto & discovery : discoveries->Vector())
		{
			if(!discovery.isString())
				throw std::runtime_error("VGT " + action + " discovery must be an object identifier");
			expected.push_back(discovery.String());
		}
	}

	std::vector<std::string> actual;
	for(const auto & discovery : replayDerivedDiscoveries)
	{
		if(vstd::contains(expected, discovery))
		{
			actual.push_back(discovery);
			continue;
		}

		std::optional<std::string> matchingRelative;
		for(int colorIndex = 0; colorIndex < PlayerColor::PLAYER_LIMIT_I; ++colorIndex)
		{
			const std::string relative = relativeObjectAlias(discovery, PlayerColor(colorIndex));
			if(vstd::contains(expected, relative))
			{
				matchingRelative = relative;
				break;
			}
		}
		actual.push_back(matchingRelative.value_or(relativeObjectAlias(discovery, player)));
	}
	std::ranges::sort(expected);
	std::ranges::sort(actual);
	expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
	actual.erase(std::unique(actual.begin(), actual.end()), actual.end());
	replayDerivedDiscoveries.clear();
	if(expected == actual)
		return;

	throw std::runtime_error(
		"VGT " + action + " discovery check failed: transcript [" +
		boost::algorithm::join(expected, ", ") + "], engine [" +
		boost::algorithm::join(actual, ", ") + "]");
}

bool objectMatchesAlias(const CGObjectInstance & object, const std::string & alias)
{
	const auto canonical = canonicalObjectAlias(object);
	return canonical == alias || (aliasDefaultPlayer && relativeObjectAlias(canonical, *aliasDefaultPlayer) == alias);
}

ObjectInstanceID resolveObjectAlias(const CGameState & gameState, const std::string & alias)
{
	if(alias == "none")
		return ObjectInstanceID::NONE;
	if(alias.starts_with("id-"))
		return ObjectInstanceID(std::stoi(alias.substr(std::string("id-").size())));

	const auto & map = gameState.getMap();
	const ObjectAliasCacheKey cacheKey(aliasDefaultPlayer ? aliasDefaultPlayer->getNum() : -1, alias);
	const auto cached = objectAliasCache.find(cacheKey);
	if(cached != objectAliasCache.end())
	{
		const auto * object = map.getObject(cached->second);
		if(object && objectMatchesAlias(*object, alias))
			return cached->second;
		objectAliasCache.erase(cached);
	}

	auto tryObject = [&](const CGObjectInstance * object) -> std::optional<ObjectInstanceID>
	{
		if(!object || !objectMatchesAlias(*object, alias))
			return std::nullopt;
		objectAliasCache.emplace(cacheKey, object->id);
		return object->id;
	};

	if(const auto position = positionFromAlias(alias))
	{
		if(map.isInTheMap(*position))
		{
			for(const auto objectID : map.getTile(*position).visitableObjects)
			{
				if(const auto result = tryObject(map.getObject(objectID)))
					return *result;
			}
		}

		for(const auto & object : map.objects)
		{
			if(object && object->visitablePos() == *position)
			{
				if(const auto result = tryObject(object.get()))
					return *result;
			}
		}

		throw std::runtime_error("Unable to resolve positioned VGT object alias: " + alias);
	}

	for(const auto heroID : map.getHeroesOnMap())
		if(const auto result = tryObject(map.getObject(heroID)))
			return *result;

	for(const auto & object : map.objects)
	{
		if(object && dynamic_cast<const CGHeroInstance *>(object.get()))
		{
			if(const auto result = tryObject(object.get()))
				return *result;
		}
	}

	throw std::runtime_error("Unable to resolve VGT object alias: " + alias);
}

std::optional<PlayerColor> tryPlayerFromActor(const std::string & actor)
{
	std::string value = actor;
	for(const std::string prefix : {"player/", "timer/"})
	{
		if(value.starts_with(prefix))
		{
			value.erase(0, prefix.size());
			break;
		}
	}
	if(value.starts_with("ai/") || value.starts_with("battleAI/") || value.starts_with("human/"))
	{
		const auto parts = splitAlias(value);
		if(parts.size() >= 2)
			value = parts[1];
	}
	if(value == "world")
		return std::nullopt;

	const auto decoded = PlayerColor::decode(value);
	if(decoded < 0 || decoded >= PlayerColor::PLAYER_LIMIT_I)
		return std::nullopt;
	return PlayerColor(decoded);
}

PlayerColor playerFromActor(const std::string & actor)
{
	if(const auto player = tryPlayerFromActor(actor))
		return *player;

	throw std::runtime_error("Unsupported VGT actor: " + actor);
}

std::string scalarAliasText(const JsonNode & node, const std::string & label)
{
	if(node.isNumber())
		return std::to_string(node.Integer());
	if(node.isString())
		return node.String();

	throw std::runtime_error("Unsupported VGT " + label + " alias type");
}

std::string battleAliasText(const JsonNode & node)
{
	return scalarAliasText(node, "battle");
}

int64_t decodePrefixedIntegerAlias(const std::string & value, const std::string & prefix, const std::string & label)
{
	if(value.starts_with(prefix))
		return std::stoll(value.substr(prefix.size()));
	return std::stoll(value);
}

int64_t decodePrefixedIntegerAlias(const JsonNode & node, const std::string & prefix, const std::string & label)
{
	if(node.isNumber())
		return node.Integer();
	if(node.isString())
		return decodePrefixedIntegerAlias(node.String(), prefix, label);

	throw std::runtime_error("Unsupported VGT " + label + " alias type");
}

int64_t decodeStackAlias(const JsonNode & node)
{
	return decodePrefixedIntegerAlias(node, "stack/", "stack");
}

BattleID decodeBattleAlias(const JsonNode & node)
{
	return BattleID(static_cast<int>(decodePrefixedIntegerAlias(node, "battle/", "battle")));
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

bool optionalBool(const JsonNode & node, const char * field, bool defaultValue)
{
	if(const auto * value = findField(node, field))
	{
		if(!value->isBool())
			throw std::runtime_error(std::string("VGT replay field is not a bool: ") + field);
		return value->Bool();
	}
	return defaultValue;
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

BattleAction decodeBattleAction(const JsonNode & node)
{
	BattleAction result;
	result.side = decodeBattleSide(requireString(node, "side"));
	result.stackNumber = static_cast<ui32>(decodeStackAlias(requireField(node, "stack")));
	result.actionType = decodeActionType(requireString(node, "action"));
	result.spell = SpellID::NONE;
	if(const auto * spellNode = findField(node, "spell"))
	{
		if(!spellNode->isString())
			throw std::runtime_error("VGT replay battle action spell must be a string");
		result.spell = decodeSpell(spellNode->String());
	}
	result.target.clear();

	if(const auto * targets = findField(node, "target"))
	{
		if(!targets->isVector())
			throw std::runtime_error("VGT replay battle action target must be a list");

		for(const auto & target : targets->Vector())
			{
				BattleAction::DestinationInfo destination;
				destination.unitValue = BattleAction::INVALID_UNIT_ID;
				destination.hexValue = BattleHex();
				if(target.isStruct())
				{
					const auto unitIter = target.Struct().find("unit");
					if(unitIter != target.Struct().end() && (unitIter->second.isString() || unitIter->second.isNumber()))
						destination.unitValue = static_cast<int32_t>(decodeStackAlias(unitIter->second));

				const auto hexIter = target.Struct().find("hex");
				if(hexIter != target.Struct().end() && hexIter->second.isNumber())
					destination.hexValue = BattleHex(static_cast<si16>(hexIter->second.Integer()));
			}
			result.target.push_back(destination);
		}
	}
	return result;
}

ResourceSet decodeResources(const JsonNode & values)
{
	if(!values.isStruct())
		throw std::runtime_error("VGT replay resources field is not a mapping");
	ResourceSet result;
	for(const auto & [identifier, amount] : values.Struct())
	{
		const auto resource = decodeResource(identifier);
		if(resource != GameResID::NONE)
			result[resource] = static_cast<TResource>(amount.Integer());
	}
	return result;
}

std::pair<GameResID, ui32> decodeSingleResourceAmount(const JsonNode & node, const std::string & context)
{
	if(!node.isStruct() || node.Struct().size() != 1)
		throw std::runtime_error("VGT " + context + " must name exactly one resource");
	const auto & entry = *node.Struct().begin();
	if(!entry.second.isNumber() || entry.second.Integer() < 0)
		throw std::runtime_error("VGT " + context + " amount must be a nonnegative integer");
	return {decodeResource(entry.first), static_cast<ui32>(entry.second.Integer())};
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

void applyEffectPack(CGameHandler & gameHandler, CPackForClient & pack);

void applyArmyState(CGameHandler & gameHandler, ObjectInstanceID armyID, const JsonNode & node)
{
	auto * army = gameHandler.gs->getArmyInstance(armyID);
	if(!army)
		throw std::runtime_error("VGT army state references non-army object: " + std::to_string(armyID.getNum()));

	const CSimpleArmy desiredArmy = decodeSimpleArmy(node);
	std::set<int> desiredSlots;
	for(const auto & [slotID, stack] : desiredArmy.army)
	{
		desiredSlots.insert(slotID.getNum());
		const auto desiredCreature = stack.first;
		const auto desiredCount = stack.second;

		if(!army->hasStackAtSlot(slotID))
		{
			InsertNewStack pack;
			pack.army = armyID;
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
			pack.army = armyID;
			pack.slot = slotID;
			pack.type = desiredCreature;
			applyEffectPack(gameHandler, pack);
		}
		if(army->getStackPtr(slotID)->getCount() != desiredCount)
		{
			ChangeStackCount pack;
			pack.army = armyID;
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
		pack.army = armyID;
		pack.slot = slotID;
		applyEffectPack(gameHandler, pack);
	}

	for(const auto & entry : node.Vector())
	{
		const auto * experience = findField(entry, "experience");
		const SlotID slotID(static_cast<int>(requireInteger(entry, "slot")));
		auto * current = army->getStackPtr(slotID);
		if(!current)
			throw std::runtime_error("VGT army experience slot is absent: " + std::to_string(slotID.getNum()));
		const TExpType total = experience ? experience->Integer() : 0;
		current->setTotalStackExperience(total);
		current->nodeHasChanged();
	}
	army->nodeHasChanged();

	if(dynamic_cast<const CGCreature *>(army) && army->Slots().size() == 1 && army->hasStackAtSlot(SlotID(0)))
	{
		const auto count = army->getStackPtr(SlotID(0))->getCount();
		gameHandler.setObjPropertyValue(
			armyID, ObjProperty::MONSTER_POWER, static_cast<int32_t>(count * 1000));
	}
}

Handicap decodeHandicap(const JsonNode & node)
{
	Handicap result;
	if(const auto * resources = findField(node, "resources"))
		result.startBonus = decodeResources(*resources);
	if(hasField(node, "incomePercent"))
		result.percentIncome = static_cast<int>(requireInteger(node, "incomePercent"));
	if(hasField(node, "growthPercent"))
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

ExtraOptionsInfo decodeExtraOptions(const JsonNode & node)
{
	ExtraOptionsInfo result;
	result.cheatsAllowed = requireBool(node, "cheatsAllowed");
	result.unlimitedReplay = requireBool(node, "unlimitedReplay");
	return result;
}

PlayerSettings decodePlayerSettings(const JsonNode & node, PlayerColor color, const PlayerSettings * base = nullptr)
{
	PlayerSettings result = base ? *base : PlayerSettings();
	result.color = color;
	const std::string controller = hasField(node, "controller") ? requireString(node, "controller") :
		(result.isControlledByHuman() ? "human" : "ai");
	if(hasField(node, "faction"))
		result.castle = decodeFaction(requireString(node, "faction"));
	if(hasField(node, "hero"))
		result.hero = decodeHeroType(requireString(node, "hero"));
	if(hasField(node, "heroPortrait"))
		result.heroPortrait = decodeHeroType(requireString(node, "heroPortrait"));
	if(hasField(node, "heroNameTextId"))
		result.heroNameTextId = requireString(node, "heroNameTextId");
	if(hasField(node, "startingBonus"))
		result.bonus = decodeStartingBonus(requireString(node, "startingBonus"));
	if(const auto * handicap = findField(node, "handicap"))
		result.handicap = decodeHandicap(*handicap);
	if(hasField(node, "name"))
		result.name = requireString(node, "name");
	else if(!base)
		result.name = controller == "ai" ? "Computer" : "";
	if(const auto * connections = findField(node, "connections"))
		result.connectedPlayerIDs = decodeConnections(*connections);
	if(hasField(node, "computerOnly"))
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

std::map<PlayerColor, PlayerSettings> decodePlayerSettingsDeltaMap(const JsonNode & players, const JsonNode & deltas)
{
	auto result = decodePlayerSettingsMap(players);
	if(!deltas.isStruct())
		throw std::runtime_error("VGT replay initialPlayers field is not a mapping");
	for(const auto & entry : deltas.Struct())
	{
		const PlayerColor color = decodePlayerColor(entry.first);
		const auto base = result.find(color);
		result[color] = decodePlayerSettings(entry.second, color, base == result.end() ? nullptr : &base->second);
	}
	return result;
}

std::map<PlayerColor, PlayerSettings> decodeGeneratedMapInitializationPlayerSettings(const JsonNode & players, const JsonNode & initialPlayers)
{
	auto result = decodePlayerSettingsMap(players);
	const auto initial = decodePlayerSettingsDeltaMap(players, initialPlayers);
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

CMapGenOptions::CPlayerSettings decodeRandomMapPlayerSettings(
	const JsonNode & node,
	PlayerColor color,
	const CMapGenOptions::CPlayerSettings * base = nullptr)
{
	CMapGenOptions::CPlayerSettings result = base ? *base : CMapGenOptions::CPlayerSettings();
	result.setColor(color);
	if(hasField(node, "type"))
		result.setPlayerType(decodeRandomMapPlayerType(requireString(node, "type")));
	if(hasField(node, "faction"))
		result.setStartingTown(decodeFaction(requireString(node, "faction")));
	if(hasField(node, "hero"))
		result.setStartingHero(decodeHeroType(requireString(node, "hero")));
	if(hasField(node, "team"))
		result.setTeam(decodeTeam(requireField(node, "team")));
	return result;
}

std::shared_ptr<CMapGenOptions> decodeRandomMapGenerator(const JsonNode & node)
{
	auto result = std::make_shared<CMapGenOptions>();
	result->setWidth(static_cast<si32>(requireInteger(node, "width")));
	result->setHeight(static_cast<si32>(requireInteger(node, "height")));
	result->setLevels(hasField(node, "levels") ? static_cast<int>(requireInteger(node, "levels")) : 1);
	result->setHumanOrCpuPlayerCount(static_cast<si8>(requireInteger(node, "humanOrComputerPlayers")));
	result->setTeamCount(hasField(node, "teams") ? static_cast<si8>(requireInteger(node, "teams")) : 0);
	result->setCompOnlyPlayerCount(hasField(node, "computerOnlyPlayers")
		? static_cast<si8>(requireInteger(node, "computerOnlyPlayers"))
		: 0);
	result->setCompOnlyTeamCount(hasField(node, "computerOnlyTeams")
		? static_cast<si8>(requireInteger(node, "computerOnlyTeams"))
		: 0);
	result->setWaterContent(hasField(node, "water")
		? decodeRandomMapWater(requireString(node, "water"))
		: EWaterContent::NONE);
	result->setMonsterStrength(hasField(node, "monsters")
		? decodeRandomMapMonsterStrength(requireString(node, "monsters"))
		: EMonsterStrength::GLOBAL_NORMAL);

	if(const auto * templateNode = findField(node, "template");
		templateNode && templateNode->isString() && !templateNode->String().empty())
		result->setMapTemplate(templateNode->String());

	for(const auto roadId : { RoadId::DIRT_ROAD, RoadId::GRAVEL_ROAD, RoadId::COBBLESTONE_ROAD })
		result->setRoadEnabled(roadId, false);

	if(const auto * roads = findField(node, "roads"))
	{
		if(!roads->isVector())
			throw std::runtime_error("VGT replay random map roads must be a list");
		for(const auto & road : roads->Vector())
		{
			if(!road.isString())
				throw std::runtime_error("VGT replay random map road is not a string");
			result->setRoadEnabled(decodeRoad(road.String()), true);
		}
	}

	std::map<PlayerColor, CMapGenOptions::CPlayerSettings> playerSettings;
	std::vector<PlayerColor> playerSlots;
	if(const auto * slots = findField(node, "slots"))
	{
		if(!slots->isVector())
			throw std::runtime_error("VGT replay random map slots must be a list");
		for(const auto & slot : slots->Vector())
		{
			if(!slot.isString())
				throw std::runtime_error("VGT replay random map slot is not a player color");
			playerSlots.push_back(decodePlayerColor(slot.String()));
		}
	}
	else
	{
		const int standardPlayers = std::max<int>(0, result->getHumanOrCpuPlayerCount());
		const int computerOnlyPlayers = std::max<int>(0, result->getCompOnlyPlayerCount());
		for(int index = 0; index < standardPlayers + computerOnlyPlayers; ++index)
			playerSlots.emplace_back(index);
	}
	for(const PlayerColor color : playerSlots)
	{
		CMapGenOptions::CPlayerSettings settings;
		settings.setColor(color);
		settings.setPlayerType(EPlayerType::AI);
		settings.setStartingTown(FactionID::RANDOM);
		settings.setStartingHero(HeroTypeID::RANDOM);
		settings.setTeam(TeamID(color.getNum()));
		playerSettings[color] = settings;
	}
	if(const auto * players = findField(node, "players"))
	{
		if(!players->isStruct())
			throw std::runtime_error("VGT replay random map players must be a mapping");
		for(const auto & entry : players->Struct())
		{
			const auto color = decodePlayerColor(entry.first);
			const auto base = playerSettings.find(color);
			playerSettings[color] = decodeRandomMapPlayerSettings(
				entry.second, color, base == playerSettings.end() ? nullptr : &base->second);
		}
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
	const auto & initialPlayers = requireField(header, "initialPlayers");
	const auto generatedMapFile = isGeneratedMapFile(map);

	result.mode = decodeStartMode(requireString(settingsNode, "start"));
	result.startTime = static_cast<time_t>(requireInteger(settingsNode, "startTime"));
	result.difficulty = decodeDifficulty(requireString(settingsNode, "difficulty"));
	result.fileURI = requireString(map, "uri");
	result.mapname = hasField(map, "name") ? requireString(map, "name") : result.fileURI;
	if(const auto * simturns = findField(settingsNode, "simturns"))
		result.simturnsInfo = decodeSimturns(*simturns);
	if(const auto * timer = findField(settingsNode, "timer"))
		result.turnTimerInfo = decodeTimer(*timer);
	if(const auto * extraOptions = findField(settingsNode, "extraOptions"))
		result.extraOptionsInfo = decodeExtraOptions(*extraOptions);
	if(generatedMapFile)
		result.playerInfos = decodeGeneratedMapInitializationPlayerSettings(players, initialPlayers);
	else
		result.playerInfos = decodePlayerSettingsDeltaMap(players, initialPlayers);
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
	JsonNode emptyOverrides;
	const auto * expectedOverridesNode = findField(settingsNode, "gameSettingsOverrides");
	const auto & expectedOverrides = expectedOverridesNode ? *expectedOverridesNode : emptyOverrides;

	const auto actualOverrides = gameHandler.gs->getMap().getGameSettingsOverrides();
	const auto isEmptyOverrides = [](const JsonNode & node)
	{
		return node.isNull() || (node.isStruct() && node.Struct().empty());
	};

	if(isEmptyOverrides(actualOverrides) && isEmptyOverrides(expectedOverrides))
		return;

	if(actualOverrides.toCompactString() != expectedOverrides.toCompactString())
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
			initialStartInfo->playerInfos = decodePlayerSettingsDeltaMap(
				requireField(header, "players"), requireField(header, "initialPlayers"));
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

	const auto & counterNode = requireField(mapNode, "objectNameCounter");
	if(!counterNode.isNumber())
		throw std::runtime_error("VGT replay map objectNameCounter is not numeric");

	const auto counter = counterNode.Integer();
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
			SpellID spellID = SpellID::NONE;
			if(const auto * spellNode = findField(artifactNode, "spell"))
			{
				if(!spellNode->isString())
					throw std::runtime_error("VGT initial artifact spell must be a string");
				spellID = decodeSpell(spellNode->String());
			}
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

	applyArmyState(gameHandler, heroID, requireField(node, "army"));
}

void applyInitialState(CGameHandler & gameHandler, const JsonNode & header)
{
	const auto & initialStateNode = requireField(header, "initialState");
	const auto & heroesNode = requireField(initialStateNode, "heroes");
	if(heroesNode.isVector())
	{
		for(const auto & heroNode : heroesNode.Vector())
			applyInitialHeroArmyState(gameHandler, heroNode);
		return;
	}
	if(!heroesNode.isStruct())
		throw std::runtime_error("VGT initialState.heroes must be a mapping");
	for(const auto & [hero, state] : heroesNode.Struct())
	{
		JsonNode heroState = state;
		heroState["id"].String() = hero;
		applyInitialHeroArmyState(gameHandler, heroState);
	}
}

void replayDecision(CGameHandler & gameHandler, const JsonNode & decision)
{
	const std::string actor = requireString(decision, "actor");
	const std::string kind = requireString(decision, "kind");

	if(kind == "battleAction")
	{
		MakeAction pack;
		pack.battleID = decodeBattleAlias(requireField(decision, "battle"));
		const auto & actionNode = requireField(decision, "action");
		pack.ba = decodeBattleAction(actionNode);
		std::vector<uint32_t> secondaryTargets;
		if(const auto * targets = findField(actionNode, "secondaryTargets"))
		{
			if(!targets->isVector())
				throw std::runtime_error("VGT replay battle action secondaryTargets must be a list");
			for(const auto & target : targets->Vector())
			{
				if(!target.isNumber())
					throw std::runtime_error("VGT replay battle action secondary target must be numeric");
				secondaryTargets.push_back(static_cast<uint32_t>(target.Integer()));
			}
		}
		if(const auto player = tryPlayerFromActor(actor))
		{
			if(!gameHandler.battles->makePlayerBattleAction(pack.battleID, *player, pack.ba, secondaryTargets))
				throw std::runtime_error(
					"VGT battle action was rejected for " + actor + ": " + actionNode.toCompactString());
		}
		else if(actor == "world")
		{
			const auto * battle = gameHandler.gameState().getBattle(pack.battleID);
			if(!battle)
				throw std::runtime_error("VGT replay world battle action references missing battle: " + battleAliasText(requireField(decision, "battle")));

			const auto * active = battle->battleActiveUnit();
			const PlayerColor owner = active ? battle->battleGetOwner(active) : PlayerColor::UNFLAGGABLE;
			if(!gameHandler.battles->makePlayerBattleAction(pack.battleID, owner, pack.ba, secondaryTargets))
				throw std::runtime_error(
					"VGT world battle action was rejected: " + actionNode.toCompactString());
		}
		else
			throw std::runtime_error("Unsupported VGT actor: " + actor);
		return;
	}

	const PlayerColor player = playerFromActor(actor);
	ScopedAliasDefaultPlayer aliasScope(player);

	if(kind == "endTurn")
	{
		EndTurn pack;
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "move")
	{
		MoveHero pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.transit = optionalBool(decision, "transit", false);
		pack.layer = EPathfindingLayer::AUTO;
		beginDiscoveryCheck(gameHandler.gameState(), player);
		if(hasField(decision, "steps"))
		{
			const auto * hero = gameHandler.gameState().getHero(pack.hid);
			if(!hero)
				throw std::runtime_error("VGT move action references a missing hero");
			for(const auto & destination : decodeMoveSteps(hero->anchorPos(), decision))
			{
				pack.path = {destination};
				replayPack(gameHandler, pack, player);
			}
			verifyDiscoveryCheck(decision, "discovers", player, "move");
			return;
		}
		if(const auto * destination = findField(decision, "to"))
			pack.path = {decodePosition(*destination)};
		else
			throw std::runtime_error("VGT move action must contain to");
		replayPack(gameHandler, pack, player);
		verifyDiscoveryCheck(decision, "discovers", player, "move");
		return;
	}

	if(kind == "castleTeleportHero")
	{
		CastleTeleportHero pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.dest = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "destination"));
		pack.source = static_cast<si8>(requireInteger(decision, "source"));
		beginDiscoveryCheck(gameHandler.gameState(), player);
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "build")
	{
		BuildStructure pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "town"));
		pack.bid = decodeBuilding(requireString(decision, "building"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "visitTownBuilding")
	{
		VisitTownBuilding pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "town"));
		pack.bid = decodeBuilding(requireString(decision, "building"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "razeStructure")
	{
		RazeStructure pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "town"));
		pack.bid = decodeBuilding(requireString(decision, "building"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "spellResearch")
	{
		SpellResearch pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "town"));
		pack.spellAtSlot = decodeSpell(requireString(decision, "spell"));
		pack.accepted = requireBool(decision, "accepted");
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "recruit")
	{
		const auto source = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "at"));
		const auto destination = findField(decision, "to")
			? resolveObjectAlias(gameHandler.gameState(), requireString(decision, "to"))
			: source;
		const auto & units = requireField(decision, "units");
		auto recruit = [&](const std::string & creatureIdentifier, int64_t count, const std::optional<std::string> & poolName)
		{
			if(count <= 0 || count > std::numeric_limits<ui32>::max())
				throw std::runtime_error("VGT semantic recruitment has an invalid count");
			const CreatureID creature = decodeCreature(creatureIdentifier);
			int fromLevel = -1;
			if(const auto * dwelling = dynamic_cast<const CGDwelling *>(gameHandler.gameState().getObj(source)))
			{
				if(poolName)
				{
					if(*poolName == "portalOfSummoning")
						fromLevel = static_cast<int>(dwelling->creatures.size()) - 1;
					else if(poolName->starts_with("level-"))
						fromLevel = std::stoi(poolName->substr(std::string("level-").size())) - 1;
					else
						throw std::runtime_error("Unknown VGT recruitment pool: " + *poolName);
				}
				else
				{
					std::vector<int> matchingPools;
					for(size_t index = 0; index < dwelling->creatures.size(); ++index)
					{
						const auto & pool = dwelling->creatures[index];
						if(pool.first >= static_cast<ui32>(count) &&
							std::find(pool.second.begin(), pool.second.end(), creature) != pool.second.end())
							matchingPools.push_back(static_cast<int>(index));
					}
					if(matchingPools.size() == 1)
						fromLevel = matchingPools.front();
					else if(matchingPools.size() > 1)
						throw std::runtime_error(
							"VGT semantic recruitment is ambiguous; a named pool is required");
				}
			}
			if(!gameHandler.recruitCreatures(
				source,
				destination,
				creature,
				static_cast<ui32>(count),
				fromLevel,
				player))
			{
				throw std::runtime_error("VGT semantic recruitment was rejected");
			}
		};
		if(units.isStruct())
		{
			if(units.Struct().size() != 1)
				throw std::runtime_error("Compact VGT recruitment must contain exactly one creature");
			const auto & unit = *units.Struct().begin();
			const auto * pool = findField(decision, "pool");
			recruit(unit.first, unit.second.Integer(), pool ? std::optional<std::string>(pool->String()) : std::nullopt);
		}
		else if(units.isVector())
		{
			for(const auto & unit : units.Vector())
			{
				const auto * pool = findField(unit, "pool");
				recruit(requireString(unit, "creature"), requireInteger(unit, "count"),
					pool ? std::optional<std::string>(pool->String()) : std::nullopt);
			}
		}
		else
			throw std::runtime_error("VGT recruitment units must be a mapping or ordered list");
		return;
	}

	if(kind == "dismissHero")
	{
		DismissHero pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "hire")
	{
		HireHero pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "at"));
		pack.hid = decodeHeroType(requireString(decision, "hero"));
		pack.nhid = hasField(decision, "replacement")
			? decodeHeroType(requireString(decision, "replacement"))
			: HeroTypeID::NONE;
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "swapStacks" || kind == "mergeStacks" || kind == "splitStack")
	{
		const auto & from = requireField(decision, "from");
		const auto & to = requireField(decision, kind == "mergeStacks" ? "into" : "to");

		ArrangeStacks pack;
		pack.what = kind == "swapStacks" ? 1 : kind == "mergeStacks" ? 2 : 3;
		pack.id1 = resolveObjectAlias(gameHandler.gameState(), requireString(from, "army"));
		pack.p1 = decodeSlot(requireField(from, "slot"));
		pack.id2 = resolveObjectAlias(gameHandler.gameState(), requireString(to, "army"));
		pack.p2 = decodeSlot(requireField(to, "slot"));
		pack.val = kind == "splitStack" ? static_cast<si32>(requireInteger(decision, "count")) : 0;
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "bulkMoveArmy")
	{
		const auto & from = requireField(decision, "from");
		BulkMoveArmy pack;
		pack.srcArmy = resolveObjectAlias(gameHandler.gameState(), requireString(from, "army"));
		pack.srcSlot = decodeSlot(requireField(from, "slot"));
		pack.destArmy = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "to"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "bulkSplitStack")
	{
		const auto & source = requireField(decision, "source");
		BulkSplitStack pack;
		pack.srcOwner = resolveObjectAlias(gameHandler.gameState(), requireString(source, "army"));
		pack.src = decodeSlot(requireField(source, "slot"));
		pack.amount = static_cast<si32>(requireInteger(decision, "amount"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "bulkMergeStacks")
	{
		const auto & source = requireField(decision, "source");
		BulkMergeStacks pack;
		pack.srcOwner = resolveObjectAlias(gameHandler.gameState(), requireString(source, "army"));
		pack.src = decodeSlot(requireField(source, "slot"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "bulkSplitAndRebalanceStack")
	{
		const auto & source = requireField(decision, "source");
		BulkSplitAndRebalanceStack pack;
		pack.srcOwner = resolveObjectAlias(gameHandler.gameState(), requireString(source, "army"));
		pack.src = decodeSlot(requireField(source, "slot"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "disbandCreature")
	{
		DisbandCreature pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "army"));
		pack.pos = decodeSlot(requireField(decision, "slot"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "upgradeCreature")
	{
		UpgradeCreature pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "army"));
		pack.pos = decodeSlot(requireField(decision, "slot"));
		pack.cid = decodeCreature(requireString(decision, "creature"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "swapTownHeroes")
	{
		GarrisonHeroSwap pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "town"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "arrangeArmies")
	{
		const auto & armies = requireField(decision, "armies");
		if(!armies.isStruct() || armies.Struct().empty())
			throw std::runtime_error("VGT arrangeArmies action has no armies");
		struct UndeadMoraleSnapshot
		{
			CArmedInstance * army;
			std::shared_ptr<Bonus> bonus;
			size_t index;
		};
		std::map<ObjectInstanceID, UndeadMoraleSnapshot> undeadMoraleBefore;
		const auto undeadMoraleSelector = Selector::source(
			BonusSource::ARMY, BonusCustomSource::undeadMoraleDebuff).And(Selector::type()(BonusType::MORALE));
		for(const auto & [army, state] : armies.Struct())
		{
			const ObjectInstanceID armyID = resolveObjectAlias(gameHandler.gameState(), army);
			auto * armed = gameHandler.gs->getArmyInstance(armyID);
			if(!armed)
				throw std::runtime_error("VGT arrangeArmies references a non-army object");
			const auto bonus = armed->getLocalBonus(undeadMoraleSelector);
			size_t index = armed->getExportedBonusList().size();
			if(bonus)
			{
				index = 0;
				for(const auto & candidate : armed->getExportedBonusList())
				{
					if(candidate == bonus)
						break;
					++index;
				}
			}
			undeadMoraleBefore.emplace(armyID, UndeadMoraleSnapshot{armed, bonus, index});
		}
		for(const auto & [army, state] : armies.Struct())
			applyArmyState(gameHandler, resolveObjectAlias(gameHandler.gameState(), army), state);
		std::set<ObjectInstanceID> refreshedArmies;
		if(const auto * refreshed = findField(decision, "refreshUndeadMorale"))
		{
			if(!refreshed->isVector())
				throw std::runtime_error("VGT arrangeArmies refreshUndeadMorale must be a list");
			for(const auto & entry : refreshed->Vector())
			{
				const ObjectInstanceID armyID = resolveObjectAlias(gameHandler.gameState(), entry.String());
				refreshedArmies.insert(armyID);
				auto * army = gameHandler.gs->getArmyInstance(armyID);
				if(!army)
					throw std::runtime_error("VGT arrangeArmies refreshUndeadMorale references a non-army object");
				auto bonus = army->getLocalBonus(undeadMoraleSelector);
				if(!bonus)
					throw std::runtime_error("VGT arrangeArmies cannot refresh an absent undead morale modifier");
				army->removeBonus(bonus);
				army->addNewBonus(bonus);
			}
		}
		for(auto & [armyID, snapshot] : undeadMoraleBefore)
		{
			if(refreshedArmies.contains(armyID))
				continue;
			auto current = snapshot.army->getLocalBonus(undeadMoraleSelector);
			if(!snapshot.bonus)
			{
				if(current)
					snapshot.army->removeBonus(current);
				continue;
			}
			if(!current)
				continue;
			auto & bonuses = snapshot.army->getExportedBonusList();
			size_t currentIndex = 0;
			for(const auto & candidate : bonuses)
			{
				if(candidate == current)
					break;
				++currentIndex;
			}
			if(currentIndex >= bonuses.size() || currentIndex == snapshot.index)
				continue;
			bonuses.erase(static_cast<int>(currentIndex));
			bonuses.insert(bonuses.begin() + std::min(snapshot.index, bonuses.size()), 1, current);
		}
		return;
	}

	if(kind == "exchangeArtifacts")
	{
		ExchangeArtifacts pack;
		pack.src = decodeArtifactLocation(gameHandler, requireField(decision, "from"));
		pack.dst = decodeArtifactLocation(gameHandler, requireField(decision, "to"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "moveArtifacts")
	{
		const ObjectInstanceID from = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "from"));
		const ObjectInstanceID to = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "to"));
		const auto & moves = requireField(decision, "artifacts");
		if(!moves.isVector() || moves.Vector().empty())
			throw std::runtime_error("VGT moveArtifacts action has no artifacts");
		for(const auto & move : moves.Vector())
		{
			ExchangeArtifacts pack;
			pack.src = ArtifactLocation(from, decodeArtifactPosition(requireField(move, "from")));
			pack.dst = ArtifactLocation(to, decodeArtifactPosition(requireField(move, "to")));
			const auto * source = gameHandler.gameState().getArtSet(pack.src);
			const auto * instance = source ? source->getArt(pack.src.slot) : nullptr;
			if(!instance || instance->getTypeId() != decodeArtifact(requireString(move, "artifact")))
				throw std::runtime_error("VGT moveArtifacts source does not contain the named artifact");
			replayPack(gameHandler, pack, player);
		}
		return;
	}

	if(kind == "bulkExchangeArtifacts")
	{
		BulkExchangeArtifacts pack;
		pack.srcHero = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "from"));
		pack.dstHero = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "to"));
		pack.swap = requireBool(decision, "swap");
		pack.equipped = requireBool(decision, "equipped");
		pack.backpack = requireBool(decision, "backpack");
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "manageBackpackArtifacts")
	{
		ManageBackpackArtifacts pack;
		pack.artHolder = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "holder"));
		pack.cmd = decodeBackpackManageCommand(requireString(decision, "command"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "manageEquippedArtifacts")
	{
		ManageEquippedArtifacts pack;
		pack.artHolder = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "holder"));
		pack.costumeIdx = static_cast<uint32_t>(requireInteger(decision, "costume"));
		pack.saveCostume = optionalBool(decision, "save", false);
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "assembleArtifacts")
	{
		AssembleArtifacts pack;
		pack.heroID = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.artifactSlot = decodeArtifactPosition(requireField(decision, "slot"));
		pack.assemble = requireBool(decision, "assemble");
		pack.assembleTo = decodeArtifact(requireString(decision, "artifact"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "eraseArtifact")
	{
		EraseArtifactByClient pack;
		pack.al = decodeArtifactLocation(gameHandler, requireField(decision, "location"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "buyArtifact")
	{
		BuyArtifact pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.aid = decodeArtifact(requireString(decision, "artifact"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "trade")
	{
		TradeOnMarketplace pack;
		pack.marketId = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "at"));
		pack.heroId = ObjectInstanceID::NONE;
		pack.mode = EMarketMode::RESOURCE_RESOURCE;
		const auto & exchanges = requireField(decision, "exchanges");
		if(!exchanges.isVector() || exchanges.Vector().empty())
			throw std::runtime_error("VGT resource trade must contain exchanges");
		const auto * market = gameHandler.gameState().getMarket(pack.marketId);
		if(!market)
			throw std::runtime_error("VGT trade references a non-market object");
		for(const auto & exchange : exchanges.Vector())
		{
			if(!exchange.isString())
				throw std::runtime_error("VGT trade exchange must be '<amount> <resource> for <amount> <resource>'");
			std::istringstream parser(exchange.String());
			uint32_t soldAmount = 0;
			int64_t boughtAmount = 0;
			std::string soldName;
			std::string separator;
			std::string boughtName;
			if(!(parser >> soldAmount >> soldName >> separator >> boughtAmount >> boughtName))
				throw std::runtime_error("Invalid VGT trade exchange: " + exchange.String());
			parser >> std::ws;
			if(separator != "for" || soldAmount == 0 || boughtAmount <= 0 || !parser.eof())
				throw std::runtime_error("Invalid VGT trade exchange: " + exchange.String());
			const auto soldResource = decodeResource(soldName);
			const auto boughtResource = decodeResource(boughtName);
			int offerSold = 0;
			int offerBought = 0;
			if(!market->getOffer(soldResource, boughtResource, offerSold, offerBought, pack.mode) || offerSold <= 0 ||
				static_cast<int64_t>(soldAmount / offerSold * offerBought) != boughtAmount)
				throw std::runtime_error("VGT trade bought amount does not match the market offer");
			pack.r1.emplace_back(soldResource);
			pack.r2.emplace_back(boughtResource);
			pack.val.push_back(soldAmount);
		}
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "sendResources" || kind == "sellCreatures" || kind == "buyArtifacts" ||
		kind == "sellArtifacts" || kind == "transformUndead" || kind == "learnSkills" ||
		kind == "sacrificeCreatures" || kind == "sacrificeArtifacts")
	{
		TradeOnMarketplace pack;
		pack.marketId = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "at"));
		pack.heroId = hasField(decision, "hero")
			? resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"))
			: ObjectInstanceID::NONE;

		if(kind == "sendResources")
		{
			pack.mode = EMarketMode::RESOURCE_PLAYER;
			auto appendTransfer = [&](const JsonNode & transfer)
			{
				const auto recipient = decodePlayerColor(requireString(transfer, "to"));
				const auto resources = decodeResources(requireField(transfer, "resources"));
				for(size_t index = 0; index < resources.size(); ++index)
				{
					if(resources[index] <= 0)
						continue;
					pack.r1.emplace_back(GameResID(static_cast<int>(index)));
					pack.r2.emplace_back(recipient);
					pack.val.push_back(static_cast<ui32>(resources[index]));
				}
			};
			if(const auto * transfers = findField(decision, "transfers"))
			{
				if(!transfers->isVector())
					throw std::runtime_error("VGT sendResources transfers must be a list");
				for(const auto & transfer : transfers->Vector())
					appendTransfer(transfer);
			}
			else
				appendTransfer(decision);
		}
		else if(kind == "sellCreatures")
		{
			pack.mode = EMarketMode::CREATURE_RESOURCE;
			const auto & sales = requireField(decision, "sales");
			if(!sales.isVector())
				throw std::runtime_error("VGT sellCreatures sales must be a list");
			for(const auto & sale : sales.Vector())
			{
				const auto resource = decodeSingleResourceAmount(requireField(sale, "received"), "creature sale outcome").first;
				pack.r1.emplace_back(decodeSlot(requireField(sale, "slot")));
				pack.r2.emplace_back(resource);
				pack.val.push_back(static_cast<ui32>(requireInteger(sale, "count")));
			}
		}
		else if(kind == "buyArtifacts")
		{
			pack.mode = EMarketMode::RESOURCE_ARTIFACT;
			const auto & purchases = requireField(decision, "purchases");
			if(!purchases.isVector())
				throw std::runtime_error("VGT buyArtifacts purchases must be a list");
			for(const auto & purchase : purchases.Vector())
			{
				const auto resource = decodeSingleResourceAmount(requireField(purchase, "paid"), "artifact purchase cost").first;
				pack.r1.emplace_back(resource);
				pack.r2.emplace_back(decodeArtifact(requireString(purchase, "artifact")));
				pack.val.push_back(0);
			}
		}
		else if(kind == "sellArtifacts")
		{
			pack.mode = EMarketMode::ARTIFACT_RESOURCE;
			const auto & sales = requireField(decision, "sales");
			if(!sales.isVector())
				throw std::runtime_error("VGT sellArtifacts sales must be a list");
			for(const auto & sale : sales.Vector())
			{
				const auto resource = decodeSingleResourceAmount(requireField(sale, "received"), "artifact sale outcome").first;
				pack.r1.emplace_back(ArtifactInstanceID(static_cast<int>(requireInteger(sale, "instance"))));
				pack.r2.emplace_back(resource);
				pack.val.push_back(0);
			}
		}
		else if(kind == "transformUndead")
		{
			pack.mode = EMarketMode::CREATURE_UNDEAD;
			const auto & stacks = requireField(decision, "stacks");
			if(!stacks.isVector())
				throw std::runtime_error("VGT transformUndead stacks must be a list");
			for(const auto & stack : stacks.Vector())
			{
				pack.r1.emplace_back(decodeSlot(requireField(stack, "slot")));
				pack.r2.emplace_back(GameResID(0));
				pack.val.push_back(0);
			}
		}
		else if(kind == "learnSkills")
		{
			pack.mode = EMarketMode::RESOURCE_SKILL;
			const auto & skills = requireField(decision, "skills");
			if(!skills.isVector())
				throw std::runtime_error("VGT learnSkills skills must be a list");
			for(const auto & skill : skills.Vector())
			{
				pack.r1.emplace_back(GameResID(EGameResID::GOLD));
				pack.r2.emplace_back(decodeSecondarySkill(skill.String()));
				pack.val.push_back(0);
			}
		}
		else if(kind == "sacrificeCreatures")
		{
			pack.mode = EMarketMode::CREATURE_EXP;
			const auto & stacks = requireField(decision, "stacks");
			if(!stacks.isVector())
				throw std::runtime_error("VGT sacrificeCreatures stacks must be a list");
			for(const auto & stack : stacks.Vector())
			{
				pack.r1.emplace_back(decodeSlot(requireField(stack, "slot")));
				pack.r2.emplace_back(GameResID(0));
				pack.val.push_back(static_cast<ui32>(requireInteger(stack, "count")));
			}
		}
		else
		{
			pack.mode = EMarketMode::ARTIFACT_EXP;
			const auto & artifacts = requireField(decision, "artifacts");
			if(!artifacts.isVector())
				throw std::runtime_error("VGT sacrificeArtifacts artifacts must be a list");
			for(const auto & artifact : artifacts.Vector())
			{
				pack.r1.emplace_back(ArtifactInstanceID(static_cast<int>(requireInteger(artifact, "instance"))));
				pack.r2.emplace_back(GameResID(0));
				pack.val.push_back(0);
			}
		}
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "setFormation")
	{
		SetFormation pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.formation = decodeArmyFormation(requireString(decision, "formation"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "setTactics")
	{
		SetTactics pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.enabled = requireBool(decision, "enabled");
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "setTownName")
	{
		SetTownName pack;
		pack.tid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "town"));
		pack.name = requireString(decision, "name");
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "buildBoat")
	{
		BuildBoat pack;
		pack.objid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "object"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "dig")
	{
		DigWithHero pack;
		pack.id = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "castAdventureSpell")
	{
		CastAdvSpell pack;
		pack.hid = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
		pack.sid = decodeSpell(requireString(decision, "spell"));
		pack.pos = decodePosition(requireField(decision, "position"));
		beginDiscoveryCheck(gameHandler.gameState(), player);
		replayPack(gameHandler, pack, player);
		if(hasField(decision, "discovers"))
			verifyDiscoveryCheck(decision, "discovers", player, "adventure spell");
		return;
	}

	if(kind == "pauseTimer")
	{
		GamePause pack;
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "ready")
	{
		AdvInterfaceReady pack;
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "playerMessage")
	{
		PlayerMessage pack;
		pack.text = requireString(decision, "text");
		pack.currObj = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "object"));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "finish" || kind == "answer")
	{
		const auto topQuery = gameHandler.queries->topQuery(player);
		if(!topQuery)
			throw std::runtime_error("VGT semantic answer has no active query");
		QueryReply pack;
		pack.qid = topQuery->queryID;
		if(kind == "finish")
			pack.reply = 0;
		else
		{
			const auto & value = requireField(decision, "value");
			pack.reply = value.isNull() ? std::optional<int32_t>() : std::optional<int32_t>(static_cast<int32_t>(value.Integer()));
		}
		beginDiscoveryCheck(gameHandler.gameState(), player);
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "chooseSkill")
	{
		const auto query = std::dynamic_pointer_cast<CHeroLevelUpDialogQuery>(gameHandler.queries->topQuery(player));
		if(!query)
			throw std::runtime_error("VGT chooseSkill has no active hero level-up query");
		const auto selected = decodeSecondarySkill(requireString(decision, "skill"));
		const auto choice = std::find(query->hlu.skills.begin(), query->hlu.skills.end(), selected);
		if(choice == query->hlu.skills.end())
			throw std::runtime_error("VGT chooseSkill selection is not offered by the active query");
		QueryReply pack;
		pack.qid = query->queryID;
		pack.reply = static_cast<int32_t>(std::distance(query->hlu.skills.begin(), choice));
		replayPack(gameHandler, pack, player);
		return;
	}

	if(kind == "encounter")
	{
		if(const auto * approach = findField(decision, "approach"))
		{
			JsonNode move = *approach;
			move["actor"].String() = actor;
			move["kind"].String() = "move";
			move["hero"].String() = requireString(decision, "hero");
			replayDecision(gameHandler, move);
		}
		beginDiscoveryCheck(gameHandler.gameState(), player);
		const auto * choice = findField(decision, "choice");
		if(!choice)
		{
			verifyDiscoveryCheck(decision, "discovers", player, "encounter");
			return;
		}
		const auto topQuery = gameHandler.queries->topQuery(player);
		if(!topQuery)
			throw std::runtime_error("VGT encounter has no active decision query");
		QueryReply pack;
		pack.qid = topQuery->queryID;
		const auto & value = requireField(*choice, "value");
		if(value.isNull())
			pack.reply = std::nullopt;
		else
			pack.reply = static_cast<int32_t>(value.Integer());
		const std::string choiceName = requireString(*choice, "name");
		if(choiceName == "fight")
		{
			// A neutral-creature fight can involve several generated prompts: a
			// joining offer, an insufficient-funds notice, and a pursuit question.
			// The transcript records the meaningful decision once and omits those
			// UI mechanics. Keep affirming the decision until combat begins.
			for(int prompt = 0; prompt < 8 && gameHandler.gameState().currentBattles.empty(); ++prompt)
			{
				const auto query = gameHandler.queries->topQuery(player);
				if(!query || !query->endsByPlayerAnswer())
					break;
				pack.qid = query->queryID;
				if(prompt > 0 || !pack.reply)
					pack.reply = 1;
				replayPack(gameHandler, pack, player);
			}
			if(gameHandler.gameState().currentBattles.empty())
				throw std::runtime_error(
					"VGT monster fight choice did not start a battle: " + decision.toCompactString());
		}
		else if(choiceName == "cancel")
		{
			replayPack(gameHandler, pack, player);
			// Declining a neutral encounter can reveal a second generated pursuit
			// prompt. The single semantic cancel means to let the stack go, so carry
			// that decision through the prompt chain belonging to this encounter.
			for(int prompt = 0; prompt < 8; ++prompt)
			{
				const auto query = std::dynamic_pointer_cast<CBlockingDialogQuery>(
					gameHandler.queries->topQuery(player));
				if(!query)
					break;
				pack.qid = query->queryID;
				pack.reply = 0;
				replayPack(gameHandler, pack, player);
			}
		}
		else
			replayPack(gameHandler, pack, player);
		verifyDiscoveryCheck(decision, "discovers", player, "encounter");
		return;
	}

	if(kind == "teleport")
	{
		if(const auto * approach = findField(decision, "approach"))
		{
			JsonNode move = *approach;
			move["actor"].String() = actor;
			move["kind"].String() = "move";
			move["hero"].String() = requireString(decision, "hero");
			replayDecision(gameHandler, move);
		}
		beginDiscoveryCheck(gameHandler.gameState(), player);

		const auto query = std::dynamic_pointer_cast<CTeleportDialogQuery>(gameHandler.queries->topQuery(player));
		if(!query)
			throw std::runtime_error("VGT teleport has no active teleport query");
		QueryReply pack;
		pack.qid = query->queryID;
		if(optionalBool(decision, "random", false))
			pack.reply = -1;
		else if(const auto * exitNode = findField(decision, "exit"))
		{
			const auto exitID = resolveObjectAlias(gameHandler.gameState(), exitNode->String());
			auto exit = std::find_if(query->td.exits.begin(), query->td.exits.end(), [exitID](const auto & candidate)
			{
				return candidate.first == exitID;
			});
			if(exit == query->td.exits.end())
				throw std::runtime_error("VGT teleport exit is not offered by the active query");
			pack.reply = static_cast<int32_t>(std::distance(query->td.exits.begin(), exit));
		}
		else
			pack.reply = 0;
		replayPack(gameHandler, pack, player);

		if(const auto * destination = findField(decision, "to"))
		{
			const auto heroID = resolveObjectAlias(gameHandler.gameState(), requireString(decision, "hero"));
			const auto * hero = gameHandler.gameState().getHero(heroID);
			if(!hero || hero->visitablePos() != decodePosition(*destination))
				throw std::runtime_error("VGT teleport did not reach its declared destination");
		}
		verifyDiscoveryCheck(decision, "discovers", player, "teleport");
		return;
	}

	if(kind == "visit")
	{
		if(const auto * approach = findField(decision, "approach"))
		{
			JsonNode move = *approach;
			move["actor"].String() = actor;
			move["kind"].String() = "move";
			move["hero"].String() = requireString(decision, "hero");
			replayDecision(gameHandler, move);
		}
		beginDiscoveryCheck(gameHandler.gameState(), player);
		verifyDiscoveryCheck(decision, "discovers", player, "visit");
		return;
	}

	if(kind == "capture")
	{
		if(const auto * approach = findField(decision, "approach"))
		{
			JsonNode move = *approach;
			move["actor"].String() = actor;
			move["kind"].String() = "move";
			move["hero"].String() = requireString(decision, "hero");
			replayDecision(gameHandler, move);
		}
		beginDiscoveryCheck(gameHandler.gameState(), player);
		verifyDiscoveryCheck(decision, "discovers", player, "capture");
		return;
	}

	throw std::runtime_error("Unsupported VGT decision kind: " + kind);
}

bool isDecisionKind(const std::string & kind)
{
	static const std::set<std::string> decisionKinds = {
		"assembleArtifacts",
		"build",
		"buildBoat",
		"bulkExchangeArtifacts",
		"bulkMergeStacks",
		"bulkMoveArmy",
		"bulkSplitAndRebalanceStack",
		"bulkSplitStack",
		"buyArtifact",
		"buyArtifacts",
		"chooseSkill",
		"castleTeleportHero",
		"capture",
		"castAdventureSpell",
		"dig",
		"disbandCreature",
		"dismissHero",
		"endTurn",
		"encounter",
		"eraseArtifact",
		"exchangeArtifacts",
		"finish",
		"hire",
		"manageBackpackArtifacts",
		"manageEquippedArtifacts",
		"move",
		"moveArtifacts",
		"pauseTimer",
		"answer",
		"arrangeArmies",
		"playerMessage",
		"razeStructure",
		"ready",
		"recruit",
		"sacrificeArtifacts",
		"sacrificeCreatures",
		"sellArtifacts",
		"sellCreatures",
		"sendResources",
		"setFormation",
		"setTactics",
		"setTownName",
		"mergeStacks",
		"splitStack",
		"spellResearch",
		"swapTownHeroes",
		"swapStacks",
		"trade",
		"teleport",
		"transformUndead",
		"upgradeCreature",
		"visit",
		"visitTownBuilding",
		"learnSkills",
	};
	return decisionKinds.contains(kind);
}

bool isBattleActionKind(const std::string & kind)
{
	static const std::set<std::string> actionKinds = {
		"badMorale",
		"catapult",
		"defend",
		"endTactics",
		"heroSpell",
		"monsterSpell",
		"none",
		"retreat",
		"shoot",
		"stackHeal",
		"surrender",
		"wait",
		"walk",
		"walkAndAttack",
		"walkAndCast",
	};
	return actionKinds.contains(kind);
}

bool isEffectKind(const std::string & kind)
{
	static const std::set<std::string> effectKinds = {
		"adventureSpell",
		"appears",
		"army",
		"artifact",
		"artifacts",
		"available",
		"availability",
		"availableArtifacts",
		"availableHero",
		"bonus",
		"capture",
		"collects",
		"dayStart",
		"discovers",
		"experience",
		"finds",
		"growth",
		"heroOwner",
		"heroRecruited",
		"income",
		"levelUp",
		"joins",
		"mana",
		"objectPosition",
		"playerEnd",
		"query",
		"quest",
		"reveal",
		"remove",
		"refresh",
		"resources",
		"setExperience",
		"setMana",
		"setMovement",
		"setResources",
		"setSkills",
		"skills",
		"spells",
		"spawns",
		"stackExperience",
		"story",
		"opened",
		"town",
		"townless",
		"townHeroes",
		"turnEnd",
		"turnStart",
		"usedToday",
		"visit",
		"visibility",
		"week",
		"weeklyAvailability",
		"weeklyRewards",
	};
	return effectKinds.contains(kind);
}

void applyRecordedHeroLevelUp(CGameHandler & gameHandler, const JsonNode & node)
{
	const ObjectInstanceID heroID = resolveObjectAlias(gameHandler.gameState(), requireString(node, "hero"));
	const auto * hero = gameHandler.gameState().getHero(heroID);
	const auto query = hero
		? std::dynamic_pointer_cast<CHeroLevelUpDialogQuery>(gameHandler.queries->topQuery(hero->tempOwner))
		: nullptr;
	if(!query || !query->hero || query->hero->id != heroID)
		throw std::runtime_error("VGT level-up record has no matching active query");

	const PrimarySkill recordedPrimary = decodePrimarySkill(requireString(node, "primary"));
	if(query->hlu.primskill != recordedPrimary)
	{
		SetPrimarySkill rollback;
		rollback.id = heroID;
		rollback.which = query->hlu.primskill;
		rollback.mode = ChangeValueMode::RELATIVE;
		rollback.val = -1;
		gameHandler.sendAndApply(rollback);

		SetPrimarySkill apply;
		apply.id = heroID;
		apply.which = recordedPrimary;
		apply.mode = ChangeValueMode::RELATIVE;
		apply.val = 1;
		gameHandler.sendAndApply(apply);
		query->hlu.primskill = recordedPrimary;
	}

	query->hlu.skills.clear();
	const auto * choices = findField(node, "choices");
	if(!choices)
		return;
	if(!choices->isVector())
		throw std::runtime_error("VGT level-up choices are not a list");
	for(const auto & choice : choices->Vector())
	{
		if(!choice.isString())
			throw std::runtime_error("VGT level-up choice is not a skill identifier");
		query->hlu.skills.push_back(decodeSecondarySkill(choice.String()));
	}
}

void replayReadableDecision(
	CGameHandler & gameHandler,
	const std::string & kind,
	const JsonNode & node,
	const std::optional<std::string> & battleID = std::nullopt,
	const std::optional<std::string> & defaultActor = std::nullopt)
{
	JsonNode decision = node;
	if(decision.isNull())
		decision.setType(JsonNode::JsonType::DATA_STRUCT);
	else if(!decision.isStruct())
		throw std::runtime_error("VGT " + kind + " action is not a mapping");

	decision["kind"].String() = kind;
	if(defaultActor && !hasField(decision, "actor"))
		decision["actor"].String() = *defaultActor;
	if(battleID && !hasField(decision, "battle"))
		decision["battle"].String() = *battleID;
	const bool handlesActiveQuery = kind == "answer" || kind == "finish" ||
		kind == "chooseSkill" || kind == "teleport" ||
		(kind == "encounter" && hasField(decision, "choice"));
	if(!handlesActiveQuery)
	{
		if(const auto player = tryPlayerFromActor(requireString(decision, "actor")))
		{
			// Routine one-button UI notices are deliberately absent from the readable
			// transcript. Dismiss only acknowledgements (or dialogs explicitly marked
			// safe by the engine); decision-bearing yes/no and selection prompts remain
			// for the recorded semantic choice below.
			for(int acknowledgement = 0; acknowledgement < 16; ++acknowledgement)
			{
				const auto query = std::dynamic_pointer_cast<CBlockingDialogQuery>(
					gameHandler.queries->topQuery(*player));
				if(!query)
					break;
				if(!query->bd.safeToAutoaccept() && (query->bd.cancel() || query->bd.selection()))
				{
					throw std::runtime_error(
						"VGT transcript has no semantic choice for blocking prompt before " + kind +
						" (cancel=" + std::to_string(query->bd.cancel()) +
						", selection=" + std::to_string(query->bd.selection()) +
						", safe=" + std::to_string(query->bd.safeToAutoaccept()) +
						", components=" + std::to_string(query->bd.components.size()) +
						", text=" + query->bd.text.toString() + ")");
				}
				QueryReply reply;
				reply.qid = query->queryID;
				reply.reply = 1;
				replayPack(gameHandler, reply, *player);
			}
		}
	}
	replayDecision(gameHandler, decision);
}

int rosterStack(const std::map<std::string, int> & roster, const std::string & unit)
{
	if(unit == "hero")
		return -1;
	const auto position = roster.find(unit);
	if(position == roster.end())
		throw std::runtime_error("Unknown VGT battle unit: " + unit);
	return position->second;
}

std::string battleActorForSide(CGameHandler & gameHandler, const std::string & battleID, BattleSide side)
{
	const auto * battle = gameHandler.gameState().getBattle(BattleID(std::stoi(battleID)));
	if(!battle)
		throw std::runtime_error("Unknown VGT battle: " + battleID);
	const PlayerColor player = battle->getSidePlayer(side);
	return player.isValidPlayer() ? player.toString() : "world";
}

std::string resolveLiveBattleID(CGameHandler & gameHandler, const std::string & recordedBattleID)
{
	const BattleID recordedID(std::stoi(recordedBattleID));
	if(gameHandler.gameState().getBattle(recordedID))
		return recordedBattleID;

	// Battle numbers are capture-local infrastructure identifiers. They can have
	// gaps that are not represented by the semantic transcript (for example a
	// battle setup that never reaches an action block). A readable battle block
	// is applied immediately after the decision that opened it, so the sole live
	// battle is the unambiguous replay target.
	if(gameHandler.gameState().currentBattles.size() == 1)
		return std::to_string(gameHandler.gameState().currentBattles.front()->battleID.getNum());

	throw std::runtime_error(
		"Cannot match recorded VGT battle " + recordedBattleID + " to a live battle");
}

std::string inferredBattleSide(const JsonNode & action)
{
	if(const auto * side = findField(action, "side"))
		return side->String();
	if(const auto * unit = findField(action, "unit"); unit && unit->isString())
	{
		if(unit->String().starts_with("attacker/"))
			return "attacker";
		if(unit->String().starts_with("defender/"))
			return "defender";
	}
	throw std::runtime_error("VGT battle action has neither a side nor a side-qualified unit");
}

void replayReadableBattleAction(
	CGameHandler & gameHandler,
	const std::string & kind,
	const JsonNode & node,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	if(!node.isStruct())
		throw std::runtime_error("VGT " + kind + " battle action is not a mapping");

	JsonNode action = node;
	const std::string side = inferredBattleSide(action);
	const BattleSide battleSide = side == "defender" ? BattleSide::DEFENDER : BattleSide::ATTACKER;
	const std::string actor = hasField(action, "actor")
		? requireString(action, "actor")
		: battleActorForSide(gameHandler, battleID, battleSide);
	action.Struct().erase("actor");
	if(!hasField(action, "side"))
		action["side"].String() = side;
	if(const auto * unit = findField(action, "unit"))
	{
		const int requestedStack = rosterStack(roster, unit->String());
		if(const auto * battle = gameHandler.gameState().getBattle(BattleID(std::stoi(battleID)));
			battle && !battle->battleTacticDist())
		{
			if(requestedStack >= 0)
			{
				const auto * requested = battle->battleGetStackByID(requestedStack, false);
				if(!requested || !requested->alive())
					throw std::runtime_error(
						"Recorded VGT battle action references dead unit " + unit->String());
			}
			const auto * active = battle->battleActiveUnit();
			int activeStack = requestedStack;
			if(activeStack < 0 && (!active || active->unitSide() != battleSide))
			{
				// A hero spell does not name the creature whose turn made the cast
				// possible. Reconstructed initiative can differ after frozen damage
				// corrections, so anchor the spell to a surviving unit on its side.
				for(const auto * stack : battle->battleGetAllStacks(false))
				{
					if(stack && stack->alive() && stack->unitSide() == battleSide)
					{
						activeStack = static_cast<int>(stack->unitId());
						break;
					}
				}
			}
			if(activeStack >= 0 && (!active || static_cast<int>(active->unitId()) != activeStack))
			{
				// Equal-speed units can be ordered differently when replay does not run
				// the original battle AI. The transcript's acting unit is the frozen
				// initiative fact, so restore it before applying the recorded decision.
				BattleSetActiveStack setActive;
				setActive.battleID = BattleID(std::stoi(battleID));
				setActive.stack = activeStack;
				setActive.reason = BattleUnitTurnReason::TURN_QUEUE;
				gameHandler.sendAndApply(setActive);
			}
		}
		action["stack"].Integer() = requestedStack;
		action.Struct().erase("unit");
	}
	if(auto targets = action.Struct().find("target"); targets != action.Struct().end() && targets->second.isVector())
	{
		for(auto & target : targets->second.Vector())
		{
			if(const auto * unit = findField(target, "unit"))
			{
				target["unit"].Integer() = rosterStack(roster, unit->String());
			}
		}
	}
	action["action"].String() = kind;

	JsonNode decision;
	decision["actor"].String() = actor;
	decision["kind"].String() = "battleAction";
	decision["battle"].String() = battleID;
	decision["action"] = action;
	replayDecision(gameHandler, decision);
}

void replaySemanticBattleMove(
	CGameHandler & gameHandler,
	const JsonNode & node,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	const auto & path = requireField(node, "path");
	if(!path.isVector() || path.Vector().empty())
		throw std::runtime_error("VGT battle move path must be a nonempty list");
	JsonNode action;
	if(const auto * actor = findField(node, "actor"))
		action["actor"].String() = actor->String();
	if(const auto * side = findField(node, "side"))
		action["side"].String() = side->String();
	action["unit"].String() = requireString(node, "unit");
	JsonNode target;
	target["hex"].Integer() = requireInteger(node, "to");
	action["target"].Vector().push_back(std::move(target));
	try
	{
		replayReadableBattleAction(gameHandler, "walk", action, battleID, roster);
	}
	catch(const std::runtime_error & error)
	{
		std::vector<std::string> walls;
		const auto * battle = gameHandler.gameState().getBattle(BattleID(std::stoi(battleID)));
		if(battle)
		{
			for(int part = 0; part < static_cast<int>(EWallPart::PARTS_COUNT); ++part)
				walls.push_back(std::to_string(part) + ":" +
					std::to_string(static_cast<int>(battle->getWallState(static_cast<EWallPart>(part)))));
		}
		const int stackID = rosterStack(roster, requireString(node, "unit"));
		const auto * stack = battle ? battle->battleGetStackByID(stackID, false) : nullptr;
		throw std::runtime_error(
			"VGT battle move from " + std::to_string(stack ? stack->getPosition().toInt() : -1) +
			" with walls [" + boost::algorithm::join(walls, ", ") + "]: " + error.what());
	}
}

void replaySemanticCatapultEffect(
	CGameHandler & gameHandler,
	const JsonNode & node,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	const int part = static_cast<int>(requireInteger(node, "part"));
	const int tile = static_cast<int>(requireInteger(node, "tile"));
	const int damage = static_cast<int>(requireInteger(node, "damage"));
	if(part < 0 || part >= static_cast<int>(EWallPart::PARTS_COUNT) ||
		tile < std::numeric_limits<int16_t>::min() || tile > std::numeric_limits<int16_t>::max() ||
		damage < 0 || damage > std::numeric_limits<uint8_t>::max())
		throw std::runtime_error("Invalid semantic VGT catapult effect");

	CatapultAttack pack;
	pack.battleID = BattleID(std::stoi(battleID));
	pack.attackedPart = static_cast<EWallPart>(part);
	pack.destinationTile = static_cast<int16_t>(tile);
	pack.damageDealt = static_cast<uint8_t>(damage);
	if(const auto * killed = findField(node, "killedTowerShooter"))
		pack.killedTowerShooter = rosterStack(roster, killed->String());
	if(const auto * attacker = findField(node, "attacker"); attacker && attacker->String() != "spell")
		pack.attacker = rosterStack(roster, attacker->String());
	gameHandler.sendAndApply(pack);
}

struct BattleHealthSnapshot
{
	int64_t health = 0;
	int64_t count = 0;
	int64_t maxHealth = 0;
};

std::map<std::string, BattleHealthSnapshot> captureBattleHealth(
	const CGameHandler & gameHandler,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	std::map<std::string, BattleHealthSnapshot> result;
	const auto * battle = gameHandler.gameState().getBattle(BattleID(std::stoi(battleID)));
	if(!battle)
		return result;
	for(const auto & [alias, stackID] : roster)
	{
		if(const auto * stack = battle->battleGetStackByID(stackID, false))
			result[alias] = {
				stack->getAvailableHealth(),
				stack->getCount(),
				stack->getMaxHealth()};
	}
	return result;
}

std::map<std::string, int64_t> recordedBattleDamage(const JsonNode & attack)
{
	std::map<std::string, int64_t> result;
	auto addHit = [&](const JsonNode & hit)
	{
		const auto * target = findField(hit, "target");
		const auto * damage = findField(hit, "damage");
		if(target && target->isString() && damage && damage->isNumber())
			result[target->String()] += damage->Integer();
	};

	if(const auto * hits = findField(attack, "hits"); hits && hits->isVector())
	{
		for(const auto & hit : hits->Vector())
			addHit(hit);
	}
	else if(const auto * target = findField(attack, "target"); target && target->isString())
	{
		if(const auto * damage = findField(attack, "damage"); damage && damage->isNumber())
			result[target->String()] += damage->Integer();
	}

	if(const auto * retaliation = findField(attack, "retaliation"); retaliation && retaliation->isStruct())
	{
		if(const auto * hits = findField(*retaliation, "hits"); hits && hits->isVector())
		{
			for(const auto & hit : hits->Vector())
				addHit(hit);
		}
		else if(const auto * attacker = findField(attack, "by"); attacker && attacker->isString())
		{
			if(const auto * damage = findField(*retaliation, "damage"); damage && damage->isNumber())
				result[attacker->String()] += damage->Integer();
		}
	}

	if(const auto * effects = findField(attack, "effects"); effects && effects->isVector())
	{
		for(const auto & effect : effects->Vector())
			addHit(effect);
	}
	return result;
}

std::map<std::string, int64_t> recordedBattleKills(const JsonNode & attack)
{
	std::map<std::string, int64_t> result;
	auto addHit = [&](const JsonNode & hit)
	{
		const auto * target = findField(hit, "target");
		const auto * killed = findField(hit, "killed");
		if(target && target->isString() && killed && killed->isNumber())
			result[target->String()] += killed->Integer();
	};

	if(const auto * hits = findField(attack, "hits"); hits && hits->isVector())
	{
		for(const auto & hit : hits->Vector())
			addHit(hit);
	}
	else
		addHit(attack);

	if(const auto * retaliation = findField(attack, "retaliation"); retaliation && retaliation->isStruct())
	{
		if(const auto * hits = findField(*retaliation, "hits"); hits && hits->isVector())
		{
			for(const auto & hit : hits->Vector())
				addHit(hit);
		}
		else if(const auto * attacker = findField(attack, "by"); attacker && attacker->isString())
		{
			if(const auto * killed = findField(*retaliation, "killed"); killed && killed->isNumber())
				result[attacker->String()] += killed->Integer();
		}
	}

	if(const auto * effects = findField(attack, "effects"); effects && effects->isVector())
	{
		for(const auto & effect : effects->Vector())
			addHit(effect);
	}
	return result;
}

struct RecordedBattleHealth
{
	int64_t count = 0;
	int64_t topHP = 0;
};

std::map<std::string, RecordedBattleHealth> recordedBattleAfters(const JsonNode & attack)
{
	std::map<std::string, RecordedBattleHealth> result;
	auto add = [&](const JsonNode & hit)
	{
		const auto * target = findField(hit, "target");
		const auto * after = findField(hit, "after");
		if(!target || !target->isString() || !after || !after->isStruct())
			return;
		const auto * topHP = findField(*after, "topHp");
		result[target->String()] = {
			requireInteger(*after, "count"),
			topHP ? topHP->Integer() : 0};
	};

	if(const auto * hits = findField(attack, "hits"); hits && hits->isVector())
	{
		for(const auto & hit : hits->Vector())
			add(hit);
	}
	else
		add(attack);

	if(const auto * retaliation = findField(attack, "retaliation"); retaliation && retaliation->isStruct())
	{
		if(const auto * hits = findField(*retaliation, "hits"); hits && hits->isVector())
		{
			for(const auto & hit : hits->Vector())
				add(hit);
		}
		else if(const auto * attacker = findField(attack, "by"); attacker && attacker->isString())
		{
			JsonNode hit = *retaliation;
			hit["target"].String() = attacker->String();
			add(hit);
		}
	}
	if(const auto * effects = findField(attack, "effects"); effects && effects->isVector())
	{
		for(const auto & effect : effects->Vector())
			add(effect);
	}
	return result;
}

std::map<std::string, RecordedBattleHealth> recordedBattleRebirths(const JsonNode & attack)
{
	std::map<std::string, RecordedBattleHealth> result;
	auto add = [&](const JsonNode & hit)
	{
		const auto * target = findField(hit, "target");
		const auto * reborn = findField(hit, "reborn");
		if(!target || !target->isString() || !reborn || !reborn->isStruct())
			return;
		const auto * hp = findField(*reborn, "topHp");
		result[target->String()] = {
			requireInteger(*reborn, "count"),
			hp ? hp->Integer() : 0};
	};

	if(const auto * hits = findField(attack, "hits"); hits && hits->isVector())
	{
		for(const auto & hit : hits->Vector())
			add(hit);
	}
	else
		add(attack);

	if(const auto * retaliation = findField(attack, "retaliation"); retaliation && retaliation->isStruct())
	{
		if(const auto * hits = findField(*retaliation, "hits"); hits && hits->isVector())
		{
			for(const auto & hit : hits->Vector())
				add(hit);
		}
		else if(const auto * attacker = findField(attack, "by"); attacker && attacker->isString())
		{
			JsonNode hit = *retaliation;
			hit["target"].String() = attacker->String();
			add(hit);
		}
	}
	return result;
}

void applyRecordedBattleDamage(
	CGameHandler & gameHandler,
	const std::string & battleID,
	const std::map<std::string, int> & roster,
	const std::map<std::string, BattleHealthSnapshot> & healthBefore,
	const std::map<std::string, int64_t> & damage,
	const std::map<std::string, int64_t> & kills = {},
	const std::map<std::string, RecordedBattleHealth> & afters = {},
	const std::map<std::string, RecordedBattleHealth> & rebirths = {})
{
	auto * battle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
	if(!battle)
		return;

	BattleUnitsChanged changes;
	changes.battleID = BattleID(std::stoi(battleID));
	for(const auto & [alias, amount] : damage)
	{
		if(!healthBefore.contains(alias) || !roster.contains(alias))
			throw std::runtime_error("Invalid recorded VGT battle damage for " + alias);
		const auto * stack = battle->battleGetStackByID(roster.at(alias), false);
		if(!stack)
			throw std::runtime_error("Recorded VGT battle damage references missing unit " + alias);

		auto state = stack->acquireState();
		// An effect applied by the same strike can change maximum health (for
		// example Wyvern Monarch poison). The pre-strike total then belongs to a
		// different health scale; keep the correction within the live unit's legal
		// total and let the explicit outcome enforce permanent survivor counts.
		const auto frozenAfter = afters.find(alias);
		const auto rebirth = rebirths.find(alias);
		int64_t recordedHealth = healthBefore.at(alias).health - amount;
		if(frozenAfter != afters.end())
			recordedHealth = frozenAfter->second.count == 0
				? 0
				: (frozenAfter->second.count - 1) * state->getMaxHealth() + frozenAfter->second.topHP;
		else if(rebirth != rebirths.end())
			recordedHealth = (rebirth->second.count - 1) * state->getMaxHealth() + rebirth->second.topHP;
		else if(const auto killed = kills.find(alias); killed != kills.end())
		{
			const int64_t beforeCount = healthBefore.at(alias).count;
			const int64_t expectedCount = std::max<int64_t>(0, beforeCount - killed->second);
			if(expectedCount == 0)
				recordedHealth = 0;
			else
				recordedHealth = std::clamp<int64_t>(
					recordedHealth,
					(expectedCount - 1) * state->getMaxHealth() + 1,
					expectedCount * state->getMaxHealth());
		}
		const int64_t targetHealth = std::clamp<int64_t>(recordedHealth, 0, state->getTotalHealth());
		const int64_t currentHealth = state->getAvailableHealth();
		if(targetHealth == currentHealth)
			continue;
		if(targetHealth == 0 && currentHealth > 0 && state->hasBonusOfType(BonusType::REBIRTH))
		{
			// The damage was lethal, but the applied attack already consumed the
			// unit's rebirth and restored it. A damage-only correction must not kill
			// that explicitly surviving incarnation a second time.
			continue;
		}
		if(targetHealth < currentHealth)
		{
			int64_t correction = currentHealth - targetHealth;
			state->damage(correction);
		}
		else
		{
			if(state->cloned)
				continue;
			int64_t correction = targetHealth - currentHealth;
			state->heal(correction, EHealLevel::RESURRECT, EHealPower::PERMANENT);
		}
		if(state->getAvailableHealth() != targetHealth)
			throw std::runtime_error("Unable to apply recorded VGT battle damage for " + alias);

		UnitChanges change(stack->unitId(), BattleChanges::EOperation::UPDATE);
		change.healthDelta = targetHealth - currentHealth;
		change.data = state->save();
		changes.changedStacks.push_back(std::move(change));
	}
	if(!changes.changedStacks.empty())
		gameHandler.sendAndApply(changes);
}

void applyRecordedBattleHealing(
	CGameHandler & gameHandler,
	const std::string & battleID,
	const std::map<std::string, int> & roster,
	const std::string & alias,
	int64_t count,
	int64_t topHP)
{
	if(count <= 0 || topHP <= 0 || !roster.contains(alias))
		throw std::runtime_error("Invalid recorded VGT battle healing for " + alias);
	auto * battle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
	const auto * stack = battle ? battle->battleGetStackByID(roster.at(alias), false) : nullptr;
	if(!stack)
		throw std::runtime_error("Recorded VGT battle healing references missing unit " + alias);

	auto state = stack->acquireState();
	if(topHP > state->getMaxHealth())
		throw std::runtime_error("Recorded VGT battle healing exceeds maximum health for " + alias);
	const int64_t targetHealth = (count - 1) * state->getMaxHealth() + topHP;
	if(targetHealth > state->getTotalHealth())
		throw std::runtime_error("Recorded VGT battle healing exceeds stack health for " + alias);
	const int64_t currentHealth = state->getAvailableHealth();
	if(targetHealth == currentHealth)
		return;
	if(targetHealth < currentHealth)
	{
		int64_t correction = currentHealth - targetHealth;
		state->damage(correction);
	}
	else
	{
		int64_t correction = targetHealth - currentHealth;
		state->heal(correction, EHealLevel::RESURRECT, EHealPower::PERMANENT);
	}
	if(state->getAvailableHealth() != targetHealth)
		throw std::runtime_error("Unable to apply recorded VGT battle healing for " + alias);

	BattleUnitsChanged changes;
	changes.battleID = BattleID(std::stoi(battleID));
	UnitChanges change(stack->unitId(), BattleChanges::EOperation::UPDATE);
	change.healthDelta = targetHealth - currentHealth;
	change.data = state->save();
	changes.changedStacks.push_back(std::move(change));
	gameHandler.sendAndApply(changes);
}

void replaySemanticBattleHealing(
	CGameHandler & gameHandler,
	const JsonNode & node,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	requireString(node, "by");
	const std::string target = requireString(node, "target");
	const int64_t amount = requireInteger(node, "amount");
	const JsonNode & after = requireField(node, "after");
	if(amount <= 0 || !after.isStruct())
		throw std::runtime_error("Invalid semantic VGT battle healing");
	applyRecordedBattleHealing(
		gameHandler,
		battleID,
		roster,
		target,
		requireInteger(after, "count"),
		requireInteger(after, "topHp"));
}

bool replayRawBattleHealing(
	CGameHandler & gameHandler,
	const JsonNode & record,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	const auto * event = findField(record, "event");
	const auto * changes = findField(record, "changes");
	if(!event || !event->isString() || event->String() != "unitsChanged" ||
		!changes || !changes->isVector())
		return false;

	struct Healing
	{
		std::string target;
		int64_t count = 0;
		int64_t topHP = 0;
	};
	std::vector<Healing> healing;
	for(const auto & change : changes->Vector())
	{
		const auto * operation = findField(change, "operation");
		const auto * healthDelta = findField(change, "healthDelta");
		const auto * state = findField(change, "state");
		const auto * health = state && state->isStruct() ? findField(*state, "health") : nullptr;
		const auto * topHP = health && health->isStruct() ? findField(*health, "firstHPleft") : nullptr;
		if(!operation || operation->String() != "update" || !healthDelta ||
			healthDelta->Integer() <= 0 || !health || !topHP)
			return false;
		const auto * fullUnits = findField(*health, "fullUnits");
		healing.push_back({
			requireString(change, "unit"),
			(fullUnits ? fullUnits->Integer() : 0) + 1,
			topHP->Integer()});
	}
	if(healing.empty())
		return false;
	for(const auto & value : healing)
		applyRecordedBattleHealing(
			gameHandler, battleID, roster, value.target, value.count, value.topHP);
	return true;
}

void restoreRecordedBattlePosition(
	CGameHandler & gameHandler,
	const std::string & battleID,
	int stackID,
	int hex)
{
	auto * battle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
	const auto * stack = battle ? battle->battleGetStackByID(stackID, false) : nullptr;
	if(!stack || !stack->alive())
		throw std::runtime_error("Cannot restore recorded VGT position for missing battle unit");
	if(stack->getPosition().toInt() == hex)
		return;

	BattleStackMoved moved;
	moved.battleID = BattleID(std::stoi(battleID));
	moved.stack = static_cast<uint32_t>(stackID);
	moved.tilesToMove.insert(BattleHex(hex));
	gameHandler.sendAndApply(moved);
}

void replaySemanticBattleAttack(
	CGameHandler & gameHandler,
	const JsonNode & node,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	// Server-selected war-machine shots are narrative events, not player
	// decisions. The battle engine reproduces them while advancing initiative.
	if(optionalBool(node, "automatic", false))
		return;

	const auto healthBefore = captureBattleHealth(gameHandler, battleID, roster);
	JsonNode action;
	if(const auto * actor = findField(node, "actor"))
		action["actor"].String() = actor->String();
	if(const auto * side = findField(node, "side"))
		action["side"].String() = side->String();
	action["unit"].String() = requireString(node, "by");
	const int from = static_cast<int>(requireInteger(node, "from"));
	const int targetAt = static_cast<int>(requireInteger(node, "targetAt"));
	const bool ranged = optionalBool(node, "ranged", false);
	if(ranged)
	{
		restoreRecordedBattlePosition(
			gameHandler,
			battleID,
			rosterStack(roster, requireString(node, "by")),
			from);
		JsonNode target;
		if(const auto * targetUnit = findField(node, "target"))
		{
			target["unit"].String() = targetUnit->String();
			restoreRecordedBattlePosition(
				gameHandler,
				battleID,
				rosterStack(roster, targetUnit->String()),
				targetAt);
		}
		target["hex"].Integer() = targetAt;
		action["target"].Vector().push_back(std::move(target));
	}
	else
	{
		JsonNode attackerHex;
		attackerHex["hex"].Integer() = from;
		action["target"].Vector().push_back(std::move(attackerHex));
		JsonNode targetHex;
		targetHex["hex"].Integer() = targetAt;
		action["target"].Vector().push_back(std::move(targetHex));
		if(const auto * returnsTo = findField(node, "returnsTo"))
		{
			JsonNode returnHex;
			returnHex["hex"].Integer() = returnsTo->Integer();
			action["target"].Vector().push_back(std::move(returnHex));
		}
		else if(const auto * returnPath = findField(node, "returnPath");
			returnPath && returnPath->isVector() && !returnPath->Vector().empty())
		{
			JsonNode returnHex;
			returnHex["hex"].Integer() = returnPath->Vector().back().Integer();
			action["target"].Vector().push_back(std::move(returnHex));
		}
	}
	if(const auto * hits = findField(node, "hits"); hits && hits->isVector())
	{
		for(size_t index = 1; index < hits->Vector().size(); ++index)
		{
			const auto & hit = hits->Vector()[index];
			const auto * target = findField(hit, "target");
			if(target && target->isString())
				action["secondaryTargets"].Vector().emplace_back(rosterStack(roster, target->String()));
		}
	}
	try
	{
		replayReadableBattleAction(
			gameHandler,
			ranged ? "shoot" : "walkAndAttack",
			action,
			battleID,
			roster);
	}
	catch(const std::runtime_error & error)
	{
		const int stackID = rosterStack(roster, requireString(node, "by"));
		auto * battle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
		const auto * stack = battle ? battle->battleGetStackByID(stackID, false) : nullptr;
		if(ranged)
		{
			std::map<int, std::string> aliases;
			for(const auto & [alias, id] : roster)
				aliases[id] = alias;
			std::vector<std::string> positions;
			std::vector<std::string> adjacent;
			if(battle)
			{
				for(const auto * live : battle->battleGetAllStacks(false))
				{
					if(!live || !live->alive())
						continue;
					const auto alias = aliases.find(static_cast<int>(live->unitId()));
					positions.push_back(
						(alias == aliases.end() ? std::to_string(live->unitId()) : alias->second) +
						"@" + std::to_string(live->getPosition().toInt()));
				}
				if(stack)
				{
					for(const auto * live : battle->battleAdjacentUnits(stack))
					{
						const auto alias = aliases.find(static_cast<int>(live->unitId()));
						adjacent.push_back(
							alias == aliases.end() ? std::to_string(live->unitId()) : alias->second);
					}
				}
			}
			throw std::runtime_error(
				std::string(error.what()) +
				"; recorded shooter=" + std::to_string(from) +
				", target=" + std::to_string(targetAt) +
				", stackCanShoot=" + std::to_string(stack && stack->canShoot()) +
				", blocked=" + std::to_string(stack && battle && battle->battleIsUnitBlocked(stack)) +
				", canShoot=" + std::to_string(stack && battle && battle->battleCanShoot(stack)) +
				", canShootTarget=" + std::to_string(
					stack && battle && battle->battleCanShoot(stack, BattleHex(targetAt))) +
				", adjacent=[" + boost::algorithm::join(adjacent, ", ") +
				"], live positions=[" + boost::algorithm::join(positions, ", ") + "]");
		}
		const std::string_view message(error.what());
		const bool interruptedMovement =
			message.find("battle action was rejected") != std::string_view::npos ||
			message.find("Movement terminated abnormally") != std::string_view::npos;
		const bool canRestoreRecordedPath =
			!ranged && interruptedMovement && stack && stack->getPosition().toInt() != from;
		if(!canRestoreRecordedPath)
			throw;

		// `from` is a frozen tactical fact. Normally the battle processor
		// rediscovers the path and applies all of its mechanics. If transient
		// reconstructed blockers make that exact strike unreachable, restore the
		// recorded movement segment and retry from its authoritative attack cell.
		BattleStackMoved moved;
		moved.battleID = BattleID(std::stoi(battleID));
		moved.stack = static_cast<uint32_t>(stackID);
		if(const auto * via = findField(node, "via"); via && via->isVector())
		{
			for(const auto & hex : via->Vector())
				moved.tilesToMove.insert(BattleHex(static_cast<int>(hex.Integer())));
		}
		moved.tilesToMove.insert(BattleHex(from));
		moved.distance = static_cast<int>(moved.tilesToMove.size());
		gameHandler.sendAndApply(moved);
		try
		{
			replayReadableBattleAction(gameHandler, "walkAndAttack", action, battleID, roster);
		}
		catch(const std::runtime_error & retryError)
		{
			battle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
			stack = battle ? battle->battleGetStackByID(stackID, false) : nullptr;
			throw std::runtime_error(
				std::string(retryError.what()) +
				"; retry position=" + std::to_string(stack ? stack->getPosition().toInt() : -1) +
				", occupied=" + std::to_string(stack ? stack->occupiedHex().toInt() : -1) +
				", recorded from=" + std::to_string(from));
		}
	}
	applyRecordedBattleDamage(
		gameHandler,
		battleID,
		roster,
		healthBefore,
		recordedBattleDamage(node),
		recordedBattleKills(node),
		recordedBattleAfters(node),
		recordedBattleRebirths(node));
}

void replaySemanticBattleCast(
	CGameHandler & gameHandler,
	const JsonNode & node,
	const std::string & battleID,
	const std::map<std::string, int> & roster)
{
	const auto healthBefore = captureBattleHealth(gameHandler, battleID, roster);
	JsonNode action;
	if(const auto * actor = findField(node, "actor"))
		action["actor"].String() = actor->String();
	if(const auto * side = findField(node, "side"))
		action["side"].String() = side->String();
	const std::string caster = requireString(node, "caster");
	const bool stackCast = roster.contains(caster);
	action["unit"].String() = stackCast ? caster : "hero";
	action["spell"].String() = requireString(node, "spell");
	action["target"] = requireField(node, "aim");
	replayReadableBattleAction(
		gameHandler,
		stackCast ? "monsterSpell" : "heroSpell",
		action,
		battleID,
		roster);
	std::map<std::string, int64_t> healthLoss;
	if(const auto * target = findField(node, "target"); target && target->isString())
	{
		if(const auto * damage = findField(node, "damage"); damage && damage->isNumber())
			healthLoss[target->String()] += damage->Integer();
		if(const auto * healed = findField(node, "healed"); healed && healed->isNumber())
			healthLoss[target->String()] -= healed->Integer();
	}
	applyRecordedBattleDamage(gameHandler, battleID, roster, healthBefore, healthLoss);
}

std::set<ObjectInstanceID> battleRandomizerParticipants(const IBattleInfo & battle)
{
	std::set<ObjectInstanceID> result;
	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		if(const auto * army = battle.getSideArmy(side))
			result.insert(army->id);
		if(const auto * hero = battle.getSideHero(side))
			result.insert(hero->id);
	}
	return result;
}

std::set<HeroTypeID> battleRandomizerHeroes(const IBattleInfo & battle)
{
	std::set<HeroTypeID> result;
	for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		const auto * hero = battle.getSideHero(side);
		if(hero && hero->getHeroTypeID().hasValue())
			result.insert(hero->getHeroTypeID());
	}
	return result;
}

void fastForwardBattle(
	CGameHandler & gameHandler,
	const std::string & battleID,
	const std::map<std::string, int> & roster,
	const JsonNode & outcome,
	const std::set<ObjectInstanceID> & randomizerParticipants,
	const std::set<HeroTypeID> & randomizerHeroes)
{
	const BattleID liveBattleID(std::stoi(battleID));
	auto * battle = gameHandler.gs->getBattle(liveBattleID);
	if(!battle)
		throw std::runtime_error("VGT fast-forward references missing battle: " + battleID);

	const EBattleResult result = decodeBattleResult(requireString(outcome, "result"));
	const BattleSide winnerSide = decodeBattleSide(requireString(outcome, "winnerSide"));
	if(result == EBattleResult::SURRENDER)
	{
		const BattleSide surrenderingSide = battle->otherSide(winnerSide);
		const PlayerColor player = battle->sideToPlayer(surrenderingSide);
		const int cost = battle->battleGetSurrenderCost(player);
		if(cost < 0 || gameHandler.gameInfo().getResource(player, EGameResID::GOLD) < cost)
			throw std::runtime_error("Recorded VGT surrender is not legal in the replayed battle");
		gameHandler.giveResource(player, EGameResID::GOLD, -cost);
	}

	const JsonNode & survivors = requireField(outcome, "survivors");
	if(!survivors.isStruct())
		throw std::runtime_error("VGT battle outcome survivors is not a mapping");
	std::map<int, std::string> aliasesByStack;
	for(const auto & [alias, stack] : roster)
		aliasesByStack[stack] = alias;
	for(const auto & [alias, count] : survivors.Struct())
	{
		if(!count.isNumber() || count.Integer() <= 0)
			throw std::runtime_error("VGT battle survivor count must be positive: " + alias);
		if(!roster.contains(alias))
			throw std::runtime_error("VGT battle survivor is absent from the roster: " + alias);
	}
	const JsonNode * createdUnits = findField(outcome, "createdUnits");
	if(createdUnits && !createdUnits->isStruct())
		throw std::runtime_error("VGT battle outcome createdUnits is not a mapping");

	BattleUnitsChanged changes;
	changes.battleID = liveBattleID;
	std::set<std::string> seenAliases;
	for(const auto * stack : battle->battleGetAllStacks(true))
	{
		if(!stack || stack->summoned || stack->isTurret())
			continue;
		const auto aliasIter = aliasesByStack.find(static_cast<int>(stack->unitId()));
		if(aliasIter == aliasesByStack.end())
			throw std::runtime_error("Live VGT battle stack is absent from the recorded roster");
		const std::string & alias = aliasIter->second;
		seenAliases.insert(alias);
		const auto survivorIter = survivors.Struct().find(alias);
		const int targetCount = survivorIter == survivors.Struct().end()
			? 0
			: static_cast<int>(survivorIter->second.Integer());

		auto state = stack->acquireState();
		const int64_t currentHealth = state->getAvailableHealth();
		const int64_t targetHealth = static_cast<int64_t>(targetCount) * state->getMaxHealth();
		if(targetHealth < currentHealth)
		{
			int64_t damage = currentHealth - targetHealth;
			state->damage(damage);
		}
		else if(targetHealth > currentHealth)
		{
			int64_t healing = targetHealth - currentHealth;
			state->heal(healing, EHealLevel::RESURRECT, EHealPower::PERMANENT);
		}
		if(state->getCount() != targetCount)
			throw std::runtime_error("Unable to apply VGT survivor count for " + alias);

		UnitChanges change(stack->unitId(), BattleChanges::EOperation::UPDATE);
		change.healthDelta = targetHealth - currentHealth;
		change.data = state->save();
		changes.changedStacks.push_back(std::move(change));
	}
	if(!changes.changedStacks.empty())
		gameHandler.sendAndApply(changes);

	if(createdUnits)
	{
		for(const auto & [alias, created] : createdUnits->Struct())
		{
			if(!created.isStruct() || !roster.contains(alias) || !survivors.Struct().contains(alias) || seenAliases.contains(alias))
				throw std::runtime_error("Invalid VGT permanently created battle unit: " + alias);
			const int initialCount = static_cast<int>(requireInteger(created, "count"));
			const int survivorCount = static_cast<int>(survivors.Struct().at(alias).Integer());
			if(initialCount <= 0 || survivorCount > initialCount)
				throw std::runtime_error("Invalid VGT permanently created battle unit count: " + alias);

			battle::UnitInfo info;
			info.id = roster.at(alias);
			info.count = initialCount;
			info.type = decodeCreature(requireString(created, "creature"));
			info.side = alias.starts_with("attacker/") ? BattleSide::ATTACKER : BattleSide::DEFENDER;
			info.position = BattleHex(static_cast<si16>(requireInteger(created, "hex")));
			info.summoned = false;
			JsonNode data;
			info.save(data);

			BattleUnitsChanged addition;
			addition.battleID = liveBattleID;
			UnitChanges added(info.id, BattleChanges::EOperation::ADD);
			added.data = std::move(data);
			addition.changedStacks.push_back(std::move(added));
			gameHandler.sendAndApply(addition);

			auto * stack = battle->battleGetStackByID(info.id, false);
			if(!stack)
				throw std::runtime_error("Unable to create VGT permanent battle unit: " + alias);
			auto state = stack->acquireState();
			const int64_t currentHealth = state->getAvailableHealth();
			const int64_t targetHealth = static_cast<int64_t>(survivorCount) * state->getMaxHealth();
			int64_t damage = currentHealth - targetHealth;
			state->damage(damage);
			BattleUnitsChanged update;
			update.battleID = liveBattleID;
			UnitChanges changed(info.id, BattleChanges::EOperation::UPDATE);
			changed.healthDelta = targetHealth - currentHealth;
			changed.data = state->save();
			update.changedStacks.push_back(std::move(changed));
			gameHandler.sendAndApply(update);
			seenAliases.insert(alias);
		}
	}
	for(const auto & [alias, count] : survivors.Struct())
	{
		if(!seenAliases.contains(alias))
			throw std::runtime_error("VGT fast-forward lacks createdUnits state for dynamic survivor: " + alias);
	}

	if(const auto * mana = findField(outcome, "mana"))
	{
		if(!mana->isStruct())
			throw std::runtime_error("VGT battle outcome mana is not a mapping");
		for(const auto & [hero, amount] : mana->Struct())
		{
			if(!amount.isNumber())
				throw std::runtime_error("VGT battle outcome mana change is not numeric: " + hero);
			const ObjectInstanceID heroID = resolveObjectAlias(gameHandler.gameState(), hero);
			std::optional<int32_t> initialMana;
			for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
			{
				const auto & battleSide = battle->getSide(side);
				if(battleSide.heroID == heroID)
					initialMana = battleSide.initialMana;
			}
			if(!initialMana)
				throw std::runtime_error("VGT battle outcome mana hero is not a battle participant: " + hero);
			SetMana change;
			change.hid = heroID;
			change.val = static_cast<si32>(*initialMana + amount.Integer());
			change.mode = ChangeValueMode::ABSOLUTE;
			gameHandler.sendAndApply(change);
		}
	}

	BattleSideArray<TExpType> experience{0, 0};
	if(const auto * recordedExperience = findField(outcome, "experience"))
	{
		if(!recordedExperience->isStruct())
			throw std::runtime_error("VGT battle outcome experience is not a mapping");
		for(const auto & [hero, amount] : recordedExperience->Struct())
		{
			if(!amount.isNumber() || amount.Integer() < 0)
				throw std::runtime_error("Invalid VGT battle outcome experience for " + hero);
			const ObjectInstanceID heroID = resolveObjectAlias(gameHandler.gameState(), hero);
			std::optional<BattleSide> heroSide;
			for(const auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
			{
				if(battle->getSide(side).heroID == heroID)
					heroSide = side;
			}
			if(!heroSide)
				throw std::runtime_error("VGT battle outcome experience hero is not a participant: " + hero);
			experience[*heroSide] = amount.Integer();
		}
	}

	gameHandler.randomizer->loadVGTBattleJson(
		requireField(outcome, "randomBeforeAftermath"), randomizerParticipants, randomizerHeroes);
	gameHandler.battles->setBattleResultFromReplay(*battle, result, winnerSide, experience);
}

void applyBattleBlock(CGameHandler & gameHandler, const JsonNode & node, bool fastForwardBattles)
{
	const std::string recordedBattleID = battleAliasText(requireField(node, "id"));
	const std::string battleID = resolveLiveBattleID(
		gameHandler,
		recordedBattleID);
	const auto * initialBattle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
	if(!initialBattle)
		throw std::runtime_error("VGT battle block references missing battle: " + battleID);
	const auto randomizerParticipants = battleRandomizerParticipants(*initialBattle);
	const auto randomizerHeroes = battleRandomizerHeroes(*initialBattle);
	std::map<std::string, int> roster;
	if(const auto * units = findField(node, "units"))
	{
		if(!units->isStruct())
			throw std::runtime_error("VGT battle units field is not a mapping");
		for(const auto & [name, value] : units->Struct())
			roster[name] = static_cast<int>(requireInteger(value, "stack"));
	}
	gameHandler.randomizer->loadVGTBattleJson(
		requireField(node, "randomBefore"), randomizerParticipants, randomizerHeroes);
	const JsonNode & events = requireField(node, "events");
	if(!events.isVector())
		throw std::runtime_error("VGT battle block events field is not a list");
	const auto & outcome = requireField(node, "outcome");
	bool tacticalFinishedEarly = false;
	std::string currentEvent;
	std::function<void(const JsonNode &)> replayEvents;
	replayEvents = [&](const JsonNode & eventList)
	{
		if(!eventList.isVector())
			throw std::runtime_error("VGT battle round events field is not a list");
		bool replayedCatapultAction = false;
		for(const JsonNode & record : eventList.Vector())
		{
			currentEvent = record.toCompactString();
			if(tacticalFinishedEarly)
				return;
			const auto * liveBattle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
			if(!liveBattle || liveBattle->battleIsFinished())
			{
				// Damage rolls and automatic effects may finish a reconstructed battle
				// before the readable tactical scene does. Its explicit frozen outcome
				// remains authoritative for strategic replay.
				tacticalFinishedEarly = true;
				return;
			}
			if(!record.isStruct())
				throw std::runtime_error("VGT battle block event is not a mapping");
			if(hasField(record, "round") && hasField(record, "events"))
			{
				const int64_t recordedRound = requireInteger(record, "round");
				auto * battle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID)));
				while(battle->getRound() < recordedRound)
				{
					// Active-stack corrections can intentionally bypass the engine's
					// reconstructed initiative queue. Keep the transcript's explicit
					// round boundary authoritative so durations and per-round state expire
					// at the same readable point.
					BattleNextRound nextRound;
					nextRound.battleID = BattleID(std::stoi(battleID));
					gameHandler.sendAndApply(nextRound);
				}
				if(battle->getRound() != recordedRound)
					throw std::runtime_error("VGT battle replay advanced beyond the recorded round");
				replayEvents(requireField(record, "events"));
				continue;
			}
			if(record.Struct().size() == 1 &&
				(record.Struct().begin()->first == "wait" || record.Struct().begin()->first == "defend"))
			{
				const std::string actionKind = record.Struct().begin()->first;
				std::vector<std::string> units;
				const auto & actions = record.Struct().begin()->second;
				if(actions.isString())
					units.push_back(actions.String());
				else if(actions.isVector())
				{
					for(const auto & unit : actions.Vector())
						units.push_back(unit.String());
				}
				else
					throw std::runtime_error("VGT battle action batch must be a unit or list of units");
				for(const auto & unit : units)
				{
					JsonNode action;
					action["unit"].String() = unit;
					replayReadableBattleAction(gameHandler, actionKind, action, battleID, roster);
				}
				continue;
			}
			if(record.Struct().size() == 1 && record.Struct().begin()->first == "move")
			{
				replaySemanticBattleMove(gameHandler, record.Struct().begin()->second, battleID, roster);
				continue;
			}
			if(record.Struct().size() == 1 && record.Struct().begin()->first == "attack" &&
				hasField(record.Struct().begin()->second, "from"))
			{
				replaySemanticBattleAttack(
					gameHandler,
					record.Struct().begin()->second,
					battleID,
					roster);
				continue;
			}
			if(record.Struct().size() == 1 && record.Struct().begin()->first == "cast")
			{
				replaySemanticBattleCast(gameHandler, record.Struct().begin()->second, battleID, roster);
				continue;
			}
			if(record.Struct().size() == 1 && record.Struct().begin()->first == "heal")
			{
				replaySemanticBattleHealing(
					gameHandler,
					record.Struct().begin()->second,
					battleID,
					roster);
				continue;
			}
			if(replayRawBattleHealing(gameHandler, record, battleID, roster))
				continue;
			// A manual catapult decision names its acting unit and regenerates its
			// adjacent damage records. Effect-only records come from omitted automatic
			// catapult turns and must change the frozen wall state explicitly.
			if(record.Struct().size() == 1 && record.Struct().begin()->first == "catapult" &&
				hasField(record.Struct().begin()->second, "unit"))
			{
				replayReadableBattleAction(
					gameHandler,
					"catapult",
					record.Struct().begin()->second,
					battleID,
					roster);
				replayedCatapultAction = true;
				continue;
			}
			if(record.Struct().size() == 1 && record.Struct().begin()->first == "catapult")
			{
				if(!replayedCatapultAction)
					replaySemanticCatapultEffect(
						gameHandler, record.Struct().begin()->second, battleID, roster);
				continue;
			}
			replayedCatapultAction = false;
			if(hasField(record, "event") || hasField(record, "attack"))
				continue;
			static const std::set<std::string> semanticEffects = {
				"casts", "catapult", "cloned", "drainMana", "enchant", "enchanterCounter", "fear", "gate",
				"hasClone", "morale", "poison", "regenerate", "triggeredEffect", "unbind"
			};
			if(record.Struct().size() == 1 && semanticEffects.contains(record.Struct().begin()->first))
				continue;
			if(record.Struct().size() == 1 && isBattleActionKind(record.Struct().begin()->first))
			{
				const auto & entry = *record.Struct().begin();
				replayReadableBattleAction(gameHandler, entry.first, entry.second, battleID, roster);
				continue;
			}
			if(record.Struct().size() == 1 && isEffectKind(record.Struct().begin()->first))
				continue;
			throw std::runtime_error("Unsupported VGT battle block event");
		}
	};
	if(fastForwardBattles)
		fastForwardBattle(
			gameHandler, battleID, roster, outcome, randomizerParticipants, randomizerHeroes);
	else
	{
		try
		{
			replayEvents(events);
		}
		catch(const std::exception & error)
		{
			throw std::runtime_error(
				"tactical event " + currentEvent + ": " + std::string(error.what()));
		}

		if(const auto * battle = gameHandler.gs->getBattle(BattleID(std::stoi(battleID))))
		{
			// The readable attacks preserve their observed damage, while normal action
			// replay lets the engine roll damage again. If those rolls do not finish
			// result processing at the recorded point, apply the explicit frozen
			// outcome. Direct health corrections can kill the last stack without
			// invoking the normal action processor that starts the result.
			if(!gameHandler.battles->battleIsEnding(*battle))
				fastForwardBattle(
					gameHandler, battleID, roster, outcome, randomizerParticipants, randomizerHeroes);
		}
	}

	const BattleID liveBattleID(std::stoi(battleID));
	if(gameHandler.gs->getBattle(liveBattleID))
	{
		try
		{
			// Tactical reconstruction may consume a different number of combat-only
			// random draws while still reaching the frozen outcome. Strategic aftermath
			// (notably level-up offers) starts at this recorded boundary.
			gameHandler.randomizer->loadVGTBattleJson(
				requireField(outcome, "randomBeforeAftermath"), randomizerParticipants, randomizerHeroes);
			gameHandler.battles->endBattleConfirm(liveBattleID);
		}
		catch(const std::exception & error)
		{
			throw std::runtime_error("battle confirmation: " + std::string(error.what()));
		}
	}
	if(gameHandler.gs->getBattle(liveBattleID))
		throw std::runtime_error("VGT battle outcome did not finalize battle: " + battleID);
	gameHandler.randomizer->loadVGTBattleJson(
		requireField(outcome, "continuation"), randomizerParticipants, randomizerHeroes);

	requireString(outcome, "result");
	requireString(outcome, "winnerSide");
	requireString(outcome, "winner");
	requireString(outcome, "loser");
	if(!requireField(outcome, "casualties").isStruct())
		throw std::runtime_error("VGT battle outcome casualties is not a mapping");
	if(const auto * survivors = findField(outcome, "survivors"); survivors && !survivors->isStruct())
		throw std::runtime_error("VGT battle outcome survivors is not a mapping");
	if(const auto * continuation = findField(outcome, "continuation"); continuation && !continuation->isStruct())
		throw std::runtime_error("VGT battle outcome continuation is not a mapping");
	if(const auto * beforeAftermath = findField(outcome, "randomBeforeAftermath");
		beforeAftermath && !beforeAftermath->isStruct())
		throw std::runtime_error("VGT battle outcome randomBeforeAftermath is not a mapping");
	const auto & armies = requireField(outcome, "armies");
	if(!armies.isStruct())
		throw std::runtime_error("VGT battle outcome armies is not a mapping");
	for(const auto & [armyName, state] : armies.Struct())
	{
		// A cleared creature bank can be removed from the map as battle cleanup
		// finishes. Its explicit empty army is still useful in the transcript, but
		// there is no surviving state object to update during replay.
		if(state.isVector() && state.Vector().empty())
			continue;
		try
		{
			applyArmyState(
				gameHandler,
				resolveObjectAlias(*gameHandler.gs, armyName),
				state);
		}
		catch(const std::exception & error)
		{
			throw std::runtime_error("final army " + armyName + ": " + error.what());
		}
	}
	const auto * aftermath = findField(outcome, "aftermath");
	if(!aftermath)
		return;
	if(!aftermath->isVector())
		throw std::runtime_error("VGT battle aftermath is not a list");

	std::optional<std::string> aftermathActor;
	if(const auto * winner = findField(outcome, "winner"); winner && winner->isString())
		aftermathActor = winner->String();
	for(const auto & record : aftermath->Vector())
	{
		if(!record.isStruct() || record.Struct().size() != 1)
			throw std::runtime_error("VGT battle aftermath record is not a one-key mapping");
		const auto & entry = *record.Struct().begin();
		if(entry.first == "query")
		{
			if(const auto * player = findField(entry.second, "player"); player && player->isString())
				aftermathActor = player->String();
			continue;
		}
		if(entry.first == "levelUp")
		{
			applyRecordedHeroLevelUp(gameHandler, entry.second);
			continue;
		}
		// Battle resolution already produced all state effects. Only replay choices
		// that drive a surviving post-battle query (for example guarded dwelling
		// recruitment); observational capture/reward records remain derived.
		if(isDecisionKind(entry.first) && !isEffectKind(entry.first))
			replayReadableDecision(gameHandler, entry.first, entry.second, std::nullopt, aftermathActor);
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

void replayTranscriptDocuments(CGameHandler & gameHandler, const JsonNode & documents, bool fastForwardBattles)
{
	if(documents.Vector().size() == 1)
		return;

	for(size_t documentIndex = 1; documentIndex < documents.Vector().size(); ++documentIndex)
	{
		const JsonNode & document = documents.Vector()[documentIndex];
		std::optional<std::string> defaultActor;
		if(const auto * turn = findField(document, "turn"))
			defaultActor = requireString(*turn, "player");
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
			if(defaultActor)
			{
				const auto actor = decodePlayerColor(*defaultActor);
				const auto topQuery = gameHandler.queries->topQuery(actor);
				const std::string kind = record.isString()
					? record.String()
					: record.isStruct() && !record.Struct().empty() ? record.Struct().begin()->first : "invalid";
				logGlobal->trace(
					"VGT replay document %d record %d kind %s, query before: %s",
					static_cast<int>(documentIndex), static_cast<int>(recordIndex), kind,
					topQuery ? topQuery->toString() : "none");
			}
			if(record.isString())
			{
				if(!isDecisionKind(record.String()))
					throw std::runtime_error("Unsupported scalar VGT record kind: " + record.String());
				requireNoPendingDiscoveryCheck("the next decision");
				JsonNode action;
				replayReadableDecision(gameHandler, record.String(), action, std::nullopt, defaultActor);
				continue;
			}
			if(!record.isStruct())
				throw std::runtime_error("VGT replay record is not a mapping or action name");
			if(hasField(record, "with") && hasField(record, "actions"))
			{
				const std::string hero = requireString(record, "with");
				const auto & sceneActions = requireField(record, "actions");
				if(!sceneActions.isVector())
					throw std::runtime_error("VGT hero scene actions must be a list");
				for(const auto & sceneAction : sceneActions.Vector())
				{
					if(sceneAction.isString())
					{
						requireNoPendingDiscoveryCheck("the next hero-scene action");
						JsonNode action;
						action["hero"].String() = hero;
						replayReadableDecision(gameHandler, sceneAction.String(), action, std::nullopt, defaultActor);
						continue;
					}
					if(!sceneAction.isStruct() || sceneAction.Struct().size() != 1)
						throw std::runtime_error("VGT hero scene action must have one action kind");
					const auto & entry = *sceneAction.Struct().begin();
					if(entry.first == "discovers")
					{
						const std::string actor = hasField(entry.second, "actor")
							? requireString(entry.second, "actor")
							: defaultActor.value_or("");
						if(actor.empty())
							throw std::runtime_error("VGT discovery record has no acting player");
						const PlayerColor player = playerFromActor(actor);
						ScopedAliasDefaultPlayer aliasScope(player);
						verifyDiscoveryCheck(entry.second, "objects", player, "displacement");
						continue;
					}
					if(entry.first == "levelUp")
					{
						const PlayerColor player = playerFromActor(defaultActor.value_or(""));
						ScopedAliasDefaultPlayer aliasScope(player);
						JsonNode levelUp = entry.second;
						levelUp["hero"].String() = hero;
						applyRecordedHeroLevelUp(gameHandler, levelUp);
						continue;
					}
					if(isDecisionKind(entry.first))
					{
						requireNoPendingDiscoveryCheck("the next hero-scene decision");
						JsonNode action = entry.second;
						if(!action.isStruct())
							throw std::runtime_error("VGT hero scene decision must be a mapping");
						action["hero"].String() = hero;
						replayReadableDecision(gameHandler, entry.first, action, std::nullopt, defaultActor);
						continue;
					}
					if(!isEffectKind(entry.first))
						throw std::runtime_error("Unsupported VGT hero scene record kind: " + entry.first);
				}
				continue;
			}
			if(record.Struct().size() != 1)
				throw std::runtime_error("VGT replay record is not a one-key mapping");

			const auto & entry = *record.Struct().begin();
			if(entry.first == "discovers")
			{
				const std::string actor = hasField(entry.second, "actor")
					? requireString(entry.second, "actor")
					: defaultActor.value_or("");
				if(actor.empty())
					throw std::runtime_error("VGT discovery record has no acting player");
				const PlayerColor player = playerFromActor(actor);
				ScopedAliasDefaultPlayer aliasScope(player);
				verifyDiscoveryCheck(entry.second, "objects", player, "displacement");
				continue;
			}
			if(entry.first == "levelUp" && defaultActor)
			{
				const PlayerColor player = playerFromActor(defaultActor.value_or(""));
				ScopedAliasDefaultPlayer aliasScope(player);
				applyRecordedHeroLevelUp(gameHandler, entry.second);
				continue;
			}
			// A world document has no acting player. Some semantic effect names (notably
			// visit and capture) are also valid decision names inside a turn; in world
			// scope they are observational effects and must not be replayed as actions.
			if(!defaultActor && isEffectKind(entry.first))
				continue;
			if(isDecisionKind(entry.first))
			{
				requireNoPendingDiscoveryCheck("the next decision");
				replayReadableDecision(gameHandler, entry.first, entry.second, std::nullopt, defaultActor);
				continue;
			}
			if(entry.first == "battle")
			{
				requireNoPendingDiscoveryCheck("the next battle");
				try
				{
					applyBattleBlock(gameHandler, entry.second, fastForwardBattles);
				}
				catch(const std::exception & error)
				{
					throw std::runtime_error(
						"VGT battle " + battleAliasText(requireField(entry.second, "id")) +
						" replay failed: " + error.what());
				}
				continue;
			}
			if(entry.first == "localState")
			{
				requireNoPendingDiscoveryCheck("the next local state record");
				applyLocalState(gameHandler, entry.second);
				continue;
			}
			if(entry.first == "unmodelled")
				throw std::runtime_error("VGT replay encountered unmodelled material pack: " + requireString(entry.second, "pack"));
			if(!isEffectKind(entry.first))
				throw std::runtime_error("Unsupported VGT record kind: " + entry.first);
		}
		requireNoPendingDiscoveryCheck("the end of document " + std::to_string(documentIndex));
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
			<< " owner=" << colorAlias(hero->tempOwner)
			<< " pos=" << hero->visitablePos().toString()
			<< " experience=" << hero->exp
			<< " level=" << hero->level
			<< " mana=" << hero->mana
			<< " movement=" << hero->movementPointsRemaining()
			<< " moveDir=" << static_cast<int>(hero->moveDir)
			<< " tactics=" << hero->tacticFormationEnabled
			<< " secondarySkills=";
		for(const auto & [skill, level] : hero->secSkills)
			output << SecondarySkill::encode(skill.getNum()) << ":" << static_cast<int>(level) << ",";
		output << " spells=";
		for(const auto & spell : hero->getSpellsInSpellbook())
			output << SpellID::encode(spell.getNum()) << ",";
		output << " visitedObjects=";
		for(const auto & object : hero->visitedObjects)
			output << object.getNum() << ",";
		output
			<< " artifacts=" << artifactSetSummary(*hero)
			<< " army=" << armySummary(*hero)
			<< "\n";
	}

	for(const auto * town : gameState.getMap().getObjects<CGTownInstance>())
	{
		output << "town id=" << town->id.getNum()
			<< " name=" << town->getNameTranslated()
			<< " owner=" << colorAlias(town->tempOwner)
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
		output << " creatures=";
		for(size_t index = 0; index < town->creatures.size(); ++index)
		{
			if(index)
				output << ";";
			output << index << ":" << town->creatures[index].first << ":";
			for(size_t creatureIndex = 0; creatureIndex < town->creatures[index].second.size(); ++creatureIndex)
			{
				if(creatureIndex)
					output << ",";
				output << CreatureID::encode(town->creatures[index].second[creatureIndex].getNum());
			}
		}
		output << "\n";
	}

	for(const auto * creature : gameState.getMap().getObjects<CGCreature>())
	{
		output << "creature id=" << creature->id.getNum()
			<< " name=" << creature->instanceName
			<< " type=" << CreatureID::encode(creature->getCreatureID().getNum())
			<< " owner=" << colorAlias(creature->tempOwner)
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
			<< " owner=" << colorAlias(object->tempOwner)
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
				if(bonusNode)
				{
					size_t bonusIndex = 0;
					for(const auto & bonus : bonusNode->getExportedBonusList())
					{
						output << "objectBonus id=" << object->id.getNum()
							<< " index=" << bonusIndex++
							<< " type=" << static_cast<int>(bonus->type)
							<< " subtype=" << bonus->subtype.toString()
							<< " sourceType=" << static_cast<int>(bonus->source)
							<< " source=" << bonus->sid.toString()
							<< " value=" << bonus->val
							<< " description=" << bonus->description.toString()
							<< "\n";
					}
				}
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
	output << "handler.randomizer.json=" << gameHandler.randomizer->toVGTJson().toCompactString() << "\n";
	output << "handler.battles=" << serializedValueFingerprint(*gameHandler.battles) << "\n";
	output << "handler.heroPool=" << serializedValueFingerprint(*gameHandler.heroPool) << "\n";
	output << "handler.playerMessages=" << serializedValueFingerprint(*gameHandler.playerMessages) << "\n";
	output << "handler.turnOrder=" << serializedValueFingerprint(*gameHandler.turnOrder) << "\n";
	for(PlayerColor player(0); player < PlayerColor::PLAYER_LIMIT; ++player)
	{
		output << "handler.turnOrder.player." << player.toString()
			<< " awaiting=" << (gameHandler.turnOrder->isPlayerWaitingForTurn(player) ? "true" : "false")
			<< " acting=" << (gameHandler.turnOrder->isPlayerMakingTurn(player) ? "true" : "false")
			<< " acted=" << (gameHandler.turnOrder->hasPlayerActedThisDay(player) ? "true" : "false")
			<< "\n";
	}
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
	objectAliasCache.clear();
	replayDiscoveryTracker = VGTDiscoveryTracker();
	replayDerivedDiscoveries.clear();
	const JsonNode documents = readJsonFile(options.inputJson);
	if(!documents.isVector() || documents.Vector().empty())
		throw std::runtime_error("VGT replay JSON must contain transcript documents");

	const JsonNode & header = documents.Vector().front();
	if(requireInteger(header, "vgt") != 4)
		throw std::runtime_error("VGT replay only supports format version 4");
	if(requireString(header, "format") != "VCMI readable event transcript")
		throw std::runtime_error("VGT replay header has an unsupported format name");
	setReplaySeed(header);
	StartInfo startInfo = decodeStartInfo(header);

	ReplayGameServer replayServer;
	replayServer.configureTurnStates(options.expectedTurnStateDirectory, options.outputTurnStateDirectory);
	replayServer.configureBattleOutcomeCapture(options.capturedBattleOutcomes);
	CGameHandler gameHandler(replayServer);
	replayServer.attach(gameHandler);

	Load::ProgressAccumulator progress;
	gameHandler.init(&startInfo, progress);
	applyMapEngineState(gameHandler, header);
	applyInitialState(gameHandler, header);
	applyGameSettingsOverrides(gameHandler, header);
	if(documents.Vector().size() > 1)
		gameHandler.start(false);
	replayTranscriptDocuments(gameHandler, documents, options.fastForwardBattles);
	replayServer.verifyTurnStatesComplete();
	replayServer.writeCapturedBattleOutcomes();
	if(!options.outputSave.empty())
		gameHandler.saveToFile(options.outputSave);
	writeGameStateSave(gameHandler, options.outputGameStateSave);
	return 0;
}
