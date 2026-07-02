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
