/*
 * McpGameHelpers.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "McpGameHelpers.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace Mcp
{

namespace
{

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

uint64_t readUnsignedInteger(const JsonNode & node, const std::string & field, uint64_t defaultValue)
{
	if(!hasField(node, field))
		return defaultValue;
	if(!node[field].isNumber() || node[field].getType() != JsonNode::JsonType::DATA_INTEGER)
		throw std::invalid_argument("Missing or non-integer argument: " + field);
	if(node[field].Integer() < 0)
		throw std::invalid_argument("Negative integer argument: " + field);
	return static_cast<uint64_t>(node[field].Integer());
}

std::string canonicalSectionName(std::string section)
{
	if(section == "ownedHeroes")
		return "heroes";
	if(section == "ownedTowns")
		return "towns";
	if(section == "visibleMap")
		return "visibleMap";
	return section;
}

std::string removeToolPrefix(std::string tool)
{
	if(tool.starts_with("vcmi."))
		tool.erase(0, 5);
	return tool;
}

void copyIfPresent(JsonNode & destination, const JsonNode & source, const std::string & key)
{
	if(hasField(source, key))
		destination[key] = source[key];
}

}

bool StateSelection::fullState() const
{
	return sections.empty();
}

bool StateSelection::includes(const std::string & section) const
{
	return fullState() || sections.count(canonicalSectionName(section)) != 0;
}

StateSelection readStateSelection(const JsonNode & arguments)
{
	StateSelection selection;
	selection.sinceRevision = readUnsignedInteger(arguments, "since_revision", 0);
	if(*selection.sinceRevision == 0)
		selection.sinceRevision.reset();
	selection.maxUpdates = static_cast<size_t>(std::clamp<uint64_t>(readUnsignedInteger(arguments, "max_updates", 200), 1, 1000));

	if(!hasField(arguments, "select"))
		return selection;
	if(!arguments["select"].isVector())
		throw std::invalid_argument("Non-array argument: select");

	for(const JsonNode & node : arguments["select"].Vector())
	{
		if(!node.isString())
			throw std::invalid_argument("select entries must be strings");
		selection.sections.insert(canonicalSectionName(node.String()));
	}

	return selection;
}

JsonNode filterStateBySelection(const JsonNode & state, const StateSelection & selection)
{
	if(selection.fullState())
		return state;

	JsonNode result;
	copyIfPresent(result, state, "initialized");
	copyIfPresent(result, state, "revision");
	copyIfPresent(result, state, "player");

	if(selection.includes("summary"))
	{
		copyIfPresent(result["summary"], state, "turn");
		copyIfPresent(result["summary"], state, "pendingQuery");
		copyIfPresent(result["summary"], state, "battle");
		copyIfPresent(result["summary"], state, "resources");
	}
	if(selection.includes("resources"))
		copyIfPresent(result, state, "resources");
	if(selection.includes("heroes"))
		copyIfPresent(result, state, "heroes");
	if(selection.includes("towns"))
		copyIfPresent(result, state, "towns");
	if(selection.includes("tavern"))
		copyIfPresent(result, state, "tavern");
	if(selection.includes("visibleMap"))
		copyIfPresent(result, state, "visibleMap");
	if(selection.includes("reachable"))
		copyIfPresent(result, state, "reachable");
	if(selection.includes("updates"))
		copyIfPresent(result, state, "updates");
	if(selection.includes("battle"))
		copyIfPresent(result, state, "battle");
	if(selection.includes("pendingQuery"))
		copyIfPresent(result, state, "pendingQuery");

	return result;
}

std::string makeRouteId(
	int32_t heroId,
	int32_t startX,
	int32_t startY,
	int32_t startZ,
	int32_t destinationX,
	int32_t destinationY,
	int32_t destinationZ,
	int32_t layerId,
	int32_t movementRemaining)
{
	std::ostringstream stream;
	stream
		<< "h" << heroId
		<< ":s" << startX << "," << startY << "," << startZ
		<< ":d" << destinationX << "," << destinationY << "," << destinationZ
		<< ":l" << layerId
		<< ":m" << movementRemaining;
	return stream.str();
}

JsonNode makeRevisionedUpdate(uint64_t revision, const std::string & type, JsonNode data)
{
	JsonNode update;
	update["revision"] = JsonNode(static_cast<int64_t>(revision));
	update["type"] = JsonNode(type);
	update["data"] = std::move(data);
	return update;
}

JsonNode collectUpdatesSince(const std::deque<JsonNode> & journal, uint64_t sinceRevision, size_t maxUpdates)
{
	JsonNode result;
	result["sinceRevision"] = JsonNode(static_cast<int64_t>(sinceRevision));
	result["truncated"] = JsonNode(false);
	result["updates"].Vector();

	size_t emitted = 0;
	for(const JsonNode & update : journal)
	{
		if(!hasField(update, "revision") || update["revision"].Integer() <= static_cast<int64_t>(sinceRevision))
			continue;

		if(emitted >= maxUpdates)
		{
			result["truncated"] = JsonNode(true);
			break;
		}

		result["updates"].Vector().push_back(update);
		++emitted;
	}

	if(!result["updates"].Vector().empty())
		result["latestRevision"] = result["updates"].Vector().back()["revision"];
	else
		result["latestRevision"] = JsonNode(static_cast<int64_t>(sinceRevision));

	return result;
}

std::vector<std::string> acceptedPlanActionTypes()
{
	return {
		"build",
		"recruit",
		"move_hero",
		"visit_object",
		"answer_query",
		"end_turn"
	};
}

std::string canonicalPlanActionType(std::string type)
{
	type = removeToolPrefix(std::move(type));

	if(type == "build_town_building" || type == "build_building" || type == "building")
		return "build";
	if(type == "recruit_creatures" || type == "buy_creatures" || type == "recruitment")
		return "recruit";
	if(type == "move" || type == "hero_move" || type == "move_tile")
		return "move_hero";
	if(type == "move_hero_to_object" || type == "visit" || type == "object" || type == "capture_object" || type == "collect_object")
		return "visit_object";
	if(type == "answer" || type == "query" || type == "query_answer")
		return "answer_query";
	if(type == "end" || type == "end_day")
		return "end_turn";
	return type;
}

JsonNode normalizePlanAction(const JsonNode & action)
{
	if(!action.isStruct())
		throw std::invalid_argument("Plan action must be an object");

	JsonNode normalized;
	if(hasField(action, "tool"))
	{
		if(!action["tool"].isString())
			throw std::invalid_argument("Plan action field 'tool' must be a string");

		if(hasField(action, "arguments"))
		{
			if(!action["arguments"].isStruct())
				throw std::invalid_argument("Plan action field 'arguments' must be an object");
			normalized = action["arguments"];
		}
		else
		{
			normalized.Struct();
		}

		if(hasField(action, "id"))
			normalized["id"] = action["id"];
		if(!hasField(normalized, "type"))
			normalized["type"] = JsonNode(canonicalPlanActionType(action["tool"].String()));
	}
	else
	{
		normalized = action;
	}

	if(hasField(normalized, "type"))
	{
		if(!normalized["type"].isString())
			throw std::invalid_argument("Plan action field 'type' must be a string");
		normalized["type"] = JsonNode(canonicalPlanActionType(normalized["type"].String()));
	}

	return normalized;
}

}
