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
#include "../lib/json/JsonNode.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/networkPacks/PacksForClient.h"
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

CreatureID decodeCreature(const std::string & value)
{
	if(value == "core:none")
		return CreatureID::NONE;
	return CreatureID(CreatureID::decode(value));
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

void replayDecision(CGameHandler & gameHandler, const JsonNode & decision)
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

void replayTranscriptDocuments(CGameHandler & gameHandler, const JsonNode & documents)
{
	if(documents.Vector().size() == 1)
		return;

	gameHandler.start(false);
	size_t decisionIndex = 0;
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
			if(entry.first == "decision")
			{
				++decisionIndex;
				logGlobal->info("VGT replay decision %d: %s", static_cast<int>(decisionIndex), requireString(entry.second, "kind"));
				replayDecision(gameHandler, entry.second);
			}
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
