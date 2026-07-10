/*
 * AdventurePlan.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "AdventurePlan.h"

#include <stdexcept>
#include <utility>

namespace AI
{
namespace
{

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

std::string removeToolPrefix(std::string tool)
{
	if(tool.starts_with("vcmi."))
		tool.erase(0, 5);
	return tool;
}

void setSchemaProperty(JsonNode & schema, const std::string & name, const std::string & type, const std::string & description = "")
{
	schema["properties"][name]["type"] = JsonNode(type);
	if(!description.empty())
		schema["properties"][name]["description"] = JsonNode(description);
}

void setRequired(JsonNode & schema, std::initializer_list<const char *> fields)
{
	for(const char * field : fields)
		schema["required"].Vector().push_back(JsonNode(field));
}

JsonNode makeStringArraySchema(const std::string & description)
{
	JsonNode schema;
	schema["type"] = JsonNode("array");
	schema["description"] = JsonNode(description);
	schema["items"]["type"] = JsonNode("string");
	return schema;
}

JsonNode makePlanActionSchema(const std::string & type, std::initializer_list<std::pair<const char *, const char *>> properties, std::initializer_list<const char *> required)
{
	JsonNode schema;
	schema["type"] = JsonNode("object");
	schema["additionalProperties"] = JsonNode(false);
	schema["properties"]["id"]["type"] = JsonNode("string");
	schema["properties"]["type"]["type"] = JsonNode("string");
	schema["properties"]["type"]["enum"].Vector().push_back(JsonNode(type));
	for(const auto & property : properties)
		schema["properties"][property.first]["type"] = JsonNode(property.second);
	setRequired(schema, required);
	return schema;
}

JsonNode makeFlexibleTypedPlanActionSchema()
{
	JsonNode schema;
	schema["type"] = JsonNode("object");
	schema["additionalProperties"] = JsonNode(true);
	schema["properties"]["id"]["type"] = JsonNode("string");
	schema["properties"]["type"]["type"] = JsonNode("string");
	schema["properties"]["type"]["description"] = JsonNode("Canonical or aliased plan action type. Canonical values: build, recruit, hire_hero, transfer_army, move_hero, visit_object, answer_query, cancel_query, end_turn.");
	setRequired(schema, {"type"});
	return schema;
}

JsonNode makeToolShapedPlanActionSchema()
{
	JsonNode schema;
	schema["type"] = JsonNode("object");
	schema["additionalProperties"] = JsonNode(false);
	schema["properties"]["id"]["type"] = JsonNode("string");
	schema["properties"]["tool"]["type"] = JsonNode("string");
	schema["properties"]["tool"]["description"] = JsonNode("MCP tool name to normalize into a plan action, for example vcmi.move_hero_to_object.");
	schema["properties"]["arguments"]["type"] = JsonNode("object");
	schema["properties"]["arguments"]["additionalProperties"] = JsonNode(true);
	setRequired(schema, {"tool"});
	return schema;
}

}

std::vector<std::string> acceptedPlanActionTypes()
{
	return {
		"build",
		"recruit",
		"hire_hero",
		"transfer_army",
		"move_hero",
		"visit_object",
		"answer_query",
		"cancel_query",
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
	if(type == "hire" || type == "hirehero" || type == "recruit_hero" || type == "buy_hero")
		return "hire_hero";
	if(type == "transfer" || type == "army_transfer" || type == "move_army" || type == "bulk_move_army")
		return "transfer_army";
	if(type == "move" || type == "hero_move" || type == "move_tile")
		return "move_hero";
	if(type == "move_hero_to_object" || type == "visit" || type == "object" || type == "capture_object" || type == "collect_object")
		return "visit_object";
	if(type == "answer" || type == "query" || type == "query_answer")
		return "answer_query";
	if(type == "cancel" || type == "cancel_query" || type == "query_cancel")
		return "cancel_query";
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

JsonNode makeExecutePlanSchema()
{
	JsonNode schema;
	schema["type"] = JsonNode("object");
	schema["additionalProperties"] = JsonNode(false);
	setSchemaProperty(schema, "plan_id", "string");
	setSchemaProperty(schema, "dry_run", "boolean");
	setSchemaProperty(schema, "max_updates", "integer");
	schema["properties"]["return_select"] = makeStringArraySchema("Optional state sections to return after executing the plan.");
	schema["properties"]["actions"]["type"] = JsonNode("array");
	schema["properties"]["actions"]["description"] = JsonNode("Sequential day-plan actions. Use canonical type values when possible; aliased and tool-shaped actions are accepted and normalized by VCMI.");
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("build", {{"town_id", "integer"}, {"building_id", "integer"}}, {"type", "town_id", "building_id"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("recruit", {{"source_id", "integer"}, {"town_id", "integer"}, {"destination_id", "integer"}, {"level", "integer"}, {"creature_id", "integer"}, {"amount", "integer"}}, {"type", "level"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("hire_hero", {{"source_id", "integer"}, {"town_id", "integer"}, {"tavern_id", "integer"}, {"hero_type_id", "integer"}, {"next_hero_type_id", "integer"}}, {"type", "hero_type_id"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("transfer_army", {{"source_id", "integer"}, {"destination_id", "integer"}, {"source_slot", "integer"}}, {"type", "source_id", "destination_id", "source_slot"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("move_hero", {{"hero_id", "integer"}, {"x", "integer"}, {"y", "integer"}, {"z", "integer"}, {"route_id", "string"}}, {"type", "hero_id", "x", "y"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("visit_object", {{"hero_id", "integer"}, {"object_id", "integer"}, {"route_id", "string"}}, {"type", "hero_id", "object_id"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("answer_query", {{"query_id", "integer"}, {"answer", "integer"}}, {"type", "query_id"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("cancel_query", {{"query_id", "integer"}}, {"type", "query_id"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makePlanActionSchema("end_turn", {}, {"type"}));
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makeFlexibleTypedPlanActionSchema());
	schema["properties"]["actions"]["items"]["anyOf"].Vector().push_back(makeToolShapedPlanActionSchema());
	setRequired(schema, {"actions"});
	return schema;
}

}
