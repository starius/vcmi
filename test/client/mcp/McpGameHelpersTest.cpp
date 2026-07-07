#include "../../StdInc.h"

#include "../../../client/mcp/McpGameHelpers.h"

namespace
{

JsonNode parseJson(const std::string & text)
{
	JsonNode node(reinterpret_cast<const std::byte *>(text.data()), text.size(), JsonParsingSettings(), "mcp-game-helper-test");
	return node;
}

}

TEST(McpGameHelpersTest, ReadsStateSelectorsAndAliases)
{
	const JsonNode arguments = parseJson(R"({"select":["summary","ownedHeroes","ownedTowns","updates"],"since_revision":12,"max_updates":5})");

	const Mcp::StateSelection selection = Mcp::readStateSelection(arguments);

	EXPECT_TRUE(selection.includes("summary"));
	EXPECT_TRUE(selection.includes("heroes"));
	EXPECT_TRUE(selection.includes("towns"));
	EXPECT_TRUE(selection.includes("updates"));
	ASSERT_TRUE(selection.sinceRevision);
	EXPECT_EQ(*selection.sinceRevision, 12);
	EXPECT_EQ(selection.maxUpdates, 5);
}

TEST(McpGameHelpersTest, FiltersStateBySelectedSections)
{
	const JsonNode state = parseJson(R"({
		"initialized":true,
		"revision":7,
		"player":{"color":"red"},
		"turn":{"active":true},
		"pendingQuery":null,
		"battle":{"activeStack":false},
		"resources":{"gold":1000},
		"heroes":[{"id":1}],
		"towns":[{"id":2}],
		"updates":{"updates":[]}
	})");
	const JsonNode arguments = parseJson(R"({"select":["summary","ownedHeroes"]})");

	const JsonNode filtered = Mcp::filterStateBySelection(state, Mcp::readStateSelection(arguments));

	EXPECT_TRUE(filtered["initialized"].Bool());
	EXPECT_EQ(filtered["player"]["color"].String(), "red");
	EXPECT_TRUE(filtered["summary"]["turn"]["active"].Bool());
	EXPECT_EQ(filtered["heroes"].Vector().size(), 1);
	EXPECT_FALSE(filtered["towns"].isVector());
	EXPECT_FALSE(filtered["updates"].isStruct());
}

TEST(McpGameHelpersTest, MakesStableRouteIds)
{
	EXPECT_EQ(
		Mcp::makeRouteId(990, 18, 4, 0, 20, 2, 0, 0, 1200),
		"h990:s18,4,0:d20,2,0:l0:m1200");
}

TEST(McpGameHelpersTest, CollectsRevisionedUpdates)
{
	std::deque<JsonNode> journal;
	JsonNode first;
	first["id"] = JsonNode(1);
	JsonNode second;
	second["id"] = JsonNode(2);
	JsonNode third;
	third["id"] = JsonNode(3);

	journal.push_back(Mcp::makeRevisionedUpdate(1, "turn.started", first));
	journal.push_back(Mcp::makeRevisionedUpdate(2, "hero.moved", second));
	journal.push_back(Mcp::makeRevisionedUpdate(3, "object.removed", third));

	const JsonNode updates = Mcp::collectUpdatesSince(journal, 1, 1);

	EXPECT_TRUE(updates["truncated"].Bool());
	ASSERT_EQ(updates["updates"].Vector().size(), 1);
	EXPECT_EQ(updates["updates"].Vector()[0]["revision"].Integer(), 2);
	EXPECT_EQ(updates["updates"].Vector()[0]["type"].String(), "hero.moved");
}

TEST(McpGameHelpersTest, NormalizesToolShapedPlanActions)
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

TEST(McpGameHelpersTest, NormalizesPlanActionAliases)
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
