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
#include "../../lib/CPlayerState.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/mapObjects/CGDwelling.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/army/CArmedInstance.h"
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../luascript/LuaAdventureScriptRunner.h"

namespace ScriptedAdventureAI
{
namespace
{

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

int32_t readInteger(const JsonNode & node, const std::string & field)
{
	if(!node[field].isNumber() || node[field].getType() != JsonNode::JsonType::DATA_INTEGER)
		throw std::invalid_argument("Missing or non-integer script action field: " + field);
	return static_cast<int32_t>(node[field].Integer());
}

std::string readString(const JsonNode & node, const std::string & field)
{
	if(!node[field].isString())
		throw std::invalid_argument("Missing or non-string script action field: " + field);
	return node[field].String();
}

JsonNode jsonPosition(const int3 & position)
{
	JsonNode node;
	node["x"] = JsonNode(position.x);
	node["y"] = JsonNode(position.y);
	node["z"] = JsonNode(position.z);
	return node;
}

}

CScriptedAdventureAI::CScriptedAdventureAI()
	: scriptPath("ai/defaultAdventure.lua")
{
}

CScriptedAdventureAI::~CScriptedAdventureAI() = default;

void CScriptedAdventureAI::yourTurn(QueryID queryID)
{
	LOG_TRACE_PARAMS(logAi, "queryID '%i'", queryID);
	nullkiller->invalidatePathfinderData();
	status.addQuery(queryID, "YourTurn");
	executeActionAsync("yourTurn", [this, queryID](){ answerQuery(queryID, 0); });
	status.startedTurn();

	nullkiller->makingTurnInterruption.reset();

	asyncTasks->run([this]()
	{
		ScopedThreadName guard("ScriptedAdventureAI::makingTurn");
		status.waitTillFree();
		makeScriptedTurn();
	});
}

void CScriptedAdventureAI::makeScriptedTurn()
{
	try
	{
		if(!tryMakeScriptedTurn())
			AIGateway::makeTurn();
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
	const auto source = loadScriptSource();
	if(!source)
	{
		fallbackToNullkiller("script source is not available");
		return false;
	}

	auto runner = makeRunner(*source);
	JsonNode progress;

	for(size_t callIndex = 0; callIndex < maxScriptCallsPerTurn && status.haveTurn(); ++callIndex)
	{
		AI::AdventureScriptInput input;
		input.state = makeScriptInputState();
		input.progress = progress;
		input.memory = scriptMemory;
		input.limits = makeScriptInputLimits();

		const AI::AdventureScriptOutput output = runner->planDay(input);
		scriptMemory = output.memory;

		if(output.status == AI::AdventureScriptStatus::FALLBACK)
		{
			fallbackToNullkiller("script requested fallback");
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
				progress = makeProgressJson(executed, failed, remaining);
				fallbackToNullkiller(e.what());
				return false;
			}

			if(actionResult["ok"].isBool() && !actionResult["ok"].Bool())
			{
				failed.Vector().push_back(actionResult);
				for(size_t remainingIndex = actionIndex + 1; remainingIndex < output.actions.size(); ++remainingIndex)
					remaining.Vector().push_back(output.actions[remainingIndex]);
				progress = makeProgressJson(executed, failed, remaining);
				fallbackToNullkiller(actionResult["error"].isString() ? actionResult["error"].String() : "script action failed");
				return false;
			}

			executed.Vector().push_back(actionResult);
			if(stopped || !status.haveTurn())
				break;
		}

		progress = makeProgressJson(executed, failed, remaining);

		if(!status.haveTurn())
			return true;
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

bool CScriptedAdventureAI::executeScriptAction(const JsonNode & action, JsonNode & actionResult)
{
	const std::string type = readString(action, "type");
	actionResult["type"] = JsonNode(type);

	if(type == "build")
	{
		const CGTownInstance * town = cc->getTown(ObjectInstanceID(readInteger(action, "town_id")));
		if(!town || town->tempOwner != playerID)
			throw std::invalid_argument("Unknown town or town is not owned by scripted AI");

		const BuildingID buildingID(readInteger(action, "building_id"));
		if(cc->canBuildStructure(town, buildingID) != EBuildingState::ALLOWED)
			throw std::invalid_argument("Building is not currently allowed");

		buildStructure(town, buildingID);
		waitTillFree();
		actionResult["town_id"] = JsonNode(town->id.getNum());
		actionResult["building_id"] = JsonNode(buildingID.getNum());
		actionResult["ok"] = JsonNode(true);
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

		recruitCreatures(dwelling, destination);
		waitTillFree();
		actionResult["source_id"] = JsonNode(dwelling->id.getNum());
		actionResult["destination_id"] = JsonNode(destination->id.getNum());
		actionResult["ok"] = JsonNode(true);
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
			destination = object->visitablePos();
			actionResult["object_id"] = JsonNode(object->id.getNum());
		}
		else
		{
			const int z = hasField(action, "z") ? readInteger(action, "z") : hero->visitablePos().z;
			destination = int3(readInteger(action, "x"), readInteger(action, "y"), z);
		}

		const bool moved = moveHeroToTile(destination, NK2AI::HeroPtr(hero, cc.get()));
		waitTillFree();
		actionResult["hero_id"] = JsonNode(hero->id.getNum());
		actionResult["destination"] = jsonPosition(destination);
		actionResult["ok"] = JsonNode(moved);
		if(!moved)
			actionResult["error"] = JsonNode("Hero movement did not reach requested destination");
		return moved;
	}

	if(type == "answer_query")
	{
		const QueryID queryID(readInteger(action, "query_id"));
		const int answer = hasField(action, "answer") ? readInteger(action, "answer") : 0;
		answerQuery(queryID, answer);
		waitTillFree();
		actionResult["query_id"] = JsonNode(queryID.getNum());
		actionResult["answer"] = JsonNode(answer);
		actionResult["ok"] = JsonNode(true);
		return true;
	}

	if(type == "end_turn")
	{
		actionResult["ok"] = JsonNode(true);
		endTurn();
		return false;
	}

	throw std::invalid_argument("Unsupported script action type: " + type);
}

JsonNode CScriptedAdventureAI::makeScriptInputState() const
{
	JsonNode state;
	state["day"] = JsonNode(cc->getCalendar().getCurrentDay());
	state["player"] = JsonNode(playerID.toString());
	state["resources"] = JsonNode(cc->getResourceAmount().toString());
	state["heroes"].Vector();
	for(const CGHeroInstance * hero : cc->getHeroesInfo())
	{
		JsonNode heroNode;
		heroNode["id"] = JsonNode(hero->id.getNum());
		heroNode["name"] = JsonNode(hero->getNameTranslated());
		heroNode["position"] = jsonPosition(hero->visitablePos());
		heroNode["movement"] = JsonNode(hero->movementPointsRemaining());
		heroNode["mana"] = JsonNode(hero->mana);
		state["heroes"].Vector().push_back(heroNode);
	}
	state["towns"].Vector();
	for(const CGTownInstance * town : cc->getTownsInfo())
	{
		JsonNode townNode;
		townNode["id"] = JsonNode(town->id.getNum());
		townNode["name"] = JsonNode(town->getNameTranslated());
		townNode["position"] = jsonPosition(town->visitablePos());
		state["towns"].Vector().push_back(townNode);
	}
	return state;
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

std::optional<std::string> CScriptedAdventureAI::loadScriptSource() const
{
	const ScriptPath path = ScriptPath::builtinTODO(scriptPath).addPrefix("SCRIPTS/");
	auto * loader = CResourceHandler::get("core");
	if(!loader || !loader->existsResource(path))
		return std::nullopt;

	auto rawData = loader->load(path)->readAll();
	return std::string(reinterpret_cast<char *>(rawData.first.get()), rawData.second);
}

std::unique_ptr<scripting::LuaAdventureScriptRunner> CScriptedAdventureAI::makeRunner(const std::string & source) const
{
	return std::make_unique<scripting::LuaAdventureScriptRunner>("core:" + scriptPath, source, limits);
}

void CScriptedAdventureAI::fallbackToNullkiller(const std::string & reason)
{
	logAi->warn("ScriptedAdventureAI falling back to Nullkiller: %s", reason);
}

}
