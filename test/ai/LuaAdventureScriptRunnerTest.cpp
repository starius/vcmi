#include "../../StdInc.h"

#include "../../lib/ai/AdventureScript.h"
#include "../../luascript/LuaAdventureScriptRunner.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
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

JsonNode readJsonFile(const std::filesystem::path & path)
{
	std::ifstream stream(path);
	if(!stream)
		throw std::runtime_error("Unable to read adventure script fixture: " + path.string());

	std::ostringstream buffer;
	buffer << stream.rdbuf();
	const std::string text = buffer.str();

	JsonParsingSettings settings;
	settings.strict = true;
	return JsonNode(text.data(), text.size(), settings, path.string());
}

bool hasField(const JsonNode & node, const std::string & field)
{
	return node.isStruct() && node.Struct().find(field) != node.Struct().end();
}

bool numberEquals(const JsonNode & actual, const JsonNode & expected)
{
	return actual.isNumber() && expected.isNumber() && std::abs(actual.Float() - expected.Float()) < 1e-8;
}

bool matchesPartialJson(const JsonNode & actual, const JsonNode & expected)
{
	if(expected.isStruct())
	{
		if(!actual.isStruct())
			return false;

		for(const auto & [key, expectedValue] : expected.Struct())
		{
			if(!hasField(actual, key) || !matchesPartialJson(actual[key], expectedValue))
				return false;
		}
		return true;
	}

	if(expected.isVector())
	{
		if(!actual.isVector() || actual.Vector().size() != expected.Vector().size())
			return false;

		for(size_t index = 0; index < expected.Vector().size(); ++index)
		{
			if(!matchesPartialJson(actual.Vector()[index], expected.Vector()[index]))
				return false;
		}
		return true;
	}

	if(numberEquals(actual, expected))
		return true;

	return actual == expected;
}

bool containsMatchingAction(const std::vector<JsonNode> & actions, const JsonNode & expected)
{
	return std::any_of(actions.begin(), actions.end(), [&](const JsonNode & action)
	{
		return matchesPartialJson(action, expected);
	});
}

AI::AdventureScriptInput readFixtureInput(const JsonNode & fixture)
{
	AI::AdventureScriptInput input;
	const JsonNode & data = fixture["input"];

	if(hasField(data, "state"))
		input.state = data["state"];
	if(hasField(data, "updates"))
		input.updates = data["updates"];
	if(hasField(data, "opponentUpdates"))
		input.opponentUpdates = data["opponentUpdates"];
	if(hasField(data, "progress"))
		input.progress = data["progress"];
	if(hasField(data, "memory"))
		input.memory = data["memory"];
	if(hasField(data, "actionSpace"))
		input.actionSpace = data["actionSpace"];
	if(hasField(data, "analysis"))
		input.analysis = data["analysis"];
	if(hasField(data, "limits"))
		input.limits = data["limits"];

	return input;
}

void expectStringContains(const std::string & value, const JsonNode & expected, const std::string & field)
{
	if(expected.isString())
	{
		EXPECT_NE(value.find(expected.String()), std::string::npos) << field;
		return;
	}

	ASSERT_TRUE(expected.isVector()) << field << " must be a string or array of strings";
	for(const JsonNode & item : expected.Vector())
	{
		ASSERT_TRUE(item.isString()) << field << " must contain only strings";
		EXPECT_NE(value.find(item.String()), std::string::npos) << field;
	}
}

void verifyFixtureExpectation(const std::filesystem::path & path, const JsonNode & fixture, const AI::AdventureScriptOutput & output)
{
	SCOPED_TRACE(path.string());
	const JsonNode outputJson = AI::makeAdventureScriptOutputJson(output);
	const JsonNode & expect = fixture["expect"];

	if(hasField(expect, "status"))
	{
		EXPECT_EQ(AI::adventureScriptStatusToString(output.status), expect["status"].String());
	}

	if(hasField(expect, "firstAction"))
	{
		ASSERT_FALSE(output.actions.empty());
		EXPECT_TRUE(matchesPartialJson(output.actions.front(), expect["firstAction"]))
			<< "expected: " << expect["firstAction"].toCompactString()
			<< "\nactual: " << output.actions.front().toCompactString();
	}

	if(hasField(expect, "actionsExact"))
	{
		ASSERT_TRUE(expect["actionsExact"].isVector());
		ASSERT_EQ(output.actions.size(), expect["actionsExact"].Vector().size());
		for(size_t index = 0; index < expect["actionsExact"].Vector().size(); ++index)
		{
			EXPECT_TRUE(matchesPartialJson(output.actions[index], expect["actionsExact"].Vector()[index]))
				<< "action index: " << index
				<< "\nexpected: " << expect["actionsExact"].Vector()[index].toCompactString()
				<< "\nactual: " << output.actions[index].toCompactString();
		}
	}

	if(hasField(expect, "actionsContain"))
	{
		ASSERT_TRUE(expect["actionsContain"].isVector());
		for(const JsonNode & action : expect["actionsContain"].Vector())
		{
			EXPECT_TRUE(containsMatchingAction(output.actions, action)) << action.toCompactString();
		}
	}

	if(hasField(expect, "actionsDoNotContain"))
	{
		ASSERT_TRUE(expect["actionsDoNotContain"].isVector());
		for(const JsonNode & action : expect["actionsDoNotContain"].Vector())
		{
			EXPECT_FALSE(containsMatchingAction(output.actions, action)) << action.toCompactString();
		}
	}

	if(hasField(expect, "intentContains"))
	{
		ASSERT_TRUE(output.intent);
		expectStringContains(*output.intent, expect["intentContains"], "intentContains");
	}

	if(hasField(expect, "memoryContains"))
	{
		EXPECT_TRUE(matchesPartialJson(output.memory, expect["memoryContains"]))
			<< "expected: " << expect["memoryContains"].toCompactString()
			<< "\nactual: " << output.memory.toCompactString();
	}

	if(hasField(expect, "memoryDoesNotContain"))
	{
		EXPECT_FALSE(matchesPartialJson(output.memory, expect["memoryDoesNotContain"]))
			<< "unexpected memory pattern: " << expect["memoryDoesNotContain"].toCompactString()
			<< "\nactual: " << output.memory.toCompactString();
	}

	if(hasField(expect, "outputContains"))
	{
		EXPECT_TRUE(matchesPartialJson(outputJson, expect["outputContains"]))
			<< "expected: " << expect["outputContains"].toCompactString()
			<< "\nactual: " << outputJson.toCompactString();
	}
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

TEST(LuaAdventureScriptRunnerTest, RunsImperativeDayAndExecutesHostCommand)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local result = ai:build(7, 12)
				return ai:output("end_turn", "built from imperative script", 0.75)
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-execute", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		response["result"]["town_id"] = command["payload"]["town_id"];
		response["result"]["building_id"] = command["payload"]["building_id"];
		return response;
	});

	ASSERT_EQ(commands.size(), 1);
	EXPECT_EQ(commands[0]["kind"].String(), "execute");
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "build");
	EXPECT_EQ(commands[0]["payload"]["town_id"].Integer(), 7);
	EXPECT_EQ(commands[0]["payload"]["building_id"].Integer(), 12);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "built from imperative script");
	ASSERT_TRUE(output.confidence);
	EXPECT_DOUBLE_EQ(*output.confidence, 0.75);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallBoundedNullkillerSubroutines)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local candidates = ai:nullkillerTasks("adventure", 7)
				ai:runNullkillerTask(candidates.tasks[1].task_id)
				local step = ai:nullkillerStep({ mode = "priority", max_candidates = 2 })
				ai:pickBestArtifacts(5, 6)
				ai:nullkillerTrade()
				ai:dismissHero(17)
				ai:buildBoat(18)
				ai:dig(19)
				ai:castSpell(20, 21, 3, 4, 0)
				return {
					status = "end_turn",
					memory = {
						version = 1,
						firstTask = candidates.tasks[1].task_id,
						stepTask = step.selectedTask.task_id
					},
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-nullkiller-subroutines", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];

		const std::string type = command["payload"]["type"].String();
		if(type == "nullkiller_tasks")
		{
			response["result"]["nullkiller"]["tasks"].Vector();
			JsonNode task;
			task["task_id"] = JsonNode(41);
			response["result"]["nullkiller"]["tasks"].Vector().push_back(task);
		}
		else if(type == "nullkiller_step")
		{
			response["result"]["selectedTask"]["task_id"] = JsonNode(42);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 9);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_tasks");
	EXPECT_EQ(commands[0]["payload"]["mode"].String(), "adventure");
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 7);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_task");
	EXPECT_EQ(commands[1]["payload"]["task_id"].Integer(), 41);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[2]["payload"]["mode"].String(), "priority");
	EXPECT_EQ(commands[2]["payload"]["max_candidates"].Integer(), 2);
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "pick_best_artifacts");
	EXPECT_EQ(commands[3]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[3]["payload"]["other_hero_id"].Integer(), 6);
	EXPECT_EQ(commands[4]["payload"]["type"].String(), "nullkiller_trade");
	EXPECT_EQ(commands[5]["payload"]["type"].String(), "dismiss_hero");
	EXPECT_EQ(commands[5]["payload"]["hero_id"].Integer(), 17);
	EXPECT_EQ(commands[6]["payload"]["type"].String(), "build_boat");
	EXPECT_EQ(commands[6]["payload"]["shipyard_id"].Integer(), 18);
	EXPECT_EQ(commands[7]["payload"]["type"].String(), "dig");
	EXPECT_EQ(commands[7]["payload"]["hero_id"].Integer(), 19);
	EXPECT_EQ(commands[8]["payload"]["type"].String(), "cast_spell");
	EXPECT_EQ(commands[8]["payload"]["hero_id"].Integer(), 20);
	EXPECT_EQ(commands[8]["payload"]["spell_id"].Integer(), 21);
	EXPECT_EQ(commands[8]["payload"]["x"].Integer(), 3);
	EXPECT_EQ(commands[8]["payload"]["y"].Integer(), 4);
	EXPECT_EQ(commands[8]["payload"]["z"].Integer(), 0);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["firstTask"].Integer(), 41);
	EXPECT_EQ(output.memory["stepTask"].Integer(), 42);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanDelegateToFallback)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:setMemory({ version = 1, delegated = true })
				ai:nullkiller("delegate after opening checks")
				error("host should not resume after fallback")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-fallback", source);
	bool commandHandlerCalled = false;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode &)
	{
		commandHandlerCalled = true;
		JsonNode response;
		response["ok"] = JsonNode(true);
		return response;
	});

	EXPECT_FALSE(commandHandlerCalled);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::FALLBACK);
	EXPECT_TRUE(output.memory["delegated"].Bool());
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "delegate after opening checks");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanRefreshVisibleInput)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local refreshed = ai:refresh()
				return {
					status = "end_turn",
					memory = { version = 1, refreshedDay = refreshed.state.day },
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-refresh", source);

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		EXPECT_EQ(command["kind"].String(), "refresh");

		AI::AdventureScriptInput refreshed = makeInput();
		refreshed.state["day"] = JsonNode(2);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["input"] = refreshed.toJson();
		return response;
	});

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["refreshedDay"].Integer(), 2);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeHostErrorsAreCatchable)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local ok, err = pcall(function()
					ai:build(7, 12)
				end)
				return {
					status = "end_turn",
					memory = { version = 1, caught = not ok, error = err },
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-catch", source);

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode &)
	{
		JsonNode response;
		response["ok"] = JsonNode(false);
		response["error"] = JsonNode("invalid build");
		return response;
	});

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_TRUE(output.memory["caught"].Bool());
	EXPECT_NE(output.memory["error"].String().find("invalid build"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, UncaughtImperativeHostErrorFailsRunDay)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:build(7, 12)
				return ai:output("end_turn")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-uncaught", source);

	EXPECT_THROW(runner.runDayImperative(makeInput(), [&](const JsonNode &)
	{
		JsonNode response;
		response["ok"] = JsonNode(false);
		response["error"] = JsonNode("invalid build");
		return response;
	}), std::runtime_error);
}

TEST(LuaAdventureScriptRunnerTest, JsonPolicyFixtures)
{
	const std::filesystem::path fixtureRoot = std::filesystem::path(VCMI_SOURCE_DIR) / "test/testdata/ai/adventure-script";
	std::vector<std::filesystem::path> fixtures;

	for(const auto & entry : std::filesystem::recursive_directory_iterator(fixtureRoot))
	{
		if(entry.is_regular_file() && entry.path().extension() == ".json")
			fixtures.push_back(entry.path());
	}
	std::sort(fixtures.begin(), fixtures.end());

	ASSERT_FALSE(fixtures.empty());

	for(const std::filesystem::path & path : fixtures)
	{
		SCOPED_TRACE(path.string());
		const JsonNode fixture = readJsonFile(path);
		ASSERT_TRUE(fixture["script"].isString());

		scripting::LuaAdventureScriptRunner runner(fixture["script"].String(), readAdventureScript(fixture["script"].String()));
		const AI::AdventureScriptOutput output = runner.planDay(readFixtureInput(fixture));

		verifyFixtureExpectation(path, fixture, output);
	}
}

TEST(LuaAdventureScriptRunnerTest, BundledAdventureScriptVariantsRun)
{
	const std::vector<std::string> scripts = {
		"scripts/ai/defaultAdventure.lua",
		"scripts/ai/aggressiveAdventure.lua",
		"scripts/ai/economyAdventure.lua",
		"scripts/ai/explorerAdventure.lua",
		"scripts/ai/candidates/boundedNullkillerAdventure.lua",
		"scripts/ai/candidates/fallbackAdventure.lua"
	};

	for(const std::string & script : scripts)
	{
		scripting::LuaAdventureScriptRunner runner(script, readAdventureScript(script));
		const AI::AdventureScriptOutput output = runner.planDay(makeInput());

		if(script == "scripts/ai/defaultAdventure.lua"
			|| script == "scripts/ai/candidates/boundedNullkillerAdventure.lua"
			|| script == "scripts/ai/candidates/fallbackAdventure.lua")
		{
			EXPECT_EQ(output.status, AI::AdventureScriptStatus::FALLBACK) << script;
			EXPECT_TRUE(output.actions.empty()) << script;
		}
		else
		{
			EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN) << script;
			ASSERT_EQ(output.actions.size(), 1) << script;
			EXPECT_EQ(output.actions[0]["type"].String(), "end_turn") << script;
		}
	}
}

TEST(LuaAdventureScriptRunnerTest, PackagedConfigUsesFallbackControlScript)
{
	const JsonNode config = readJsonFile(std::filesystem::path(VCMI_SOURCE_DIR) / "config/ai/scriptedAdventure.json");

	ASSERT_TRUE(config["script"].isString());
	EXPECT_EQ(config["script"].String(), "ai/candidates/fallbackAdventure.lua");
	EXPECT_FALSE(hasField(config, "players"))
		<< "Experimental personality scripts should stay opt-in until they beat the fallback control.";
}
