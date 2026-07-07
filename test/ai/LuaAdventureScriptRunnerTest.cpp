#include "../../StdInc.h"

#include "../../luascript/LuaAdventureScriptRunner.h"

namespace
{

AI::AdventureScriptInput makeInput()
{
	AI::AdventureScriptInput input;
	input.state["resources"]["gold"] = JsonNode(1200);
	input.memory["version"] = JsonNode(1);
	return input;
}

}

TEST(LuaAdventureScriptRunnerTest, CallsPlanDayAndValidatesOutput)
{
	const std::string source = R"lua(
		local Script = {}

		function Script.planDay(input)
			return {
				status = "end_turn",
				memory = { version = input.memory.version, selected = input.state.resources.gold },
				actions = {
					{ type = "move", hero_id = 34, x = 12, y = 7 },
					{ type = "end" }
				},
				returnSelect = { "summary", "heroes" },
				intent = "test plan",
				confidence = 0.5
			}
		end

		return Script
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:good", source);

	const AI::AdventureScriptOutput output = runner.planDay(makeInput());

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["selected"].Integer(), 1200);
	ASSERT_EQ(output.actions.size(), 2);
	EXPECT_EQ(output.actions[0]["type"].String(), "move_hero");
	EXPECT_EQ(output.actions[1]["type"].String(), "end_turn");
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "test plan");
	ASSERT_TRUE(output.confidence);
	EXPECT_DOUBLE_EQ(*output.confidence, 0.5);
}

TEST(LuaAdventureScriptRunnerTest, RejectsMissingPlanDay)
{
	const std::string source = R"lua(
		return {}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:missing", source);

	EXPECT_THROW(runner.planDay(makeInput()), std::runtime_error);
}

TEST(LuaAdventureScriptRunnerTest, RejectsInvalidOutput)
{
	const std::string source = R"lua(
		return {
			planDay = function(input)
				return { status = "invalid" }
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:invalid", source);

	EXPECT_THROW(runner.planDay(makeInput()), std::invalid_argument);
}

TEST(LuaAdventureScriptRunnerTest, RemovesUnsafeGlobals)
{
	const std::string source = R"lua(
		return {
			planDay = function(input)
				return {
					status = "continue",
					memory = {
						hasLoad = load ~= nil,
						hasRandom = math.random ~= nil
					},
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:restricted", source);

	const AI::AdventureScriptOutput output = runner.planDay(makeInput());

	EXPECT_FALSE(output.memory["hasLoad"].Bool());
	EXPECT_FALSE(output.memory["hasRandom"].Bool());
}
