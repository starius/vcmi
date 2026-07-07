#include "../../StdInc.h"

#include "../../lib/ai/AdventureScript.h"

namespace
{

JsonNode parseJson(const std::string & text)
{
	JsonNode node(reinterpret_cast<const std::byte *>(text.data()), text.size(), JsonParsingSettings(), "adventure-script-test");
	return node;
}

}

TEST(AdventureScriptTest, ParsesAndNormalizesScriptOutput)
{
	const JsonNode output = parseJson(R"({
		"status":"need_replan",
		"memory":{"version":1,"mainHero":34},
		"actions":[
			{"tool":"vcmi.move_hero_to_object","arguments":{"hero_id":34,"object_id":180,"route_id":"r1"}},
			{"type":"end"}
		],
		"returnSelect":["summary","heroes"],
		"intent":"collect nearby resources",
		"confidence":0.75
	})");

	const AI::AdventureScriptOutput parsed = AI::parseAdventureScriptOutput(output);

	EXPECT_EQ(parsed.status, AI::AdventureScriptStatus::NEED_REPLAN);
	ASSERT_EQ(parsed.actions.size(), 2);
	EXPECT_EQ(parsed.actions[0]["type"].String(), "visit_object");
	EXPECT_EQ(parsed.actions[0]["hero_id"].Integer(), 34);
	EXPECT_EQ(parsed.actions[1]["type"].String(), "end_turn");
	ASSERT_EQ(parsed.returnSelect.size(), 2);
	EXPECT_EQ(parsed.returnSelect[0], "summary");
	ASSERT_TRUE(parsed.intent);
	EXPECT_EQ(*parsed.intent, "collect nearby resources");
	ASSERT_TRUE(parsed.confidence);
	EXPECT_DOUBLE_EQ(*parsed.confidence, 0.75);
}

TEST(AdventureScriptTest, RejectsUnsupportedStatus)
{
	const JsonNode output = parseJson(R"({"status":"panic","actions":[]})");

	EXPECT_THROW(AI::parseAdventureScriptOutput(output), std::invalid_argument);
}

TEST(AdventureScriptTest, RejectsTooManyActions)
{
	const JsonNode output = parseJson(R"({"actions":[{"type":"end_turn"},{"type":"end_turn"}]})");
	AI::AdventureScriptLimits limits;
	limits.maxActions = 1;

	EXPECT_THROW(AI::parseAdventureScriptOutput(output, limits), std::invalid_argument);
}

TEST(AdventureScriptTest, RejectsOversizedMemory)
{
	const JsonNode output = parseJson(R"({"memory":{"note":"abcdef"},"actions":[]})");
	AI::AdventureScriptLimits limits;
	limits.maxMemoryBytes = 8;

	EXPECT_THROW(AI::parseAdventureScriptOutput(output, limits), std::invalid_argument);
}

TEST(AdventureScriptTest, SerializesScriptInput)
{
	AI::AdventureScriptInput input;
	input.state = parseJson(R"({"turn":1})");
	input.updates = parseJson(R"({"updates":[]})");
	input.memory = parseJson(R"({"version":1})");

	const JsonNode serialized = input.toJson();

	EXPECT_EQ(serialized["state"]["turn"].Integer(), 1);
	EXPECT_TRUE(serialized["updates"]["updates"].isVector());
	EXPECT_EQ(serialized["memory"]["version"].Integer(), 1);
}
