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
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CTown.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/logging/CLogger.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/networkPacks/Component.h"
#include "../../lib/texts/MetaString.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <shared_mutex>
#include <sstream>

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

JsonNode jsonBuildOption(const CGTownInstance * town, const CBuilding * building)
{
	JsonNode node;
	node["town_id"] = JsonNode(town->id.getNum());
	node["town"] = JsonNode(town->getNameTranslated());
	node["building_id"] = JsonNode(building->bid.getNum());
	node["building"] = JsonNode(building->getNameTranslated());
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
		"Returns current visible player state as JSON.",
		makeObjectSchema({}, {}),
		[this](const JsonNode & arguments)
		{
			return traceToolResult("vcmi.get_state", arguments, makeJsonResult(makeStateJson()));
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
			cb->endTurn();
			return traceToolResult("vcmi.end_turn", arguments, makeOkResult("turn ended"));
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
		"Moves an owned hero to an adventure-map tile.",
		makeObjectSchema({{"hero_id", "integer"}, {"x", "integer"}, {"y", "integer"}, {"z", "integer"}, {"transit", "boolean"}}, {"hero_id", "x", "y"}),
		[this](const JsonNode & arguments)
		{
			const CGHeroInstance * hero = cb->getHero(ObjectInstanceID(readInteger(arguments, "hero_id")));
			if(!hero)
				return traceToolResult("vcmi.move_hero", arguments, makeToolError("Unknown hero id"));
			if(hero->tempOwner != playerID)
				return traceToolResult("vcmi.move_hero", arguments, makeToolError("Hero is not owned by this MCP player"));

			int z = hasField(arguments, "z") ? readInteger(arguments, "z") : hero->visitablePos().z;
			bool transit = readBool(arguments, "transit", false);
			cb->moveHero(hero, int3(readInteger(arguments, "x"), readInteger(arguments, "y"), z), transit);
			return traceToolResult("vcmi.move_hero", arguments, makeOkResult("move requested"));
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
				return traceToolResult("vcmi.build_town_building", arguments, makeToolError("Unknown town id"));
			if(town->tempOwner != playerID)
				return traceToolResult("vcmi.build_town_building", arguments, makeToolError("Town is not owned by this MCP player"));

			bool accepted = cb->buildBuilding(town, BuildingID(readInteger(arguments, "building_id")));
			return traceToolResult("vcmi.build_town_building", arguments, accepted ? makeOkResult("build requested") : makeToolError("Build request was rejected by the client callback"));
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

JsonNode CMcpPlayerInterface::makeActionSpaceJson() const
{
	JsonNode actionSpace;
	actionSpace["purpose"] = JsonNode("Use this payload to choose one legal VCMI MCP tool call for the current player.");
	actionSpace["rules"].Vector().push_back(JsonNode("Call vcmi.get_state or vcmi.get_action_space before choosing an action."));
	actionSpace["rules"].Vector().push_back(JsonNode("Use only object ids returned by vcmi.get_state or vcmi.get_action_space."));
	actionSpace["rules"].Vector().push_back(JsonNode("If a tool returns isError=true, inspect vcmi.get_state again before retrying."));
	actionSpace["rules"].Vector().push_back(JsonNode("If unsure and turn.active is true, vcmi.end_turn is a valid conservative action."));

	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_state", "Read current visible player state."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.get_action_space", "Read current guidance, object ids, and action candidates."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.end_turn", "End the active adventure-map turn."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.answer_query", "Answer the pending query/dialog using query_id and optional answer."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.move_hero", "Move an owned hero to x, y, optional z."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.build_town_building", "Build a currently allowed building in an owned town."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_defend", "Defend with the active battle stack."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_wait", "Wait with the active battle stack."));
	actionSpace["allTools"].Vector().push_back(jsonAction("vcmi.battle_end_tactics", "End the current tactics phase."));

	const JsonNode state = makeStateJson();
	actionSpace["stateSummary"]["turnActive"] = state["turn"]["active"];
	actionSpace["stateSummary"]["pendingQuery"] = state["pendingQuery"];
	actionSpace["stateSummary"]["battle"] = state["battle"];

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
		JsonNode defend = jsonAction("vcmi.battle_defend", "Defend with the active battle stack.");
		defend["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(defend);

		JsonNode wait = jsonAction("vcmi.battle_wait", "Wait with the active battle stack.");
		wait["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(wait);
	}

	if(state["battle"]["activeTactics"].Bool())
	{
		JsonNode action = jsonAction("vcmi.battle_end_tactics", "End the current tactics phase.");
		action["arguments"].Struct();
		actionSpace["currentActions"].Vector().push_back(action);
	}

	for(const JsonNode & hero : state["heroes"].Vector())
	{
		JsonNode action = jsonAction("vcmi.move_hero", "Move this owned hero. Destination must be reachable and visible to the player.");
		action["arguments"]["hero_id"] = hero["id"];
		action["arguments"]["x"] = hero["position"]["x"];
		action["arguments"]["y"] = hero["position"]["y"];
		action["arguments"]["z"] = hero["position"]["z"];
		action["notes"].Vector().push_back(JsonNode("Replace x/y/z with the intended destination. The server validates the move."));
		actionSpace["currentActions"].Vector().push_back(action);
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

std::string CMcpPlayerInterface::makeAgentGuideText() const
{
	return
		"VCMI MCP agent guide\n"
		"1. Call vcmi.get_action_space first. It lists the current tools, ids, and action candidates.\n"
		"2. Call vcmi.get_state when you need full current player state.\n"
		"3. Use only ids and coordinates from MCP state/action-space payloads.\n"
		"4. Prefer concrete progress: build an allowed town building, move an owned hero, answer a pending query, or handle battle/tactics.\n"
		"5. If no useful action is clear and turn.active is true, call vcmi.end_turn.\n"
		"6. If a tool result has isError=true, read state/action-space again before retrying.\n";
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

	if(queryID.hasValue())
		cb->selectionMade(0, queryID);
}

void CMcpPlayerInterface::yourTacticPhase(const BattleID & battleID, int distance)
{
	std::lock_guard lock(interfaceMutex);
	activeTacticsBattleID = battleID;

	JsonNode event;
	event["event"] = JsonNode("tactics_phase_started");
	event["player"] = JsonNode(playerID.toString());
	event["battle_id"] = JsonNode(battleID.getNum());
	event["distance"] = JsonNode(distance);
	appendTraceEvent(event);
}

void CMcpPlayerInterface::activeStack(const BattleID & battleID, const CStack * stack)
{
	std::lock_guard lock(interfaceMutex);
	activeBattleID = battleID;
	activeStackToMove = stack;

	JsonNode event;
	event["event"] = JsonNode("battle_stack_active");
	event["player"] = JsonNode(playerID.toString());
	event["battle_id"] = JsonNode(battleID.getNum());
	if(stack)
	{
		event["stack"]["id"] = JsonNode(static_cast<int32_t>(stack->unitId()));
		event["stack"]["name"] = JsonNode(stack->getName());
	}
	appendTraceEvent(event);
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
