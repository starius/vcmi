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

#include "McpHttpServer.h"

#include "../../lib/CPlayerState.h"
#include "../../lib/CStack.h"
#include "../../lib/ResourceSet.h"
#include "../../lib/battle/BattleAction.h"
#include "../../lib/battle/CPlayerBattleCallback.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/logging/CLogger.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/networkPacks/Component.h"
#include "../../lib/texts/MetaString.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <shared_mutex>

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

JsonNode jsonPosition(const int3 & position)
{
	JsonNode node;
	node["x"] = JsonNode(position.x);
	node["y"] = JsonNode(position.y);
	node["z"] = JsonNode(position.z);
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
		stack["name"] = JsonNode(slot.second->getName());
		node.Vector().push_back(stack);
	}
	return node;
}

JsonNode jsonHero(const CGHeroInstance * hero)
{
	JsonNode node;
	node["id"] = JsonNode(hero->id.getNum());
	node["name"] = JsonNode(hero->getNameTranslated());
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
	node["name"] = JsonNode(town->getNameTranslated());
	node["position"] = jsonPosition(town->visitablePos());
	node["owner"] = JsonNode(town->tempOwner.toString());
	node["fortLevel"] = JsonNode(static_cast<int32_t>(town->fortLevel()));
	node["visitingHeroId"] = jsonObjectId(town->getVisitingHero());
	node["garrisonHeroId"] = jsonObjectId(town->getGarrisonHero());
	node["army"] = jsonArmy(*town);

	node["buildings"].Vector();
	for(const BuildingID & building : town->getBuildings())
		node["buildings"].Vector().push_back(JsonNode(building.getNum()));

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

}

CMcpPlayerInterface::CMcpPlayerInterface() = default;

void CMcpPlayerInterface::initGameInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CCallback> CB)
{
	env = ENV;
	cb = CB;
	human = false;
	dllName = "McpAI";
	playerID = cb->getPlayerID().value_or(PlayerColor::CANNOT_DETERMINE);

	configureProtocol();
	startHttpServer();
}

CMcpPlayerInterface::~CMcpPlayerInterface() = default;

void CMcpPlayerInterface::finish()
{
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

	protocol.registerTool({
		"vcmi.get_state",
		"Returns current visible player state as JSON.",
		makeObjectSchema({}, {}),
		[this](const JsonNode &)
		{
			return makeJsonResult(makeStateJson());
		}
	});

	protocol.registerTool({
		"vcmi.end_turn",
		"Ends the current adventure-map turn.",
		makeObjectSchema({}, {}),
		[this](const JsonNode &)
		{
			{
				std::lock_guard lock(interfaceMutex);
				if(!turnActive)
					return makeToolError("No active turn to end");
				turnActive = false;
			}
			cb->endTurn();
			return makeOkResult("turn ended");
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
			return makeJsonResult(result);
		}
	});

	protocol.registerTool({
		"vcmi.move_hero",
		"Moves an owned hero to an adventure-map tile.",
		makeObjectSchema({{"hero_id", "integer"}, {"x", "integer"}, {"y", "integer"}, {"z", "integer"}, {"transit", "boolean"}}, {"hero_id", "x", "y"}),
		[this](const JsonNode & arguments)
		{
			const CGHeroInstance * hero = cb->getHero(ObjectInstanceID(readInteger(arguments, "hero_id")));
			if(!hero)
				return makeToolError("Unknown hero id");
			if(hero->tempOwner != playerID)
				return makeToolError("Hero is not owned by this MCP player");

			int z = hasField(arguments, "z") ? readInteger(arguments, "z") : hero->visitablePos().z;
			bool transit = readBool(arguments, "transit", false);
			cb->moveHero(hero, int3(readInteger(arguments, "x"), readInteger(arguments, "y"), z), transit);
			return makeOkResult("move requested");
		}
	});

	protocol.registerTool({
		"vcmi.build_town_building",
		"Requests construction of a building in an owned town.",
		makeObjectSchema({{"town_id", "integer"}, {"building_id", "integer"}}, {"town_id", "building_id"}),
		[this](const JsonNode & arguments)
		{
			const CGTownInstance * town = cb->getTown(ObjectInstanceID(readInteger(arguments, "town_id")));
			if(!town)
				return makeToolError("Unknown town id");
			if(town->tempOwner != playerID)
				return makeToolError("Town is not owned by this MCP player");

			bool accepted = cb->buildBuilding(town, BuildingID(readInteger(arguments, "building_id")));
			return accepted ? makeOkResult("build requested") : makeToolError("Build request was rejected by the client callback");
		}
	});

	protocol.registerTool({
		"vcmi.battle_defend",
		"Defends with the currently active battle stack.",
		makeObjectSchema({}, {}),
		[this](const JsonNode &)
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
				return makeToolError("No active battle stack");

			cb->battleMakeUnitAction(battleID, BattleAction::makeDefend(stack));
			return makeOkResult("battle defend requested");
		}
	});

	protocol.registerTool({
		"vcmi.battle_wait",
		"Waits with the currently active battle stack.",
		makeObjectSchema({}, {}),
		[this](const JsonNode &)
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
				return makeToolError("No active battle stack");

			cb->battleMakeUnitAction(battleID, BattleAction::makeWait(stack));
			return makeOkResult("battle wait requested");
		}
	});

	protocol.registerTool({
		"vcmi.battle_end_tactics",
		"Ends the current tactics phase.",
		makeObjectSchema({}, {}),
		[this](const JsonNode &)
		{
			BattleID battleID = BattleID::NONE;
			{
				std::lock_guard lock(interfaceMutex);
				battleID = activeTacticsBattleID;
				activeTacticsBattleID = BattleID::NONE;
			}

			if(battleID == BattleID::NONE)
				return makeToolError("No active tactics phase");

			cb->battleMakeTacticAction(battleID, BattleAction::makeEndOFTacticPhase(cb->getBattle(battleID)->battleGetTacticsSide()));
			return makeOkResult("tactics phase ended");
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
			return;
		}
		catch(const std::exception & exception)
		{
			httpServer.reset();
			logGlobal->warn("Failed to start MCP HTTP server for player %s on port %d: %s", playerID.toString(), portToTry, exception.what());
		}
	}
}

JsonNode CMcpPlayerInterface::makeStateJson() const
{
	JsonNode state;

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
			state["battle"]["stack"]["name"] = JsonNode(activeStackToMove->getName());
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

	return state;
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

void CMcpPlayerInterface::setPendingQuery(QueryID queryID, const std::string & type, JsonNode data)
{
	std::lock_guard lock(interfaceMutex);
	pendingQuery = PendingQuery{queryID, type, std::move(data)};
}

void CMcpPlayerInterface::clearPendingQuery(QueryID queryID)
{
	std::lock_guard lock(interfaceMutex);
	if(pendingQuery && pendingQuery->id == queryID)
		pendingQuery.reset();
}

void CMcpPlayerInterface::yourTurn(QueryID queryID)
{
	{
		std::lock_guard lock(interfaceMutex);
		turnActive = true;
	}

	if(queryID.hasValue())
		cb->selectionMade(0, queryID);
}

void CMcpPlayerInterface::yourTacticPhase(const BattleID & battleID, int distance)
{
	std::lock_guard lock(interfaceMutex);
	activeTacticsBattleID = battleID;
}

void CMcpPlayerInterface::activeStack(const BattleID & battleID, const CStack * stack)
{
	std::lock_guard lock(interfaceMutex);
	activeBattleID = battleID;
	activeStackToMove = stack;
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
	data["text"] = JsonNode(text);
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
	data["title"] = JsonNode(customTitle.toString());
	setPendingQuery(queryID, "garrison_dialog", data);
}

void CMcpPlayerInterface::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	JsonNode data;
	data["title"] = JsonNode(title.toString());
	data["description"] = JsonNode(description.toString());
	data["objects"].Vector();
	for(const ObjectInstanceID & object : objects)
		data["objects"].Vector().push_back(JsonNode(object.getNum()));
	setPendingQuery(askID, "map_object_select_dialog", data);
}

std::optional<BattleAction> CMcpPlayerInterface::makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState)
{
	return std::nullopt;
}
