/*
 * AdventureScript.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "AdventureScript.h"

#include <stdexcept>

namespace AI
{
namespace
{

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

std::string readString(const JsonNode & node, const std::string & field)
{
	if(!node[field].isString())
		throw std::invalid_argument("Missing or non-string script output field: " + field);
	return node[field].String();
}

double readNumber(const JsonNode & node, const std::string & field)
{
	if(!node[field].isNumber())
		throw std::invalid_argument("Non-number script output field: " + field);
	return node[field].Float();
}

std::vector<std::string> readStringVector(const JsonNode & node, const std::string & field)
{
	if(!node[field].isVector())
		throw std::invalid_argument("Non-array script output field: " + field);

	std::vector<std::string> result;
	result.reserve(node[field].Vector().size());
	for(const JsonNode & item : node[field].Vector())
	{
		if(!item.isString())
			throw std::invalid_argument("Script output field '" + field + "' must contain only strings");
		result.push_back(item.String());
	}
	return result;
}

}

JsonNode AdventureScriptInput::toJson() const
{
	JsonNode result;
	result["state"] = state;
	result["updates"] = updates;
	result["opponentUpdates"] = opponentUpdates;
	result["progress"] = progress;
	result["memory"] = memory;
	result["actionSpace"] = actionSpace;
	result["analysis"] = analysis;
	result["limits"] = limits;
	return result;
}

std::string adventureScriptStatusToString(AdventureScriptStatus status)
{
	switch(status)
	{
		case AdventureScriptStatus::CONTINUE:
			return "continue";
		case AdventureScriptStatus::END_TURN:
			return "end_turn";
		case AdventureScriptStatus::NEED_REPLAN:
			return "need_replan";
		case AdventureScriptStatus::FALLBACK:
			return "fallback";
	}
	return "fallback";
}

AdventureScriptStatus adventureScriptStatusFromString(const std::string & status)
{
	if(status == "continue")
		return AdventureScriptStatus::CONTINUE;
	if(status == "end_turn")
		return AdventureScriptStatus::END_TURN;
	if(status == "need_replan")
		return AdventureScriptStatus::NEED_REPLAN;
	if(status == "fallback")
		return AdventureScriptStatus::FALLBACK;
	throw std::invalid_argument("Unsupported script status: " + status);
}

AdventureScriptOutput parseAdventureScriptOutput(const JsonNode & output, const AdventureScriptLimits & limits)
{
	if(!output.isStruct())
		throw std::invalid_argument("Script output must be an object");

	AdventureScriptOutput result;
	if(hasField(output, "status"))
		result.status = adventureScriptStatusFromString(readString(output, "status"));

	if(hasField(output, "memory"))
	{
		result.memory = output["memory"];
		if(result.memory.toCompactString().size() > limits.maxMemoryBytes)
			throw std::invalid_argument("Script memory exceeds configured size limit");
	}

	if(hasField(output, "actions"))
	{
		if(!output["actions"].isVector())
			throw std::invalid_argument("Script output field 'actions' must be an array");
		if(output["actions"].Vector().size() > limits.maxActions)
			throw std::invalid_argument("Script output has too many actions");

		result.actions.reserve(output["actions"].Vector().size());
		for(const JsonNode & action : output["actions"].Vector())
			result.actions.push_back(normalizePlanAction(action));
	}

	if(hasField(output, "returnSelect"))
		result.returnSelect = readStringVector(output, "returnSelect");
	if(hasField(output, "intent"))
		result.intent = readString(output, "intent");
	if(hasField(output, "confidence"))
		result.confidence = readNumber(output, "confidence");

	return result;
}

JsonNode makeAdventureScriptOutputJson(const AdventureScriptOutput & output)
{
	JsonNode result;
	result["status"] = JsonNode(adventureScriptStatusToString(output.status));
	result["memory"] = output.memory;
	result["actions"].Vector();
	for(const JsonNode & action : output.actions)
		result["actions"].Vector().push_back(action);
	result["returnSelect"].Vector();
	for(const std::string & section : output.returnSelect)
		result["returnSelect"].Vector().push_back(JsonNode(section));
	if(output.intent)
		result["intent"] = JsonNode(*output.intent);
	if(output.confidence)
		result["confidence"] = JsonNode(*output.confidence);
	return result;
}

}
