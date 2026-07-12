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
#include "../lib/networkPacks/PacksForClient.h"

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

	bool isPlayerHost(const PlayerColor &) const override
	{
		return false;
	}

	bool hasPlayerAt(PlayerColor, GameConnectionID) const override
	{
		return false;
	}

	bool hasBothPlayersAtSameConnection(PlayerColor, PlayerColor) const override
	{
		return false;
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
}

int replayVGTJson(const VGTReplayOptions & options)
{
	const JsonNode documents = readJsonFile(options.inputJson);
	if(!documents.isVector() || documents.Vector().empty())
		throw std::runtime_error("VGT replay JSON must contain transcript documents");
	if(documents.Vector().size() != 1)
		throw std::runtime_error("VGT event replay is not implemented yet; pass only the header document for initialization replay");

	const JsonNode & header = documents.Vector().front();
	setReplaySeed(header);
	StartInfo startInfo = decodeStartInfo(header);

	ReplayGameServer replayServer;
	CGameHandler gameHandler(replayServer);
	replayServer.attach(gameHandler);

	Load::ProgressAccumulator progress;
	gameHandler.init(&startInfo, progress);
	gameHandler.saveToFile(options.outputSave);
	return 0;
}
