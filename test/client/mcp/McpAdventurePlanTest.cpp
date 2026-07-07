#include "../../StdInc.h"

#include "../../../client/mcp/McpAdventurePlan.h"

namespace
{

JsonNode parseJson(const std::string & text)
{
	JsonNode node(reinterpret_cast<const std::byte *>(text.data()), text.size(), JsonParsingSettings(), "mcp-adventure-plan-test");
	return node;
}

}

TEST(McpAdventurePlanTest, NormalizesToolShapedPlanActions)
{
	const JsonNode action = parseJson(R"({
		"id":"visit-wood",
		"tool":"vcmi.move_hero_to_object",
		"arguments":{"hero_id":990,"object_id":181,"route_id":"r1"}
	})");

	const JsonNode normalized = Mcp::normalizePlanAction(action);

	EXPECT_EQ(normalized["id"].String(), "visit-wood");
	EXPECT_EQ(normalized["type"].String(), "visit_object");
	EXPECT_EQ(normalized["hero_id"].Integer(), 990);
	EXPECT_EQ(normalized["object_id"].Integer(), 181);
	EXPECT_EQ(normalized["route_id"].String(), "r1");
}

TEST(McpAdventurePlanTest, NormalizesPlanActionAliases)
{
	EXPECT_EQ(Mcp::canonicalPlanActionType("move"), "move_hero");
	EXPECT_EQ(Mcp::canonicalPlanActionType("hero_move"), "move_hero");
	EXPECT_EQ(Mcp::canonicalPlanActionType("vcmi.move_hero"), "move_hero");
	EXPECT_EQ(Mcp::canonicalPlanActionType("move_hero_to_object"), "visit_object");
	EXPECT_EQ(Mcp::canonicalPlanActionType("vcmi.build_town_building"), "build");
	EXPECT_EQ(Mcp::canonicalPlanActionType("end"), "end_turn");

	const JsonNode action = parseJson(R"({"type":"move","hero_id":990,"x":20,"y":2})");
	const JsonNode normalized = Mcp::normalizePlanAction(action);
	EXPECT_EQ(normalized["type"].String(), "move_hero");
}

TEST(McpAdventurePlanTest, ExecutePlanSchemaDescribesCanonicalAndToolShapedActions)
{
	const JsonNode schema = Mcp::makeExecutePlanSchema();

	EXPECT_EQ(schema["type"].String(), "object");
	ASSERT_TRUE(schema["properties"]["actions"]["items"]["anyOf"].isVector());

	const auto & variants = schema["properties"]["actions"]["items"]["anyOf"].Vector();
	EXPECT_GE(variants.size(), 8);

	bool hasBuild = false;
	bool hasToolShape = false;
	for(const JsonNode & variant : variants)
	{
		if(variant["properties"]["type"]["enum"].isVector()
			&& !variant["properties"]["type"]["enum"].Vector().empty()
			&& variant["properties"]["type"]["enum"].Vector().front().String() == "build")
		{
			hasBuild = true;
		}
		if(variant["properties"]["tool"].isStruct())
			hasToolShape = true;
	}

	EXPECT_TRUE(hasBuild);
	EXPECT_TRUE(hasToolShape);
}
