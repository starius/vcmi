#include "../../StdInc.h"

#include "../../luascript/LuaAdventureScriptRunner.h"

#include <fstream>
#include <sstream>

namespace
{

AI::AdventureScriptInput makeInput()
{
	AI::AdventureScriptInput input;
	input.state["resources"]["gold"] = JsonNode(1200);
	input.memory["version"] = JsonNode(1);
	return input;
}

std::string readDefaultAdventureScript()
{
	std::ifstream stream(std::string(VCMI_SOURCE_DIR) + "/scripts/ai/defaultAdventure.lua");
	if(!stream)
		throw std::runtime_error("Unable to read default adventure script");

	std::ostringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
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

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureScriptScoresCandidates)
{
	AI::AdventureScriptInput input = makeInput();
	input.state["day"] = JsonNode(1);
	input.state["resources"]["gold"] = JsonNode(5000);

	JsonNode buildOption;
	buildOption["building"] = JsonNode("City Hall");
	buildOption["building_id"] = JsonNode(11);
	buildOption["town_id"] = JsonNode(42);
	buildOption["cost"]["gold"] = JsonNode(2500);
	buildOption["income"]["gold"] = JsonNode(1000);
	buildOption["planAction"]["type"] = JsonNode("build");
	buildOption["planAction"]["town_id"] = JsonNode(42);
	buildOption["planAction"]["building_id"] = JsonNode(11);
	input.actionSpace["buildOptions"].Vector().push_back(buildOption);

	JsonNode target;
	target["object"]["id"] = JsonNode(77);
	target["object"]["name"] = JsonNode("Gold pile");
	target["object"]["type"] = JsonNode("Resource");
	target["path"]["cost"].Float() = 0.25;
	target["path"]["pathAction"] = JsonNode("visit");
	target["path"]["isTeleportAction"] = JsonNode(false);
	target["planAction"]["type"] = JsonNode("visit_object");
	target["planAction"]["hero_id"] = JsonNode(5);
	target["planAction"]["object_id"] = JsonNode(77);
	target["planAction"]["route_id"] = JsonNode("route");
	input.actionSpace["reachableObjects"].Vector().push_back(target);

	scripting::LuaAdventureScriptRunner runner("scripts/ai/defaultAdventure.lua", readDefaultAdventureScript());
	const AI::AdventureScriptOutput output = runner.planDay(input);

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::NEED_REPLAN);
	ASSERT_EQ(output.actions.size(), 2);
	EXPECT_EQ(output.actions[0]["type"].String(), "build");
	EXPECT_EQ(output.actions[0]["town_id"].Integer(), 42);
	EXPECT_EQ(output.actions[1]["type"].String(), "visit_object");
	EXPECT_EQ(output.actions[1]["object_id"].Integer(), 77);
	EXPECT_EQ(output.memory["version"].Integer(), 1);
	ASSERT_TRUE(output.intent);
	EXPECT_NE(output.intent->find("City Hall"), std::string::npos);
}
