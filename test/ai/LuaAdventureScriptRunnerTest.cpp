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

std::string readAdventureScript(const std::string & path)
{
	std::ifstream stream(std::string(VCMI_SOURCE_DIR) + "/" + path);
	if(!stream)
		throw std::runtime_error("Unable to read adventure script: " + path);

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

	scripting::LuaAdventureScriptRunner runner("scripts/ai/defaultAdventure.lua", readAdventureScript("scripts/ai/defaultAdventure.lua"));
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

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureScriptMovesThreatenedHeroAway)
{
	AI::AdventureScriptInput input = makeInput();
	input.state["day"] = JsonNode(2);

	JsonNode alert;
	alert["level"] = JsonNode("critical");
	alert["hero_id"] = JsonNode(5);
	alert["distanceSquared"] = JsonNode(4);
	alert["enemyPosition"]["x"] = JsonNode(0);
	alert["enemyPosition"]["y"] = JsonNode(0);
	alert["enemyPosition"]["z"] = JsonNode(0);
	input.analysis["heroThreatAlerts"].Vector().push_back(alert);

	JsonNode movement;
	movement["hero_id"] = JsonNode(5);
	movement["hero"] = JsonNode("Scout");
	movement["safe"] = JsonNode(true);
	movement["value"] = JsonNode(100.0);
	movement["path"]["destination"]["x"] = JsonNode(8);
	movement["path"]["destination"]["y"] = JsonNode(8);
	movement["path"]["destination"]["z"] = JsonNode(0);
	movement["path"]["isTeleportAction"] = JsonNode(false);
	movement["planAction"]["type"] = JsonNode("move_hero");
	movement["planAction"]["hero_id"] = JsonNode(5);
	movement["planAction"]["x"] = JsonNode(8);
	movement["planAction"]["y"] = JsonNode(8);
	movement["planAction"]["z"] = JsonNode(0);
	movement["planAction"]["route_id"] = JsonNode("escape");
	input.actionSpace["movementOptions"].Vector().push_back(movement);

	scripting::LuaAdventureScriptRunner runner("scripts/ai/defaultAdventure.lua", readAdventureScript("scripts/ai/defaultAdventure.lua"));
	const AI::AdventureScriptOutput output = runner.planDay(input);

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::NEED_REPLAN);
	ASSERT_EQ(output.actions.size(), 1);
	EXPECT_EQ(output.actions[0]["type"].String(), "move_hero");
	EXPECT_EQ(output.actions[0]["route_id"].String(), "escape");
	ASSERT_TRUE(output.intent);
	EXPECT_NE(output.intent->find("move threatened hero"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureScriptPrefersSafeObjectTarget)
{
	AI::AdventureScriptInput input = makeInput();
	input.state["day"] = JsonNode(2);

	JsonNode unsafeTarget;
	unsafeTarget["object"]["id"] = JsonNode(77);
	unsafeTarget["object"]["name"] = JsonNode("Gold pile");
	unsafeTarget["object"]["type"] = JsonNode("Resource");
	unsafeTarget["path"]["cost"].Float() = 0.1;
	unsafeTarget["path"]["pathAction"] = JsonNode("battle");
	unsafeTarget["path"]["isTeleportAction"] = JsonNode(false);
	unsafeTarget["safe"] = JsonNode(false);
	unsafeTarget["dangerRatio"] = JsonNode(3.0);
	unsafeTarget["value"] = JsonNode(1000.0);
	unsafeTarget["planAction"]["type"] = JsonNode("visit_object");
	unsafeTarget["planAction"]["hero_id"] = JsonNode(5);
	unsafeTarget["planAction"]["object_id"] = JsonNode(77);
	unsafeTarget["planAction"]["route_id"] = JsonNode("unsafe");
	input.actionSpace["reachableObjects"].Vector().push_back(unsafeTarget);

	JsonNode safeTarget;
	safeTarget["object"]["id"] = JsonNode(88);
	safeTarget["object"]["name"] = JsonNode("Wood pile");
	safeTarget["object"]["type"] = JsonNode("Resource");
	safeTarget["path"]["cost"].Float() = 0.4;
	safeTarget["path"]["pathAction"] = JsonNode("visit");
	safeTarget["path"]["isTeleportAction"] = JsonNode(false);
	safeTarget["safe"] = JsonNode(true);
	safeTarget["dangerRatio"] = JsonNode(0.0);
	safeTarget["value"] = JsonNode(500.0);
	safeTarget["planAction"]["type"] = JsonNode("visit_object");
	safeTarget["planAction"]["hero_id"] = JsonNode(5);
	safeTarget["planAction"]["object_id"] = JsonNode(88);
	safeTarget["planAction"]["route_id"] = JsonNode("safe");
	input.actionSpace["reachableObjects"].Vector().push_back(safeTarget);

	scripting::LuaAdventureScriptRunner runner("scripts/ai/defaultAdventure.lua", readAdventureScript("scripts/ai/defaultAdventure.lua"));
	const AI::AdventureScriptOutput output = runner.planDay(input);

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::NEED_REPLAN);
	ASSERT_EQ(output.actions.size(), 1);
	EXPECT_EQ(output.actions[0]["type"].String(), "visit_object");
	EXPECT_EQ(output.actions[0]["object_id"].Integer(), 88);
}

TEST(LuaAdventureScriptRunnerTest, BundledAdventureScriptVariantsRun)
{
	const std::vector<std::string> scripts = {
		"scripts/ai/defaultAdventure.lua",
		"scripts/ai/aggressiveAdventure.lua",
		"scripts/ai/economyAdventure.lua",
		"scripts/ai/explorerAdventure.lua"
	};

	for(const std::string & script : scripts)
	{
		scripting::LuaAdventureScriptRunner runner(script, readAdventureScript(script));
		const AI::AdventureScriptOutput output = runner.planDay(makeInput());

		EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN) << script;
		ASSERT_EQ(output.actions.size(), 1) << script;
		EXPECT_EQ(output.actions[0]["type"].String(), "end_turn") << script;
	}
}
