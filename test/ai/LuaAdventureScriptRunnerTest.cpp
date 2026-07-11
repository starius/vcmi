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

bool matchesCommandExpectation(const JsonNode & command, const JsonNode & expected)
{
	if(hasField(expected, "kind") || hasField(expected, "payload"))
		return matchesPartialJson(command, expected);

	if(hasField(command, "payload"))
		return matchesPartialJson(command["payload"], expected);

	return matchesPartialJson(command, expected);
}

bool containsMatchingCommand(const std::vector<JsonNode> & commands, const JsonNode & expected)
{
	return std::any_of(commands.begin(), commands.end(), [&](const JsonNode & command)
	{
		return matchesCommandExpectation(command, expected);
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

JsonNode defaultHostResponse(const JsonNode & fixture, const AI::AdventureScriptInput & input, const JsonNode & command)
{
	JsonNode response;
	response["ok"] = JsonNode(true);

	if(command["kind"].String() == "refresh")
	{
		response["input"] = hasField(fixture, "refreshInput") ? fixture["refreshInput"] : input.toJson();
		return response;
	}

	response["result"]["ok"] = JsonNode(true);
	response["result"]["type"] = command["payload"]["type"];
	return response;
}

JsonNode fixtureHostResponse(const JsonNode & fixture, const AI::AdventureScriptInput & input, const JsonNode & command)
{
	if(hasField(fixture, "hostResponses"))
	{
		if(!fixture["hostResponses"].isVector())
			throw std::runtime_error("hostResponses must be an array");
		for(const JsonNode & item : fixture["hostResponses"].Vector())
		{
			const JsonNode & expectedCommand = hasField(item, "command") ? item["command"] : item["match"];
			if(!matchesCommandExpectation(command, expectedCommand))
				continue;

			if(hasField(item, "response"))
				return item["response"];

			JsonNode response = defaultHostResponse(fixture, input, command);
			if(hasField(item, "input"))
				response["input"] = item["input"];
			if(hasField(item, "result"))
				response["result"] = item["result"];
			return response;
		}
	}

	return defaultHostResponse(fixture, input, command);
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

void verifyFixtureCommandExpectation(const std::filesystem::path & path, const JsonNode & fixture, const std::vector<JsonNode> & commands)
{
	SCOPED_TRACE(path.string());
	const JsonNode & expect = fixture["expect"];

	if(hasField(expect, "firstCommand"))
	{
		ASSERT_FALSE(commands.empty());
		EXPECT_TRUE(matchesCommandExpectation(commands.front(), expect["firstCommand"]))
			<< "expected: " << expect["firstCommand"].toCompactString()
			<< "\nactual: " << commands.front().toCompactString();
	}

	if(hasField(expect, "commandsExact"))
	{
		ASSERT_TRUE(expect["commandsExact"].isVector());
		ASSERT_EQ(commands.size(), expect["commandsExact"].Vector().size());
		for(size_t index = 0; index < expect["commandsExact"].Vector().size(); ++index)
		{
			EXPECT_TRUE(matchesCommandExpectation(commands[index], expect["commandsExact"].Vector()[index]))
				<< "command index: " << index
				<< "\nexpected: " << expect["commandsExact"].Vector()[index].toCompactString()
				<< "\nactual: " << commands[index].toCompactString();
		}
	}

	if(hasField(expect, "commandsContain"))
	{
		ASSERT_TRUE(expect["commandsContain"].isVector());
		for(const JsonNode & command : expect["commandsContain"].Vector())
		{
			EXPECT_TRUE(containsMatchingCommand(commands, command)) << command.toCompactString();
		}
	}

	if(hasField(expect, "commandsDoNotContain"))
	{
		ASSERT_TRUE(expect["commandsDoNotContain"].isVector());
		for(const JsonNode & command : expect["commandsDoNotContain"].Vector())
		{
			EXPECT_FALSE(containsMatchingCommand(commands, command)) << command.toCompactString();
		}
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
				ai:visitTownBuilding(7, 13)
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

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["kind"].String(), "execute");
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "build");
	EXPECT_EQ(commands[0]["payload"]["town_id"].Integer(), 7);
	EXPECT_EQ(commands[0]["payload"]["building_id"].Integer(), 12);
	EXPECT_EQ(commands[1]["kind"].String(), "execute");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "visit_town_building");
	EXPECT_EQ(commands[1]["payload"]["town_id"].Integer(), 7);
	EXPECT_EQ(commands[1]["payload"]["building_id"].Integer(), 13);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "built from imperative script");
	ASSERT_TRUE(output.confidence);
	EXPECT_DOUBLE_EQ(*output.confidence, 0.75);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanHireHeroFromGenericSource)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:hireHero(7, 101, 102)
				ai:hireHero({ tavern_id = 55, hero_type_id = 201 })
				return ai:output("end_turn", "hired from town and tavern sources")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-hire-source", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "hire_hero");
	EXPECT_EQ(commands[0]["payload"]["source_id"].Integer(), 7);
	EXPECT_EQ(commands[0]["payload"]["town_id"].Integer(), 7);
	EXPECT_EQ(commands[0]["payload"]["hero_type_id"].Integer(), 101);
	EXPECT_EQ(commands[0]["payload"]["next_hero_type_id"].Integer(), 102);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "hire_hero");
	EXPECT_EQ(commands[1]["payload"]["tavern_id"].Integer(), 55);
	EXPECT_EQ(commands[1]["payload"]["hero_type_id"].Integer(), 201);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "hired from town and tavern sources");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeRunnerKeepsLuaStateBetweenDays)
{
	const std::string source = R"lua(
		local invocations = 0
		return {
			runDay = function(ai, input)
				invocations = invocations + 1
				return {
					status = "end_turn",
					memory = {
						version = 1,
						invocations = invocations
					},
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-persistent-state", source);
	const auto commandHandler = [](const JsonNode &)
	{
		JsonNode response;
		response["ok"] = JsonNode(true);
		return response;
	};

	const AI::AdventureScriptOutput first = runner.runDayImperative(makeInput(), commandHandler);
	const AI::AdventureScriptOutput second = runner.runDayImperative(makeInput(), commandHandler);

	EXPECT_EQ(first.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(second.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(first.memory["invocations"].Integer(), 1);
	EXPECT_EQ(second.memory["invocations"].Integer(), 2);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallBoundedNullkillerSubroutines)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local candidates = ai:nullkillerTasks("adventure", 7)
				ai:runNullkillerTask(candidates.tasks[1].task_id)
				local step = ai:nullkillerStep({ mode = "priority", max_candidates = 2, max_attempts = 3 })
				ai:pickBestArtifacts(5, 6)
				ai:swapArtifacts({ holder_id = 5, slot = 1 }, { holder_id = 6, slot = 2 })
				ai:bulkMoveArtifacts(5, 6, true, false, true)
				ai:sortBackpackArtifacts(5, "cost")
				ai:scrollBackpackArtifacts(5, false)
				ai:manageHeroCostume(5, 2, true)
				ai:nullkillerTrade()
				ai:requestStatistic()
				ai:tradeResources(16, 0, 6, 100, 5)
				ai:swapCreatures(30, 0, 31, 1)
				ai:mergeStacks(30, 2, 31, 3)
				ai:splitStack(30, 4, 31, 5, 7)
				ai:bulkSplitStack(30, 0, 2)
				ai:bulkMergeStacks(30, 1)
				ai:bulkSplitAndRebalanceStack(30, 2)
				ai:dismissCreature(30, 3)
				ai:upgradeCreature(30, 4, 37)
				ai:setFormation(17, 1)
				ai:setTactics(17, true)
				ai:swapGarrisonHero(16)
				ai:dismissHero(17)
				ai:buildBoat(18)
				ai:castleTeleport(20, 21)
				ai:dig(19)
				ai:castSpell(20, 21, 3, 4, 0)
				ai:buyArtifact(20, 0)
				ai:spellResearch(16, 31, false)
				ai:assembleArtifacts(5, 3, 141)
				ai:disassembleArtifact(5, 4)
				ai:ignoreScriptDecision(-1000)
				ai:nullkillerAnswerQuery(88, 1)
				ai:nullkillerObjectInteraction(20, 16)
				ai:mergeOrSwapStacks(30, 6, 31, 0)
				ai:setTownName(16, "Lua Keep")
				ai:eraseTransitionArtifact(5)
				ai:pickBestCreatures(31, 30)
				ai:nullkillerBuildArmy(16)
				ai:nullkillerPriorityPass(2)
				ai:nullkillerUpgradeArmy(30)
				ai:nullkillerRecruitCreatures(16, 31)
				ai:nullkillerPass({ mode = ai.nullkillerTaskModes.adventure, max_steps = 3, max_candidates = 4, max_attempts = 5 })
				ai:nullkillerAdventurePass(2, 6, 7)
				ai:nullkillerTurnSlice({ max_passes = 2, max_candidates = 8, max_attempts = 3, adventure_mode = ai.nullkillerTaskModes.defense })
				ai:nullkillerNativePass(3, 9, 4)
				ai:nullkillerNativePasses({ max_passes = 2, first_pass_index = 4, max_candidates = 10, max_attempts = 5, adventure_mode = ai.nullkillerTaskModes.escape })
				return {
					status = "end_turn",
					memory = {
						version = 1,
						firstTask = candidates.tasks[1].task_id,
						stepTask = step.selectedTask.task_id,
						executedOutcome = ai.nullkillerStepOutcomes.executed,
						defenseMode = ai.nullkillerTaskModes.defense,
						defendTier = ai.nullkillerPriorityTiers.defend,
						mainHeroRole = ai.nullkillerHeroRoles.main,
						mineKind = ai.objectKinds.mine,
						dwellingBuildingKind = ai.buildingKinds.dwelling,
						gatherTransferKind = ai.armyTransferKinds.gatherToHero,
						blockingQueryType = ai.queryTypes.blockingDialog,
						teleportBattlePathAction = ai.pathActions.teleportBattle,
						criticalThreat = ai.threatLevels.critical,
						riskyRisk = ai.riskLevels.risky,
						townPortalSpecialAction = ai.specialActionKinds.townPortal,
						dimensionDoorSpellKind = ai.adventureSpellKinds.dimensionDoor
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

	ASSERT_EQ(commands.size(), 48);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_tasks");
	EXPECT_EQ(commands[0]["payload"]["mode"].String(), "adventure");
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 7);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_task");
	EXPECT_EQ(commands[1]["payload"]["task_id"].Integer(), 41);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[2]["payload"]["mode"].String(), "priority");
	EXPECT_EQ(commands[2]["payload"]["max_candidates"].Integer(), 2);
	EXPECT_EQ(commands[2]["payload"]["max_attempts"].Integer(), 3);
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "pick_best_artifacts");
	EXPECT_EQ(commands[3]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[3]["payload"]["other_hero_id"].Integer(), 6);
	EXPECT_EQ(commands[4]["payload"]["type"].String(), "swap_artifacts");
	EXPECT_EQ(commands[4]["payload"]["src"]["holder_id"].Integer(), 5);
	EXPECT_EQ(commands[4]["payload"]["src"]["slot"].Integer(), 1);
	EXPECT_EQ(commands[4]["payload"]["dst"]["holder_id"].Integer(), 6);
	EXPECT_EQ(commands[4]["payload"]["dst"]["slot"].Integer(), 2);
	EXPECT_EQ(commands[5]["payload"]["type"].String(), "bulk_move_artifacts");
	EXPECT_EQ(commands[5]["payload"]["src_hero_id"].Integer(), 5);
	EXPECT_EQ(commands[5]["payload"]["dst_hero_id"].Integer(), 6);
	EXPECT_TRUE(commands[5]["payload"]["swap"].Bool());
	EXPECT_FALSE(commands[5]["payload"]["equipped"].Bool());
	EXPECT_TRUE(commands[5]["payload"]["backpack"].Bool());
	EXPECT_EQ(commands[6]["payload"]["type"].String(), "sort_backpack_artifacts");
	EXPECT_EQ(commands[6]["payload"]["mode"].String(), "cost");
	EXPECT_EQ(commands[7]["payload"]["type"].String(), "scroll_backpack_artifacts");
	EXPECT_FALSE(commands[7]["payload"]["left"].Bool());
	EXPECT_EQ(commands[8]["payload"]["type"].String(), "manage_hero_costume");
	EXPECT_EQ(commands[8]["payload"]["costume_index"].Integer(), 2);
	EXPECT_TRUE(commands[8]["payload"]["save"].Bool());
	EXPECT_EQ(commands[9]["payload"]["type"].String(), "nullkiller_trade");
	EXPECT_EQ(commands[10]["payload"]["type"].String(), "request_statistic");
	EXPECT_EQ(commands[11]["payload"]["type"].String(), "trade_resources");
	EXPECT_EQ(commands[11]["payload"]["market_id"].Integer(), 16);
	EXPECT_EQ(commands[11]["payload"]["sell_resource_id"].Integer(), 0);
	EXPECT_EQ(commands[11]["payload"]["buy_resource_id"].Integer(), 6);
	EXPECT_EQ(commands[11]["payload"]["amount"].Integer(), 100);
	EXPECT_EQ(commands[11]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[12]["payload"]["type"].String(), "swap_creatures");
	EXPECT_EQ(commands[12]["payload"]["source_id"].Integer(), 30);
	EXPECT_EQ(commands[12]["payload"]["source_slot"].Integer(), 0);
	EXPECT_EQ(commands[12]["payload"]["destination_id"].Integer(), 31);
	EXPECT_EQ(commands[12]["payload"]["destination_slot"].Integer(), 1);
	EXPECT_EQ(commands[13]["payload"]["type"].String(), "merge_stacks");
	EXPECT_EQ(commands[13]["payload"]["source_slot"].Integer(), 2);
	EXPECT_EQ(commands[13]["payload"]["destination_slot"].Integer(), 3);
	EXPECT_EQ(commands[14]["payload"]["type"].String(), "split_stack");
	EXPECT_EQ(commands[14]["payload"]["source_slot"].Integer(), 4);
	EXPECT_EQ(commands[14]["payload"]["destination_slot"].Integer(), 5);
	EXPECT_EQ(commands[14]["payload"]["amount"].Integer(), 7);
	EXPECT_EQ(commands[15]["payload"]["type"].String(), "bulk_split_stack");
	EXPECT_EQ(commands[15]["payload"]["army_id"].Integer(), 30);
	EXPECT_EQ(commands[15]["payload"]["source_slot"].Integer(), 0);
	EXPECT_EQ(commands[15]["payload"]["amount"].Integer(), 2);
	EXPECT_EQ(commands[16]["payload"]["type"].String(), "bulk_merge_stacks");
	EXPECT_EQ(commands[16]["payload"]["army_id"].Integer(), 30);
	EXPECT_EQ(commands[16]["payload"]["source_slot"].Integer(), 1);
	EXPECT_EQ(commands[17]["payload"]["type"].String(), "bulk_split_rebalance_stack");
	EXPECT_EQ(commands[17]["payload"]["army_id"].Integer(), 30);
	EXPECT_EQ(commands[17]["payload"]["source_slot"].Integer(), 2);
	EXPECT_EQ(commands[18]["payload"]["type"].String(), "dismiss_creature");
	EXPECT_EQ(commands[18]["payload"]["army_id"].Integer(), 30);
	EXPECT_EQ(commands[18]["payload"]["slot"].Integer(), 3);
	EXPECT_EQ(commands[19]["payload"]["type"].String(), "upgrade_creature");
	EXPECT_EQ(commands[19]["payload"]["army_id"].Integer(), 30);
	EXPECT_EQ(commands[19]["payload"]["slot"].Integer(), 4);
	EXPECT_EQ(commands[19]["payload"]["creature_id"].Integer(), 37);
	EXPECT_EQ(commands[20]["payload"]["type"].String(), "set_formation");
	EXPECT_EQ(commands[20]["payload"]["hero_id"].Integer(), 17);
	EXPECT_EQ(commands[20]["payload"]["formation_id"].Integer(), 1);
	EXPECT_EQ(commands[21]["payload"]["type"].String(), "set_tactics");
	EXPECT_EQ(commands[21]["payload"]["hero_id"].Integer(), 17);
	EXPECT_TRUE(commands[21]["payload"]["enabled"].Bool());
	EXPECT_EQ(commands[22]["payload"]["type"].String(), "swap_garrison_hero");
	EXPECT_EQ(commands[22]["payload"]["town_id"].Integer(), 16);
	EXPECT_EQ(commands[23]["payload"]["type"].String(), "dismiss_hero");
	EXPECT_EQ(commands[23]["payload"]["hero_id"].Integer(), 17);
	EXPECT_EQ(commands[24]["payload"]["type"].String(), "build_boat");
	EXPECT_EQ(commands[24]["payload"]["shipyard_id"].Integer(), 18);
	EXPECT_EQ(commands[25]["payload"]["type"].String(), "castle_teleport");
	EXPECT_EQ(commands[25]["payload"]["hero_id"].Integer(), 20);
	EXPECT_EQ(commands[25]["payload"]["destination_town_id"].Integer(), 21);
	EXPECT_EQ(commands[26]["payload"]["type"].String(), "dig");
	EXPECT_EQ(commands[26]["payload"]["hero_id"].Integer(), 19);
	EXPECT_EQ(commands[27]["payload"]["type"].String(), "cast_spell");
	EXPECT_EQ(commands[27]["payload"]["hero_id"].Integer(), 20);
	EXPECT_EQ(commands[27]["payload"]["spell_id"].Integer(), 21);
	EXPECT_EQ(commands[27]["payload"]["x"].Integer(), 3);
	EXPECT_EQ(commands[27]["payload"]["y"].Integer(), 4);
	EXPECT_EQ(commands[27]["payload"]["z"].Integer(), 0);
	EXPECT_EQ(commands[28]["payload"]["type"].String(), "buy_artifact");
	EXPECT_EQ(commands[28]["payload"]["hero_id"].Integer(), 20);
	EXPECT_EQ(commands[28]["payload"]["artifact_id"].Integer(), 0);
	EXPECT_EQ(commands[29]["payload"]["type"].String(), "spell_research");
	EXPECT_EQ(commands[29]["payload"]["town_id"].Integer(), 16);
	EXPECT_EQ(commands[29]["payload"]["spell_id"].Integer(), 31);
	EXPECT_FALSE(commands[29]["payload"]["accept"].Bool());
	EXPECT_EQ(commands[30]["payload"]["type"].String(), "assemble_artifacts");
	EXPECT_EQ(commands[30]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[30]["payload"]["slot"].Integer(), 3);
	EXPECT_TRUE(commands[30]["payload"]["assemble"].Bool());
	EXPECT_EQ(commands[30]["payload"]["artifact_id"].Integer(), 141);
	EXPECT_EQ(commands[31]["payload"]["type"].String(), "assemble_artifacts");
	EXPECT_EQ(commands[31]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[31]["payload"]["slot"].Integer(), 4);
	EXPECT_FALSE(commands[31]["payload"]["assemble"].Bool());
	EXPECT_EQ(commands[32]["payload"]["type"].String(), "ignore_script_query");
	EXPECT_EQ(commands[32]["payload"]["query_id"].Integer(), -1000);
	EXPECT_EQ(commands[33]["payload"]["type"].String(), "nullkiller_answer_query");
	EXPECT_EQ(commands[33]["payload"]["query_id"].Integer(), 88);
	EXPECT_EQ(commands[33]["payload"]["default_answer"].Integer(), 1);
	EXPECT_EQ(commands[34]["payload"]["type"].String(), "nullkiller_object_interaction");
	EXPECT_EQ(commands[34]["payload"]["hero_id"].Integer(), 20);
	EXPECT_EQ(commands[34]["payload"]["object_id"].Integer(), 16);
	EXPECT_EQ(commands[35]["payload"]["type"].String(), "merge_or_swap_stacks");
	EXPECT_EQ(commands[35]["payload"]["source_id"].Integer(), 30);
	EXPECT_EQ(commands[35]["payload"]["source_slot"].Integer(), 6);
	EXPECT_EQ(commands[35]["payload"]["destination_id"].Integer(), 31);
	EXPECT_EQ(commands[35]["payload"]["destination_slot"].Integer(), 0);
	EXPECT_EQ(commands[36]["payload"]["type"].String(), "set_town_name");
	EXPECT_EQ(commands[36]["payload"]["town_id"].Integer(), 16);
	EXPECT_EQ(commands[36]["payload"]["name"].String(), "Lua Keep");
	EXPECT_EQ(commands[37]["payload"]["type"].String(), "erase_transition_artifact");
	EXPECT_EQ(commands[37]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[38]["payload"]["type"].String(), "pick_best_creatures");
	EXPECT_EQ(commands[38]["payload"]["destination_id"].Integer(), 31);
	EXPECT_EQ(commands[38]["payload"]["source_id"].Integer(), 30);
	EXPECT_EQ(commands[39]["payload"]["type"].String(), "nullkiller_build_army");
	EXPECT_EQ(commands[39]["payload"]["town_id"].Integer(), 16);
	EXPECT_EQ(commands[40]["payload"]["type"].String(), "nullkiller_priority_pass");
	EXPECT_EQ(commands[40]["payload"]["pass_index"].Integer(), 2);
	EXPECT_EQ(commands[41]["payload"]["type"].String(), "nullkiller_upgrade_army");
	EXPECT_EQ(commands[41]["payload"]["army_id"].Integer(), 30);
	EXPECT_EQ(commands[42]["payload"]["type"].String(), "nullkiller_recruit_creatures");
	EXPECT_EQ(commands[42]["payload"]["source_id"].Integer(), 16);
	EXPECT_EQ(commands[42]["payload"]["destination_id"].Integer(), 31);
	EXPECT_EQ(commands[43]["payload"]["type"].String(), "nullkiller_pass");
	EXPECT_EQ(commands[43]["payload"]["mode"].Integer(), 1);
	EXPECT_EQ(commands[43]["payload"]["max_steps"].Integer(), 3);
	EXPECT_EQ(commands[43]["payload"]["max_candidates"].Integer(), 4);
	EXPECT_EQ(commands[43]["payload"]["max_attempts"].Integer(), 5);
	EXPECT_EQ(commands[44]["payload"]["type"].String(), "nullkiller_pass");
	EXPECT_EQ(commands[44]["payload"]["mode"].Integer(), 1);
	EXPECT_EQ(commands[44]["payload"]["max_steps"].Integer(), 2);
	EXPECT_EQ(commands[44]["payload"]["max_candidates"].Integer(), 6);
	EXPECT_EQ(commands[44]["payload"]["max_attempts"].Integer(), 7);
	EXPECT_EQ(commands[45]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[45]["payload"]["max_passes"].Integer(), 2);
	EXPECT_EQ(commands[45]["payload"]["max_candidates"].Integer(), 8);
	EXPECT_EQ(commands[45]["payload"]["max_attempts"].Integer(), 3);
	EXPECT_EQ(commands[45]["payload"]["adventure_mode"].Integer(), 8);
	EXPECT_EQ(commands[46]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[46]["payload"]["first_pass_index"].Integer(), 3);
	EXPECT_EQ(commands[46]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[46]["payload"]["max_candidates"].Integer(), 9);
	EXPECT_EQ(commands[46]["payload"]["max_attempts"].Integer(), 4);
	EXPECT_TRUE(commands[46]["payload"]["include_priority"].Bool());
	EXPECT_TRUE(commands[46]["payload"]["include_adventure"].Bool());
	EXPECT_TRUE(commands[46]["payload"]["include_trade"].Bool());
	EXPECT_TRUE(commands[46]["payload"]["optimize_artifacts"].Bool());
	EXPECT_EQ(commands[47]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[47]["payload"]["first_pass_index"].Integer(), 4);
	EXPECT_EQ(commands[47]["payload"]["max_passes"].Integer(), 2);
	EXPECT_EQ(commands[47]["payload"]["max_candidates"].Integer(), 10);
	EXPECT_EQ(commands[47]["payload"]["max_attempts"].Integer(), 5);
	EXPECT_EQ(commands[47]["payload"]["adventure_mode"].Integer(), 9);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["firstTask"].Integer(), 41);
	EXPECT_EQ(output.memory["stepTask"].Integer(), 42);
	EXPECT_EQ(output.memory["executedOutcome"].Integer(), 1);
	EXPECT_EQ(output.memory["defenseMode"].Integer(), 8);
	EXPECT_EQ(output.memory["defendTier"].Integer(), 6);
	EXPECT_EQ(output.memory["mainHeroRole"].Integer(), 1);
	EXPECT_EQ(output.memory["mineKind"].Integer(), 3);
	EXPECT_EQ(output.memory["dwellingBuildingKind"].Integer(), 11);
	EXPECT_EQ(output.memory["gatherTransferKind"].Integer(), 1);
	EXPECT_EQ(output.memory["blockingQueryType"].Integer(), 3);
	EXPECT_EQ(output.memory["teleportBattlePathAction"].Integer(), 9);
	EXPECT_EQ(output.memory["criticalThreat"].Integer(), 3);
	EXPECT_EQ(output.memory["riskyRisk"].Integer(), 2);
	EXPECT_EQ(output.memory["townPortalSpecialAction"].Integer(), 3);
	EXPECT_EQ(output.memory["dimensionDoorSpellKind"].Integer(), 2);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanInspectNullkillerTaskCandidates)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local candidates = ai:getNullkillerTaskCandidates(ai.nullkillerTaskModes.defense, 9)
				ai:runNullkillerTask(candidates.tasks[1].task_id)
				return ai:output("end_turn", "inspected and ran one Nullkiller task")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-nullkiller-task-inspect", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		if(command["kind"].String() == "inspect")
		{
			response["result"]["modeId"] = command["payload"]["mode"];
			response["result"]["mode"] = JsonNode("defense");
			response["result"]["tasks"].Vector();
			JsonNode task;
			task["task_id"] = JsonNode(51);
			response["result"]["tasks"].Vector().push_back(task);
			response["result"]["count"] = JsonNode(1);
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["kind"].String(), "inspect");
	EXPECT_EQ(commands[0]["payload"]["what"].String(), "nullkiller_tasks");
	EXPECT_EQ(commands[0]["payload"]["mode"].Integer(), 8);
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 9);
	EXPECT_EQ(commands[1]["kind"].String(), "execute");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_task");
	EXPECT_EQ(commands[1]["payload"]["task_id"].Integer(), 51);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "inspected and ran one Nullkiller task");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallNamedNullkillerModeHelpers)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerDefenseTasks(9)
				ai:nullkillerEscapeStep(4, 2)
				ai:nullkillerBuildingStep(3, 1)
				ai:nullkillerStartupTasks(5)
				ai:nullkillerStartupStep(6, 7)
				ai:nullkillerGatherArmyPass(2, 8, 3)
				return ai:output("end_turn", "named bounded helpers")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:named-nullkiller-mode-helpers", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_tasks")
			response["result"]["nullkiller"]["tasks"].Vector();
		return response;
	});

	ASSERT_EQ(commands.size(), 6);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_tasks");
	EXPECT_EQ(commands[0]["payload"]["mode"].Integer(), 8);
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 9);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[1]["payload"]["mode"].Integer(), 9);
	EXPECT_EQ(commands[1]["payload"]["max_candidates"].Integer(), 4);
	EXPECT_EQ(commands[1]["payload"]["max_attempts"].Integer(), 2);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[2]["payload"]["mode"].Integer(), 5);
	EXPECT_EQ(commands[2]["payload"]["max_candidates"].Integer(), 3);
	EXPECT_EQ(commands[2]["payload"]["max_attempts"].Integer(), 1);
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "nullkiller_tasks");
	EXPECT_EQ(commands[3]["payload"]["mode"].Integer(), 12);
	EXPECT_EQ(commands[3]["payload"]["max_candidates"].Integer(), 5);
	EXPECT_EQ(commands[4]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[4]["payload"]["mode"].Integer(), 12);
	EXPECT_EQ(commands[4]["payload"]["max_candidates"].Integer(), 6);
	EXPECT_EQ(commands[4]["payload"]["max_attempts"].Integer(), 7);
	EXPECT_EQ(commands[5]["payload"]["type"].String(), "nullkiller_pass");
	EXPECT_EQ(commands[5]["payload"]["mode"].Integer(), 10);
	EXPECT_EQ(commands[5]["payload"]["max_steps"].Integer(), 2);
	EXPECT_EQ(commands[5]["payload"]["max_candidates"].Integer(), 8);
	EXPECT_EQ(commands[5]["payload"]["max_attempts"].Integer(), 3);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "named bounded helpers");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallBulkMoveArmy)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:bulkMoveArmy(30, 31, 2)
				ai:bulkMoveArmy({ source_id = 32, destination_id = 33, source_slot = 4 })
				return ai:output("end_turn", "bulk army moves")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:bulk-move-army", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "bulk_move_army");
	EXPECT_EQ(commands[0]["payload"]["source_id"].Integer(), 30);
	EXPECT_EQ(commands[0]["payload"]["destination_id"].Integer(), 31);
	EXPECT_EQ(commands[0]["payload"]["source_slot"].Integer(), 2);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "bulk_move_army");
	EXPECT_EQ(commands[1]["payload"]["source_id"].Integer(), 32);
	EXPECT_EQ(commands[1]["payload"]["destination_id"].Integer(), 33);
	EXPECT_EQ(commands[1]["payload"]["source_slot"].Integer(), 4);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(*output.intent, "bulk army moves");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallNullkillerArmyFormationHelpers)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerAddSingleCreatureStacks(17)
				ai:nullkillerRearrangeForWhirlpool(18)
				ai:nullkillerRearrangeForSiege(19, 20)
				return ai:output("end_turn", "native army formation helpers")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:nullkiller-army-formation-helpers", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 3);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_add_single_creature_stacks");
	EXPECT_EQ(commands[0]["payload"]["hero_id"].Integer(), 17);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_rearrange_for_whirlpool");
	EXPECT_EQ(commands[1]["payload"]["hero_id"].Integer(), 18);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_rearrange_for_siege");
	EXPECT_EQ(commands[2]["payload"]["hero_id"].Integer(), 19);
	EXPECT_EQ(commands[2]["payload"]["town_id"].Integer(), 20);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallNullkillerTownCreaturePickup)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerMoveCreaturesToHero(16)
				return ai:output("end_turn", "native town creature pickup")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:nullkiller-town-creature-pickup", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 1);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_move_creatures_to_hero");
	EXPECT_EQ(commands[0]["payload"]["town_id"].Integer(), 16);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallNullkillerWeakHeroDismissal)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerDismissWeakHero()
				ai:nullkillerDismissWeakHero({ require_cap_reached = false, army_limit = 500, town_to_spare_id = 16 })
				return ai:output("end_turn", "native weak hero dismissal")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:nullkiller-weak-hero-dismissal", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_dismiss_weak_hero");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_dismiss_weak_hero");
	EXPECT_FALSE(commands[1]["payload"]["require_cap_reached"].Bool());
	EXPECT_EQ(commands[1]["payload"]["army_limit"].Integer(), 500);
	EXPECT_EQ(commands[1]["payload"]["town_to_spare_id"].Integer(), 16);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallNullkillerArtifactOptimization)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerOptimizeArtifacts()
				ai:nullkillerOptimizeArtifacts(17)
				ai:nullkillerOptimizeArtifacts({ hero_id = 18 })
				return ai:output("end_turn", "native artifact optimization")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:nullkiller-artifact-optimization", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 3);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_optimize_artifacts");
	EXPECT_FALSE(hasField(commands[0]["payload"], "hero_id"));
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_optimize_artifacts");
	EXPECT_EQ(commands[1]["payload"]["hero_id"].Integer(), 17);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_optimize_artifacts");
	EXPECT_EQ(commands[2]["payload"]["hero_id"].Integer(), 18);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanConstrainNullkillerPlanner)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerLockResources({ 0, 0, 0, 0, 0, 0, 2500 })
				ai:nullkillerLockResources({
					resource_entries = {
						{ resource_id = ai.resourceIds.gold, amount = 500 },
						{ resource_id = ai.resourceIds.wood, amount = 5 }
					}
				})
				ai:nullkillerLockHero(17)
				ai:nullkillerLockHero({ hero_id = 18, reason_id = ai.nullkillerHeroLockReasons.heroChain })
				ai:nullkillerUnlockHero(17)
				ai:nullkillerReset()
				return ai:output("end_turn", "constrained native planner")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:nullkiller-planner-constraints", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 6);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_lock_resources");
	ASSERT_TRUE(commands[0]["payload"]["resources"].isVector());
	ASSERT_EQ(commands[0]["payload"]["resources"].Vector().size(), 7);
	EXPECT_EQ(commands[0]["payload"]["resources"].Vector()[6].Integer(), 2500);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_lock_resources");
	ASSERT_TRUE(commands[1]["payload"]["resource_entries"].isVector());
	EXPECT_EQ(commands[1]["payload"]["resource_entries"].Vector()[0]["resource_id"].Integer(), 6);
	EXPECT_EQ(commands[1]["payload"]["resource_entries"].Vector()[0]["amount"].Integer(), 500);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_lock_hero");
	EXPECT_EQ(commands[2]["payload"]["hero_id"].Integer(), 17);
	EXPECT_EQ(commands[2]["payload"]["reason_id"].Integer(), 2);
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "nullkiller_lock_hero");
	EXPECT_EQ(commands[3]["payload"]["hero_id"].Integer(), 18);
	EXPECT_EQ(commands[3]["payload"]["reason_id"].Integer(), 3);
	EXPECT_EQ(commands[4]["payload"]["type"].String(), "nullkiller_unlock_hero");
	EXPECT_EQ(commands[4]["payload"]["hero_id"].Integer(), 17);
	EXPECT_EQ(commands[5]["payload"]["type"].String(), "nullkiller_reset");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanUseNumericActionTypeIds)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerReset()
				ai:runAction({
					type_id = ai.actionTypeIds.nullkillerTurnSlice,
					max_passes = 1
				})
				ai:runAction({
					type_id = ai.actionTypeIds.endTurn,
					typeId = ai.queryTypes.blockingDialog
				})
				return ai:output("end_turn", "used numeric action ids")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:numeric-action-type-ids", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		if(hasField(command["payload"], "type"))
			response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 3);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_reset");
	EXPECT_EQ(commands[0]["payload"]["type_id"].Integer(), 100);
	EXPECT_FALSE(hasField(commands[1]["payload"], "type"));
	EXPECT_EQ(commands[1]["payload"]["type_id"].Integer(), 106);
	EXPECT_FALSE(hasField(commands[2]["payload"], "type"));
	EXPECT_EQ(commands[2]["payload"]["type_id"].Integer(), 9);
	EXPECT_EQ(commands[2]["payload"]["typeId"].Integer(), 3);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "used numeric action ids");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayUsesHostAdvertisedActionTypeIds)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:nullkillerReset()
				ai:refresh()
				ai:nullkillerReset()
				return ai:output("end_turn", "used host action ids")
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	input.actionSpace["acceptedActions"].Vector();
	JsonNode firstAction;
	firstAction["type"] = JsonNode("nullkiller_reset");
	firstAction["typeId"] = JsonNode(9001);
	input.actionSpace["acceptedActions"].Vector().push_back(firstAction);

	AI::AdventureScriptInput refreshedInput = input;
	refreshedInput.actionSpace["acceptedActions"].Vector().clear();
	JsonNode refreshedAction;
	refreshedAction["type"] = JsonNode("nullkiller_reset");
	refreshedAction["typeId"] = JsonNode(9002);
	refreshedInput.actionSpace["acceptedActions"].Vector().push_back(refreshedAction);

	scripting::LuaAdventureScriptRunner runner("test:host-action-type-ids", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		if(command["kind"].String() == "refresh")
		{
			response["input"] = refreshedInput.toJson();
		}
		else
		{
			response["result"]["ok"] = JsonNode(true);
			response["result"]["type"] = command["payload"]["type"];
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 3);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_reset");
	EXPECT_EQ(commands[0]["payload"]["type_id"].Integer(), 9001);
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_reset");
	EXPECT_EQ(commands[2]["payload"]["type_id"].Integer(), 9002);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "used host action ids");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeFacadeCoversAdvertisedActionTypeIds)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				for _, action in ipairs(input.actionSpace.acceptedActions or {}) do
					ai:runAction({ type = action.type })
				end
				return {
					status = "end_turn",
					memory = {
						version = 1,
						checkedActions = #(input.actionSpace.acceptedActions or {})
					},
					actions = {}
				}
			end
		}
	)lua";

	const std::vector<std::string> advertisedActions = {
		"build",
		"recruit",
		"hire_hero",
		"transfer_army",
		"move_hero",
		"visit_object",
		"answer_query",
		"cancel_query",
		"end_turn",
		"pick_best_creatures",
		"pick_best_artifacts",
		"prepare_hero",
		"swap_artifacts",
		"bulk_move_artifacts",
		"sort_backpack_artifacts",
		"scroll_backpack_artifacts",
		"manage_hero_costume",
		"assemble_artifacts",
		"ignore_script_query",
		"erase_transition_artifact",
		"swap_creatures",
		"merge_stacks",
		"merge_or_swap_stacks",
		"split_stack",
		"bulk_move_army",
		"bulk_split_stack",
		"bulk_merge_stacks",
		"bulk_split_rebalance_stack",
		"dismiss_creature",
		"upgrade_creature",
		"set_formation",
		"set_tactics",
		"set_town_name",
		"swap_garrison_hero",
		"trade_resources",
		"market_trade",
		"request_statistic",
		"dismiss_hero",
		"build_boat",
		"castle_teleport",
		"dig",
		"cast_spell",
		"buy_artifact",
		"spell_research",
		"visit_town_building",
		"nullkiller_reset",
		"nullkiller_lock_resources",
		"nullkiller_lock_hero",
		"nullkiller_unlock_hero",
		"nullkiller_trade",
		"nullkiller_priority_pass",
		"nullkiller_turn_slice",
		"nullkiller_build_army",
		"nullkiller_upgrade_army",
		"nullkiller_recruit_creatures",
		"nullkiller_move_creatures_to_hero",
		"nullkiller_dismiss_weak_hero",
		"nullkiller_optimize_artifacts",
		"nullkiller_add_single_creature_stacks",
		"nullkiller_rearrange_for_whirlpool",
		"nullkiller_rearrange_for_siege",
		"nullkiller_tasks",
		"nullkiller_task",
		"nullkiller_step",
		"nullkiller_pass",
		"nullkiller_answer_query",
		"nullkiller_object_interaction"
	};

	AI::AdventureScriptInput input = makeInput();
	input.actionSpace["acceptedActions"].Vector();
	for(const std::string & type : advertisedActions)
	{
		JsonNode action;
		action["type"] = JsonNode(type);
		input.actionSpace["acceptedActions"].Vector().push_back(action);
	}

	AI::AdventureScriptLimits limits;
	limits.maxActions = advertisedActions.size();
	scripting::LuaAdventureScriptRunner runner("test:imperative-action-type-coverage", source, limits);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), advertisedActions.size());
	EXPECT_EQ(output.memory["checkedActions"].Integer(), static_cast<int64_t>(advertisedActions.size()));
	for(size_t index = 0; index < commands.size(); ++index)
	{
		const JsonNode & payload = commands[index]["payload"];
		EXPECT_EQ(payload["type"].String(), advertisedActions[index]) << index;
		ASSERT_TRUE(hasField(payload, "type_id")) << advertisedActions[index];
		EXPECT_GT(payload["type_id"].Integer(), 0) << advertisedActions[index];
	}
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanExecuteActionSpaceOptions)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local option = input.actionSpace.nullkillerSubroutineOptions[1]
				local helper = input.actionSpace.nullkillerHelperOptions[1]
				local artifactHelper = input.actionSpace.nullkillerHelperOptions[2]
				local turnSlice = input.actionSpace.nullkillerHelperOptions[3]
				ai:runOption(option)
				ai:runOption(option, "passAction")
				ai:runOption(helper)
				ai:runOption(artifactHelper)
				ai:runOption(turnSlice)
				ai:runAction({ type = "nullkiller_tasks", mode = ai.nullkillerTaskModes.defense, max_candidates = 5 })
				return ai:output("end_turn", "executed action-space options")
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	JsonNode subroutine;
	subroutine["modeId"] = JsonNode(8);
	subroutine["planAction"]["type"] = JsonNode("nullkiller_step");
	subroutine["planAction"]["mode"] = JsonNode(8);
	subroutine["planAction"]["max_candidates"] = JsonNode(16);
	subroutine["planAction"]["max_attempts"] = JsonNode(16);
	subroutine["passAction"]["type"] = JsonNode("nullkiller_pass");
	subroutine["passAction"]["mode"] = JsonNode(8);
	subroutine["passAction"]["max_steps"] = JsonNode(4);
	subroutine["passAction"]["max_candidates"] = JsonNode(16);
	subroutine["passAction"]["max_attempts"] = JsonNode(16);
	input.actionSpace["nullkillerSubroutineOptions"].Vector().push_back(subroutine);

	JsonNode helper;
	helper["planAction"]["type"] = JsonNode("nullkiller_priority_pass");
	helper["planAction"]["pass_index"] = JsonNode(1);
	input.actionSpace["nullkillerHelperOptions"].Vector().push_back(helper);

	JsonNode artifactHelper;
	artifactHelper["planAction"]["type"] = JsonNode("nullkiller_optimize_artifacts");
	input.actionSpace["nullkillerHelperOptions"].Vector().push_back(artifactHelper);

	JsonNode turnSliceHelper;
	turnSliceHelper["planAction"]["type"] = JsonNode("nullkiller_turn_slice");
	turnSliceHelper["planAction"]["max_passes"] = JsonNode(1);
	turnSliceHelper["planAction"]["max_candidates"] = JsonNode(16);
	turnSliceHelper["planAction"]["max_attempts"] = JsonNode(16);
	input.actionSpace["nullkillerHelperOptions"].Vector().push_back(turnSliceHelper);

	scripting::LuaAdventureScriptRunner runner("test:imperative-action-space-options", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_tasks")
			response["result"]["nullkiller"]["tasks"].Vector();
		return response;
	});

	ASSERT_EQ(commands.size(), 6);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[0]["payload"]["mode"].Integer(), 8);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_pass");
	EXPECT_EQ(commands[1]["payload"]["mode"].Integer(), 8);
	EXPECT_EQ(commands[1]["payload"]["max_steps"].Integer(), 4);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_priority_pass");
	EXPECT_EQ(commands[2]["payload"]["pass_index"].Integer(), 1);
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "nullkiller_optimize_artifacts");
	EXPECT_EQ(commands[4]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[4]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[5]["payload"]["type"].String(), "nullkiller_tasks");
	EXPECT_EQ(commands[5]["payload"]["mode"].Integer(), 8);
	EXPECT_EQ(commands[5]["payload"]["max_candidates"].Integer(), 5);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "executed action-space options");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanSelectAndRunOneNullkillerCandidate)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local executed = ai:runBestNullkillerTask(ai.nullkillerTaskModes.all, function(task)
					return task.goalTypeId == 8
				end, 5)
				local skipped = ai:runBestNullkillerTask({
					mode = ai.nullkillerTaskModes.capture,
					max_candidates = 4,
					predicate = function(task)
						return task.goalTypeId == 99
					end
				})
				return {
					status = "end_turn",
					memory = {
						version = 1,
						executedTaskId = executed.task_id,
						selectedTaskId = executed.selectedTask.task_id,
						selectedTaskIndex = executed.selectedTaskIndex,
						candidateCount = executed.candidateCount,
						skippedExecuted = skipped.executed,
						skippedReason = skipped.reason,
						skippedCandidateCount = skipped.candidateCount
					},
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-select-nullkiller-candidate", source);
	std::vector<JsonNode> commands;
	int inspectCount = 0;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		if(command["kind"].String() == "inspect")
		{
			++inspectCount;
			response["result"]["tasks"].Vector();
			if(inspectCount == 1)
			{
				JsonNode first;
				first["task_id"] = JsonNode(10);
				first["goalTypeId"] = JsonNode(1);
				response["result"]["tasks"].Vector().push_back(first);

				JsonNode second;
				second["task_id"] = JsonNode(11);
				second["goalTypeId"] = JsonNode(8);
				response["result"]["tasks"].Vector().push_back(second);
			}
			else
			{
				JsonNode rejected;
				rejected["task_id"] = JsonNode(12);
				rejected["goalTypeId"] = JsonNode(1);
				response["result"]["tasks"].Vector().push_back(rejected);
			}
		}
		else
		{
			response["result"]["ok"] = JsonNode(true);
			response["result"]["executed"] = JsonNode(true);
			response["result"]["task_id"] = command["payload"]["task_id"];
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 3);
	EXPECT_EQ(commands[0]["kind"].String(), "inspect");
	EXPECT_EQ(commands[0]["payload"]["what"].String(), "nullkiller_tasks");
	EXPECT_EQ(commands[0]["payload"]["mode"].Integer(), 2);
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 5);
	EXPECT_EQ(commands[1]["kind"].String(), "execute");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_task");
	EXPECT_EQ(commands[1]["payload"]["task_id"].Integer(), 11);
	EXPECT_EQ(commands[2]["kind"].String(), "inspect");
	EXPECT_EQ(commands[2]["payload"]["mode"].Integer(), 6);
	EXPECT_EQ(commands[2]["payload"]["max_candidates"].Integer(), 4);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["executedTaskId"].Integer(), 11);
	EXPECT_EQ(output.memory["selectedTaskId"].Integer(), 11);
	EXPECT_EQ(output.memory["selectedTaskIndex"].Integer(), 2);
	EXPECT_EQ(output.memory["candidateCount"].Integer(), 2);
	EXPECT_FALSE(output.memory["skippedExecuted"].Bool());
	EXPECT_EQ(output.memory["skippedReason"].String(), "no_matching_task");
	EXPECT_EQ(output.memory["skippedCandidateCount"].Integer(), 1);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanRunBoundedNullkillerDayOnePassAtATime)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local result = ai:nullkillerBoundedDay({
					max_passes = 3,
					max_candidates = 9,
					max_attempts = 8,
					max_queries_per_pass = 1,
					default_answer = 2
				})
				ai:setMemory({
					version = 1,
					status = result.status,
					shouldEndTurn = result.shouldEndTurn,
					passCount = result.passCount,
					answeredQueries = result.answeredQueries,
					refreshes = result.refreshes,
					priorityTasksExecuted = result.priorityTasksExecuted
				})
				return ai:output(result.shouldEndTurn and "end_turn" or "continue", result.status)
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	JsonNode query;
	query["query_id"] = JsonNode(77);
	query["typeId"] = JsonNode(3);
	query["type"] = JsonNode("blocking_dialog");
	input.state["turn"]["queries"].Vector().push_back(query);

	AI::AdventureScriptInput refreshed = input;
	refreshed.state["turn"]["queries"].Vector().clear();

	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-day-helper", source);
	std::vector<JsonNode> commands;
	int slices = 0;

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		if(command["kind"].String() == "refresh")
		{
			response["input"] = refreshed.toJson();
			return response;
		}

		const std::string type = command["payload"]["type"].String();
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(type == "nullkiller_turn_slice")
		{
			++slices;
			response["result"]["didWork"] = JsonNode(slices == 1);
			response["result"]["priorityTasksExecuted"] = JsonNode(slices == 1 ? 1 : 0);
			response["result"]["adventureStepsExecuted"] = JsonNode(0);
			response["result"]["adventureReplanSteps"] = JsonNode(0);
			response["result"]["adventureStopTurnSteps"] = JsonNode(0);
			response["result"]["adventureExhaustedSteps"] = JsonNode(0);
			response["result"]["tradePasses"] = JsonNode(0);
			response["result"]["artifactCleanupPasses"] = JsonNode(0);
			response["result"]["shouldStopTurn"] = JsonNode(slices == 2);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
			response["result"]["paused"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 5);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_answer_query");
	EXPECT_EQ(commands[0]["payload"]["query_id"].Integer(), 77);
	EXPECT_EQ(commands[0]["payload"]["default_answer"].Integer(), 2);
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[2]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[2]["payload"]["first_pass_index"].Integer(), 1);
	EXPECT_EQ(commands[2]["payload"]["max_candidates"].Integer(), 9);
	EXPECT_EQ(commands[2]["payload"]["max_attempts"].Integer(), 8);
	EXPECT_EQ(commands[3]["kind"].String(), "refresh");
	EXPECT_EQ(commands[4]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[4]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[4]["payload"]["first_pass_index"].Integer(), 2);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "stop_turn");
	EXPECT_EQ(output.memory["status"].String(), "stop_turn");
	EXPECT_TRUE(output.memory["shouldEndTurn"].Bool());
	EXPECT_EQ(output.memory["passCount"].Integer(), 2);
	EXPECT_EQ(output.memory["answeredQueries"].Integer(), 1);
	EXPECT_EQ(output.memory["refreshes"].Integer(), 1);
	EXPECT_EQ(output.memory["priorityTasksExecuted"].Integer(), 1);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanPrepareHero)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:prepareHero(5, 16, 6)
				ai:prepareHero({ hero_id = 7, include_artifacts = true, include_creatures = false })
				return ai:output("end_turn", "prepared heroes")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:prepare-hero-helper", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "prepare_hero");
	EXPECT_EQ(commands[0]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[0]["payload"]["source_id"].Integer(), 16);
	EXPECT_EQ(commands[0]["payload"]["other_hero_id"].Integer(), 6);
	EXPECT_TRUE(commands[0]["payload"]["include_artifacts"].Bool());
	EXPECT_TRUE(commands[0]["payload"]["include_creatures"].Bool());
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "prepare_hero");
	EXPECT_EQ(commands[1]["payload"]["hero_id"].Integer(), 7);
	EXPECT_TRUE(commands[1]["payload"]["include_artifacts"].Bool());
	EXPECT_FALSE(commands[1]["payload"]["include_creatures"].Bool());
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "prepared heroes");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCallMarketTradeHelpers)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				ai:marketTrade({ market_id = 16, mode_id = ai.marketModes.resourceResource, sell_resource_id = 0, buy_resource_id = 6, amount = 100 })
				ai:sendResources(16, 0, 1, 100)
				ai:sellCreatures(16, 5, 2, 6, 10)
				ai:buyMarketArtifact(16, 5, 6, 44)
				ai:sellArtifact(16, 5, 301, 6)
				ai:sacrificeArtifact(16, 5, 302)
				ai:sacrificeCreatures(16, 5, 3, 11)
				ai:sacrificeArtifacts(16, 5, { 303, 304 })
				ai:sacrificeCreatureStacks(16, 5, { 0, 1 }, { 2, 3 })
				ai:transformToUndead(16, 5, 4)
				ai:buySkill(16, 5, 7)
				return ai:output("end_turn")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-market-helpers", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 11);
	for(const JsonNode & command : commands)
	{
		EXPECT_EQ(command["payload"]["type"].String(), "market_trade");
		EXPECT_EQ(command["payload"]["market_id"].Integer(), 16);
	}
	EXPECT_EQ(commands[0]["payload"]["mode_id"].Integer(), 0);
	EXPECT_EQ(commands[0]["payload"]["sell_resource_id"].Integer(), 0);
	EXPECT_EQ(commands[0]["payload"]["buy_resource_id"].Integer(), 6);
	EXPECT_EQ(commands[0]["payload"]["amount"].Integer(), 100);
	EXPECT_EQ(commands[1]["payload"]["mode_id"].Integer(), 1);
	EXPECT_EQ(commands[1]["payload"]["target_player_id"].Integer(), 1);
	EXPECT_EQ(commands[2]["payload"]["mode_id"].Integer(), 2);
	EXPECT_EQ(commands[2]["payload"]["hero_id"].Integer(), 5);
	EXPECT_EQ(commands[2]["payload"]["slot"].Integer(), 2);
	EXPECT_EQ(commands[3]["payload"]["mode_id"].Integer(), 3);
	EXPECT_EQ(commands[3]["payload"]["artifact_id"].Integer(), 44);
	EXPECT_EQ(commands[4]["payload"]["mode_id"].Integer(), 4);
	EXPECT_EQ(commands[4]["payload"]["artifact_instance_id"].Integer(), 301);
	EXPECT_EQ(commands[5]["payload"]["mode_id"].Integer(), 5);
	EXPECT_EQ(commands[5]["payload"]["artifact_instance_id"].Integer(), 302);
	EXPECT_EQ(commands[6]["payload"]["mode_id"].Integer(), 6);
	EXPECT_EQ(commands[6]["payload"]["amount"].Integer(), 11);
	EXPECT_EQ(commands[7]["payload"]["mode_id"].Integer(), 5);
	ASSERT_TRUE(commands[7]["payload"]["artifact_instance_ids"].isVector());
	ASSERT_EQ(commands[7]["payload"]["artifact_instance_ids"].Vector().size(), 2);
	EXPECT_EQ(commands[7]["payload"]["artifact_instance_ids"].Vector()[0].Integer(), 303);
	EXPECT_EQ(commands[7]["payload"]["artifact_instance_ids"].Vector()[1].Integer(), 304);
	EXPECT_EQ(commands[8]["payload"]["mode_id"].Integer(), 6);
	ASSERT_TRUE(commands[8]["payload"]["slots"].isVector());
	ASSERT_TRUE(commands[8]["payload"]["amounts"].isVector());
	ASSERT_EQ(commands[8]["payload"]["slots"].Vector().size(), 2);
	ASSERT_EQ(commands[8]["payload"]["amounts"].Vector().size(), 2);
	EXPECT_EQ(commands[8]["payload"]["slots"].Vector()[0].Integer(), 0);
	EXPECT_EQ(commands[8]["payload"]["slots"].Vector()[1].Integer(), 1);
	EXPECT_EQ(commands[8]["payload"]["amounts"].Vector()[0].Integer(), 2);
	EXPECT_EQ(commands[8]["payload"]["amounts"].Vector()[1].Integer(), 3);
	EXPECT_EQ(commands[9]["payload"]["mode_id"].Integer(), 7);
	EXPECT_EQ(commands[9]["payload"]["slot"].Integer(), 4);
	EXPECT_EQ(commands[10]["payload"]["mode_id"].Integer(), 8);
	EXPECT_EQ(commands[10]["payload"]["skill_id"].Integer(), 7);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanStageArtifactsAtAltar)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				assert(ai.artifactSlots.transition == -3)
				assert(ai.artifactSlots.firstAvailable == -2)
				assert(ai.artifactSlots.altar == 19)
				ai:moveArtifactToAltar(16, 5, 1)
				ai:returnArtifactFromAltar(16, 5)
				ai:moveArtifactsToAltar(16, 5, false, true)
				ai:returnArtifactsFromAltar(16, 5, true, false)
				return ai:output("end_turn")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-altar-artifacts", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "swap_artifacts");
	EXPECT_EQ(commands[0]["payload"]["src"]["holder_id"].Integer(), 5);
	EXPECT_EQ(commands[0]["payload"]["src"]["slot"].Integer(), 1);
	EXPECT_EQ(commands[0]["payload"]["dst"]["holder_id"].Integer(), 16);
	EXPECT_EQ(commands[0]["payload"]["dst"]["slot"].Integer(), 19);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "swap_artifacts");
	EXPECT_EQ(commands[1]["payload"]["src"]["holder_id"].Integer(), 16);
	EXPECT_EQ(commands[1]["payload"]["src"]["slot"].Integer(), 19);
	EXPECT_EQ(commands[1]["payload"]["dst"]["holder_id"].Integer(), 5);
	EXPECT_EQ(commands[1]["payload"]["dst"]["slot"].Integer(), -2);
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "bulk_move_artifacts");
	EXPECT_EQ(commands[2]["payload"]["src_id"].Integer(), 5);
	EXPECT_EQ(commands[2]["payload"]["dst_id"].Integer(), 16);
	EXPECT_EQ(commands[2]["payload"]["src_hero_id"].Integer(), 5);
	EXPECT_FALSE(commands[2]["payload"]["swap"].Bool());
	EXPECT_FALSE(commands[2]["payload"]["equipped"].Bool());
	EXPECT_TRUE(commands[2]["payload"]["backpack"].Bool());
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "bulk_move_artifacts");
	EXPECT_EQ(commands[3]["payload"]["src_id"].Integer(), 16);
	EXPECT_EQ(commands[3]["payload"]["dst_id"].Integer(), 5);
	EXPECT_EQ(commands[3]["payload"]["dst_hero_id"].Integer(), 5);
	EXPECT_FALSE(commands[3]["payload"]["swap"].Bool());
	EXPECT_TRUE(commands[3]["payload"]["equipped"].Bool());
	EXPECT_FALSE(commands[3]["payload"]["backpack"].Bool());
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
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

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanInspectVisibleState)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local object = ai:getObject(42, 7)
				local hero = ai:getHero(7)
				local town = ai:getTown(9)
				local tile = ai:getTile(1, 2, 0)
				local objectsAt = ai:getObjectsAt(1, 2, 0)
				local availableHeroes = ai:getAvailableHeroes(9)
				local path = ai:getPath(7, 3, 4, 0)
				local objectPath = ai:getPathToObject(7, 42)
				local reachable = ai:getReachable(7, {
					radius = 8,
					maxMovementOptions = 5,
					maxObjectTargets = 6
				})
				local tileDanger = ai:getTileDanger(7, 3, 4, 0)
				local objectDanger = ai:getObjectDanger(7, 42, { checkGuards = false })
				local positionalDanger = ai:getDanger(7, 5, 6, 0, { check_guards = true })
				local state = ai:getState()
				local actionSpace = ai:getActionSpace()
				local analysis = ai:getAnalysis()
				local queries = ai:getQueries()
				local updates = ai:getUpdates(true)
				local limits = ai:getLimits()
				local grail = ai:getGrail()
				return {
					status = "end_turn",
					memory = {
						version = 1,
						objectId = object.id,
						heroId = hero.id,
						townId = town.id,
						tileX = tile.position.x,
						objectsAtX = objectsAt.position.x,
						availableHeroCount = availableHeroes.heroCount,
						pathReachable = path.reachable,
						pathRoute = path.route_id,
						objectPathObjectId = objectPath.object_id,
						reachableRadius = reachable.radius,
						reachableMoves = #reachable.movementOptions,
						tileRiskId = tileDanger.riskId,
						objectTargetKindId = objectDanger.targetKindId,
						positionalDangerX = positionalDanger.position.x,
						day = state.day,
						hasEndTurn = actionSpace.endTurnAction ~= nil,
						hasExecution = analysis.execution ~= nil,
						queryCount = #queries,
						opponentOnly = updates.opponentOnly,
						maxActions = limits.maxActions,
						grailKnownRatio = grail.knownRatio,
						grailPositionKnown = grail.positionKnown
					},
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-inspect", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		const std::string what = command["payload"]["what"].String();
		if(what == "object")
		{
			response["result"]["id"] = command["payload"]["object_id"];
			response["result"]["contextHeroId"] = command["payload"]["hero_id"];
		}
		else if(what == "hero")
		{
			response["result"]["id"] = command["payload"]["hero_id"];
		}
		else if(what == "town")
		{
			response["result"]["id"] = command["payload"]["town_id"];
		}
		else if(what == "tile" || what == "objects_at")
		{
			response["result"]["position"]["x"] = command["payload"]["x"];
			response["result"]["position"]["y"] = command["payload"]["y"];
			response["result"]["position"]["z"] = command["payload"]["z"];
		}
		else if(what == "available_heroes")
		{
			response["result"]["source_id"] = command["payload"]["source_id"];
			response["result"]["heroCount"] = JsonNode(2);
		}
		else if(what == "path")
		{
			response["result"]["hero_id"] = command["payload"]["hero_id"];
			response["result"]["reachable"] = JsonNode(true);
			response["result"]["route_id"] = JsonNode("route:test");
			if(hasField(command["payload"], "object_id"))
				response["result"]["object_id"] = command["payload"]["object_id"];
			else
			{
				response["result"]["destination"]["x"] = command["payload"]["x"];
				response["result"]["destination"]["y"] = command["payload"]["y"];
				response["result"]["destination"]["z"] = command["payload"]["z"];
			}
		}
		else if(what == "reachable")
		{
			response["result"]["hero_id"] = command["payload"]["hero_id"];
			response["result"]["radius"] = command["payload"]["radius"];
			JsonNode option;
			option["route_id"] = JsonNode("route:test");
			response["result"]["movementOptions"].Vector().push_back(option);
			response["result"]["reachableObjects"].Vector();
		}
		else if(what == "danger")
		{
			response["result"]["hero_id"] = command["payload"]["hero_id"];
			response["result"]["riskId"] = JsonNode(2);
			response["result"]["risk"] = JsonNode("risky");
			response["result"]["danger"] = JsonNode(100);
			response["result"]["safe"] = JsonNode(false);
			if(hasField(command["payload"], "object_id"))
			{
				response["result"]["targetKindId"] = JsonNode(2);
				response["result"]["object_id"] = command["payload"]["object_id"];
			}
			else
			{
				response["result"]["targetKindId"] = JsonNode(1);
				response["result"]["position"]["x"] = command["payload"]["x"];
				response["result"]["position"]["y"] = command["payload"]["y"];
				response["result"]["position"]["z"] = command["payload"]["z"];
			}
		}
		else if(what == "state")
		{
			response["result"]["day"] = JsonNode(3);
		}
		else if(what == "action_space")
		{
			response["result"]["endTurnAction"]["type"] = JsonNode("end_turn");
		}
		else if(what == "analysis")
		{
			response["result"]["execution"]["validatesVisibility"] = JsonNode(true);
		}
		else if(what == "queries")
		{
			response["result"].Vector();
		}
		else if(what == "updates")
		{
			response["result"]["opponentOnly"] = command["payload"]["opponent_only"];
		}
		else if(what == "limits")
		{
			response["result"]["maxActions"] = JsonNode(64);
		}
		else if(what == "grail")
		{
			response["result"]["knownRatio"].Float() = 0.5;
			response["result"]["fullyRevealed"] = JsonNode(false);
			response["result"]["positionKnown"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 19);
	for(const JsonNode & command : commands)
		EXPECT_EQ(command["kind"].String(), "inspect");
	EXPECT_EQ(commands[0]["payload"]["what"].String(), "object");
	EXPECT_EQ(commands[0]["payload"]["object_id"].Integer(), 42);
	EXPECT_EQ(commands[0]["payload"]["hero_id"].Integer(), 7);
	EXPECT_EQ(commands[3]["payload"]["what"].String(), "tile");
	EXPECT_EQ(commands[3]["payload"]["x"].Integer(), 1);
	EXPECT_EQ(commands[3]["payload"]["y"].Integer(), 2);
	EXPECT_EQ(commands[3]["payload"]["z"].Integer(), 0);
	EXPECT_EQ(commands[6]["payload"]["what"].String(), "path");
	EXPECT_EQ(commands[6]["payload"]["hero_id"].Integer(), 7);
	EXPECT_EQ(commands[6]["payload"]["x"].Integer(), 3);
	EXPECT_EQ(commands[6]["payload"]["y"].Integer(), 4);
	EXPECT_EQ(commands[6]["payload"]["z"].Integer(), 0);
	EXPECT_EQ(commands[7]["payload"]["what"].String(), "path");
	EXPECT_EQ(commands[7]["payload"]["object_id"].Integer(), 42);
	EXPECT_EQ(commands[8]["payload"]["what"].String(), "reachable");
	EXPECT_EQ(commands[8]["payload"]["radius"].Integer(), 8);
	EXPECT_EQ(commands[8]["payload"]["max_movement_options"].Integer(), 5);
	EXPECT_EQ(commands[8]["payload"]["max_object_targets"].Integer(), 6);
	EXPECT_EQ(commands[9]["payload"]["what"].String(), "danger");
	EXPECT_EQ(commands[9]["payload"]["hero_id"].Integer(), 7);
	EXPECT_EQ(commands[9]["payload"]["x"].Integer(), 3);
	EXPECT_EQ(commands[9]["payload"]["y"].Integer(), 4);
	EXPECT_EQ(commands[9]["payload"]["z"].Integer(), 0);
	EXPECT_EQ(commands[10]["payload"]["what"].String(), "danger");
	EXPECT_EQ(commands[10]["payload"]["object_id"].Integer(), 42);
	EXPECT_FALSE(commands[10]["payload"]["check_guards"].Bool());
	EXPECT_EQ(commands[11]["payload"]["what"].String(), "danger");
	EXPECT_EQ(commands[11]["payload"]["hero_id"].Integer(), 7);
	EXPECT_EQ(commands[11]["payload"]["x"].Integer(), 5);
	EXPECT_EQ(commands[11]["payload"]["y"].Integer(), 6);
	EXPECT_EQ(commands[11]["payload"]["z"].Integer(), 0);
	EXPECT_TRUE(commands[11]["payload"]["check_guards"].Bool());
	EXPECT_EQ(commands[18]["payload"]["what"].String(), "grail");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["objectId"].Integer(), 42);
	EXPECT_EQ(output.memory["heroId"].Integer(), 7);
	EXPECT_EQ(output.memory["townId"].Integer(), 9);
	EXPECT_EQ(output.memory["tileX"].Integer(), 1);
	EXPECT_EQ(output.memory["objectsAtX"].Integer(), 1);
	EXPECT_EQ(output.memory["availableHeroCount"].Integer(), 2);
	EXPECT_TRUE(output.memory["pathReachable"].Bool());
	EXPECT_EQ(output.memory["pathRoute"].String(), "route:test");
	EXPECT_EQ(output.memory["objectPathObjectId"].Integer(), 42);
	EXPECT_EQ(output.memory["reachableRadius"].Integer(), 8);
	EXPECT_EQ(output.memory["reachableMoves"].Integer(), 1);
	EXPECT_EQ(output.memory["tileRiskId"].Integer(), 2);
	EXPECT_EQ(output.memory["objectTargetKindId"].Integer(), 2);
	EXPECT_EQ(output.memory["day"].Integer(), 3);
	EXPECT_TRUE(output.memory["hasEndTurn"].Bool());
	EXPECT_TRUE(output.memory["hasExecution"].Bool());
	EXPECT_EQ(output.memory["queryCount"].Integer(), 0);
	EXPECT_TRUE(output.memory["opponentOnly"].Bool());
	EXPECT_EQ(output.memory["maxActions"].Integer(), 64);
	EXPECT_NEAR(output.memory["grailKnownRatio"].Float(), 0.5, 1e-8);
	EXPECT_FALSE(output.memory["grailPositionKnown"].Bool());
}

TEST(LuaAdventureScriptRunnerTest, ImperativeInspectHostErrorsAreCatchable)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local ok, err = pcall(function()
					ai:getObject(999)
				end)
				return {
					status = "end_turn",
					memory = { version = 1, caught = not ok, error = err },
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-inspect-catch", source);

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		EXPECT_EQ(command["kind"].String(), "inspect");

		JsonNode response;
		response["ok"] = JsonNode(false);
		response["error"] = JsonNode("hidden object");
		return response;
	});

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_TRUE(output.memory["caught"].Bool());
	EXPECT_NE(output.memory["error"].String().find("hidden object"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeTryHelpersReturnStructuredHostErrors)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local inspect = ai:tryInspect({ what = "object", object_id = 999 })
				local execute = ai:tryExecute({ type = "build", town_id = 7, building_id = 12 })
				local refresh = ai:tryRefresh()
				local option = ai:tryRunOption({})
				local call = ai:tryCall(function(value)
					return { echoed = value }
				end, 17)
				local invalidCall = ai:tryCall("not a function")
				return {
					status = "end_turn",
					memory = {
						version = 1,
						inspectOk = inspect.ok,
						inspectError = inspect.error,
						executeOk = execute.ok,
						executeError = execute.error,
						refreshOk = refresh.ok,
						refreshDay = refresh.input.state.day,
						optionOk = option.ok,
						optionError = option.error,
						callOk = call.ok,
						callEcho = call.result.echoed,
						invalidCallOk = invalidCall.ok,
						invalidCallError = invalidCall.error
					},
					actions = {}
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:imperative-try-helpers", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		if(command["kind"].String() == "inspect")
		{
			response["ok"] = JsonNode(false);
			response["error"] = JsonNode("hidden object");
		}
		else if(command["kind"].String() == "execute")
		{
			response["ok"] = JsonNode(false);
			response["error"] = JsonNode("invalid build");
		}
		else if(command["kind"].String() == "refresh")
		{
			AI::AdventureScriptInput refreshed = makeInput();
			refreshed.state["day"] = JsonNode(5);
			response["ok"] = JsonNode(true);
			response["input"] = refreshed.toJson();
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 3);
	EXPECT_EQ(commands[0]["kind"].String(), "inspect");
	EXPECT_EQ(commands[1]["kind"].String(), "execute");
	EXPECT_EQ(commands[2]["kind"].String(), "refresh");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_FALSE(output.memory["inspectOk"].Bool());
	EXPECT_NE(output.memory["inspectError"].String().find("hidden object"), std::string::npos);
	EXPECT_FALSE(output.memory["executeOk"].Bool());
	EXPECT_NE(output.memory["executeError"].String().find("invalid build"), std::string::npos);
	EXPECT_TRUE(output.memory["refreshOk"].Bool());
	EXPECT_EQ(output.memory["refreshDay"].Integer(), 5);
	EXPECT_FALSE(output.memory["optionOk"].Bool());
	EXPECT_NE(output.memory["optionError"].String().find("missing action field"), std::string::npos);
	EXPECT_TRUE(output.memory["callOk"].Bool());
	EXPECT_EQ(output.memory["callEcho"].Integer(), 17);
	EXPECT_FALSE(output.memory["invalidCallOk"].Bool());
	EXPECT_NE(output.memory["invalidCallError"].String().find("expects a function"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, BoundedNullkillerControlEndsTurnWhenNativeSliceIsIdle)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/boundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-control-idle", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	input.limits["maxScriptCallsPerTurn"] = JsonNode(4);
	input.limits["maxActions"] = JsonNode(16);

	std::vector<JsonNode> commands;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			response["result"]["didWork"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[0]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[0]["payload"]["first_pass_index"].Integer(), 1);
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 0);
	EXPECT_EQ(commands[0]["payload"]["max_attempts"].Integer(), 0);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "bounded Nullkiller control found no remaining native work");
	EXPECT_EQ(output.memory["totalSlices"].Integer(), 1);
}

TEST(LuaAdventureScriptRunnerTest, BoundedNullkillerControlContinuesAfterProductiveExhaustedSlice)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/boundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-control-productive-exhausted", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	input.limits["maxActions"] = JsonNode(16);

	std::vector<JsonNode> commands;
	int slices = 0;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		const std::string kind = command["kind"].String();
		if(kind == "refresh")
		{
			response["input"] = input.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			++slices;
			response["result"]["didWork"] = JsonNode(slices == 1);
			response["result"]["priorityTasksExecuted"] = JsonNode(slices == 1 ? 1 : 0);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(true);
		}
		return response;
	});

	ASSERT_EQ(slices, 2);
	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[0]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[0]["payload"]["first_pass_index"].Integer(), 1);
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[2]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[2]["payload"]["first_pass_index"].Integer(), 2);
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["totalSlices"].Integer(), 2);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "bounded Nullkiller control accepted native stop-turn signal");
}

TEST(LuaAdventureScriptRunnerTest, BoundedNullkillerControlStopsAtNativeMaxPassLimit)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/boundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-control-max-pass", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	input.limits["maxActions"] = JsonNode(16);
	input.analysis["nullkiller"]["settings"]["maxPass"] = JsonNode(2);

	std::vector<JsonNode> commands;
	int slices = 0;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		const std::string kind = command["kind"].String();
		if(kind == "refresh")
		{
			response["input"] = input.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			++slices;
			response["result"]["didWork"] = JsonNode(true);
			response["result"]["priorityTasksExecuted"] = JsonNode(1);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(slices, 2);
	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[0]["payload"]["first_pass_index"].Integer(), 1);
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[2]["payload"]["first_pass_index"].Integer(), 2);
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["totalSlices"].Integer(), 2);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "bounded Nullkiller control accepted native max-pass limit");
}

TEST(LuaAdventureScriptRunnerTest, BoundedNullkillerControlAnswersQueriesBeforeNativeSlice)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/boundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-control-query", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	JsonNode query;
	query["query_id"] = JsonNode(77);
	query["typeId"] = JsonNode(3);
	input.state["turn"]["queries"].Vector().push_back(query);
	input.limits["maxScriptCallsPerTurn"] = JsonNode(6);
	input.limits["maxActions"] = JsonNode(16);

	std::vector<JsonNode> commands;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		const std::string kind = command["kind"].String();
		if(kind == "refresh")
		{
			AI::AdventureScriptInput refreshed = input;
			refreshed.state["turn"]["queries"].Vector().clear();
			response["input"] = refreshed.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			response["result"]["didWork"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_answer_query");
	EXPECT_EQ(commands[0]["payload"]["query_id"].Integer(), 77);
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["totalQueriesAnswered"].Integer(), 1);
}

TEST(LuaAdventureScriptRunnerTest, BoundedNullkillerControlUsesImperativeActionBudget)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/boundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-control-action-budget", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	input.limits["maxScriptCallsPerTurn"] = JsonNode(4);
	input.limits["maxActions"] = JsonNode(16);
	input.analysis["nullkiller"]["settings"]["maxPass"] = JsonNode(10);

	std::vector<JsonNode> commands;
	int slices = 0;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		const std::string kind = command["kind"].String();
		if(kind == "refresh")
		{
			response["input"] = input.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			++slices;
			response["result"]["didWork"] = JsonNode(slices <= 5);
			response["result"]["priorityTasksExecuted"] = JsonNode(slices <= 5 ? 1 : 0);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(slices, 6);
	ASSERT_EQ(commands.size(), 12);
	for(size_t index = 0; index < 10; index += 2)
	{
		EXPECT_EQ(commands[index]["payload"]["type"].String(), "nullkiller_turn_slice");
		EXPECT_EQ(commands[index]["payload"]["max_passes"].Integer(), 1);
		EXPECT_EQ(commands[index]["payload"]["first_pass_index"].Integer(), static_cast<si64>(index / 2 + 1));
		EXPECT_EQ(commands[index + 1]["kind"].String(), "refresh");
	}
	EXPECT_EQ(commands[10]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[10]["payload"]["max_passes"].Integer(), 1);
	EXPECT_EQ(commands[10]["payload"]["first_pass_index"].Integer(), 6);
	EXPECT_EQ(commands[11]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["totalSlices"].Integer(), 6);
}

TEST(LuaAdventureScriptRunnerTest, DefensiveBoundedControlRecruitsBeforeNativeSliceUnderPressure)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/defensiveBoundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:defensive-bounded-control-recruit", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	input.limits["maxActions"] = JsonNode(16);

	JsonNode alert;
	alert["town_id"] = JsonNode(42);
	alert["levelId"] = JsonNode(3);
	input.analysis["defenseAlerts"].Vector().push_back(alert);

	JsonNode recruitOption;
	recruitOption["source_id"] = JsonNode(42);
	recruitOption["amount"] = JsonNode(12);
	recruitOption["level"] = JsonNode(3);
	recruitOption["planAction"]["type"] = JsonNode("recruit");
	recruitOption["planAction"]["source_id"] = JsonNode(42);
	recruitOption["planAction"]["creature_id"] = JsonNode(1);
	recruitOption["planAction"]["amount"] = JsonNode(12);
	input.actionSpace["recruitOptions"].Vector().push_back(recruitOption);

	std::vector<JsonNode> commands;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		const std::string kind = command["kind"].String();
		if(kind == "refresh")
		{
			AI::AdventureScriptInput refreshed = input;
			refreshed.analysis["defenseAlerts"].Vector().clear();
			refreshed.actionSpace["recruitOptions"].Vector().clear();
			response["input"] = refreshed.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			response["result"]["didWork"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "recruit");
	EXPECT_EQ(commands[0]["payload"]["source_id"].Integer(), 42);
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["totalEmergencyDefenseActions"].Integer(), 1);
}

TEST(LuaAdventureScriptRunnerTest, DefensiveBoundedControlBuildsOnlyWhenRecruitmentIsUnavailable)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/defensiveBoundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:defensive-bounded-control-build", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	input.limits["maxActions"] = JsonNode(16);

	JsonNode alert;
	alert["town_id"] = JsonNode(42);
	alert["levelId"] = JsonNode(3);
	input.analysis["defenseAlerts"].Vector().push_back(alert);

	JsonNode hallOption;
	hallOption["town_id"] = JsonNode(42);
	hallOption["buildingKindId"] = JsonNode(5);
	hallOption["cost"]["gold"] = JsonNode(2500);
	hallOption["planAction"]["type"] = JsonNode("build");
	hallOption["planAction"]["town_id"] = JsonNode(42);
	hallOption["planAction"]["building_id"] = JsonNode(12);
	input.actionSpace["buildOptions"].Vector().push_back(hallOption);

	JsonNode fortOption;
	fortOption["town_id"] = JsonNode(42);
	fortOption["buildingKindId"] = JsonNode(4);
	fortOption["cost"]["gold"] = JsonNode(1000);
	fortOption["planAction"]["type"] = JsonNode("build");
	fortOption["planAction"]["town_id"] = JsonNode(42);
	fortOption["planAction"]["building_id"] = JsonNode(7);
	input.actionSpace["buildOptions"].Vector().push_back(fortOption);

	std::vector<JsonNode> commands;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		const std::string kind = command["kind"].String();
		if(kind == "refresh")
		{
			AI::AdventureScriptInput refreshed = input;
			refreshed.analysis["defenseAlerts"].Vector().clear();
			refreshed.actionSpace["buildOptions"].Vector().clear();
			response["input"] = refreshed.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			response["result"]["didWork"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "build");
	EXPECT_EQ(commands[0]["payload"]["town_id"].Integer(), 42);
	EXPECT_EQ(commands[0]["payload"]["building_id"].Integer(), 7);
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["totalEmergencyDefenseActions"].Integer(), 1);
}

TEST(LuaAdventureScriptRunnerTest, DefensiveBoundedControlDoesNotSpendWhenMapTempoExists)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/defensiveBoundedNullkillerControl.lua");
	scripting::LuaAdventureScriptRunner runner("test:defensive-bounded-control-map-tempo", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.state["turn"]["queries"].Vector();
	input.limits["maxActions"] = JsonNode(16);

	JsonNode hero;
	hero["id"] = JsonNode(5);
	input.state["heroes"].Vector().push_back(hero);

	JsonNode moveOption;
	moveOption["hero_id"] = JsonNode(5);
	moveOption["planAction"]["type"] = JsonNode("move_hero");
	moveOption["planAction"]["hero_id"] = JsonNode(5);
	moveOption["planAction"]["x"] = JsonNode(10);
	moveOption["planAction"]["y"] = JsonNode(10);
	moveOption["planAction"]["z"] = JsonNode(0);
	input.actionSpace["movementOptions"].Vector().push_back(moveOption);

	JsonNode alert;
	alert["town_id"] = JsonNode(42);
	alert["levelId"] = JsonNode(3);
	input.analysis["defenseAlerts"].Vector().push_back(alert);

	JsonNode recruitOption;
	recruitOption["source_id"] = JsonNode(42);
	recruitOption["amount"] = JsonNode(12);
	recruitOption["level"] = JsonNode(3);
	recruitOption["planAction"]["type"] = JsonNode("recruit");
	recruitOption["planAction"]["source_id"] = JsonNode(42);
	recruitOption["planAction"]["creature_id"] = JsonNode(1);
	recruitOption["planAction"]["amount"] = JsonNode(12);
	input.actionSpace["recruitOptions"].Vector().push_back(recruitOption);

	std::vector<JsonNode> commands;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			response["result"]["didWork"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["exhaustedCandidates"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["totalEmergencyDefenseActions"].Integer(), 0);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanReadNullkillerSnapshots)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local beforeState = ai:nullkillerState()
				local settings = ai:nullkillerSettings()
				local economy = ai:nullkillerEconomy()
				local recruitment = ai:nullkillerHeroRecruitment()
				ai:refresh()
				local afterState = ai:nullkillerState()

				ai:setMemory({
					version = 1,
					freeGoldBeforeRefresh = beforeState.freeResources.gold,
					scanDepthAfterRefresh = afterState.scanDepthId,
					safeAttackRatio = settings.safeAttackRatio,
					goldPressure = economy.goldPressure,
					heroCapReached = recruitment.heroCapReached
				})
				return ai:output("end_turn", "read Nullkiller snapshot")
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	input.analysis["nullkiller"]["state"]["freeResources"]["gold"] = JsonNode(1200);
	input.analysis["nullkiller"]["settings"]["safeAttackRatio"].Float() = 1.5;
	input.analysis["nullkiller"]["economy"]["goldPressure"].Float() = 0.25;
	input.analysis["nullkiller"]["heroRecruitment"]["heroCapReached"] = JsonNode(false);

	scripting::LuaAdventureScriptRunner runner("test:imperative-nullkiller-snapshot", source);

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		EXPECT_EQ(command["kind"].String(), "refresh");

		AI::AdventureScriptInput refreshed = input;
		refreshed.analysis["nullkiller"]["state"]["scanDepthId"] = JsonNode(3);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["input"] = refreshed.toJson();
		return response;
	});

	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "read Nullkiller snapshot");
	EXPECT_EQ(output.memory["freeGoldBeforeRefresh"].Integer(), 1200);
	EXPECT_EQ(output.memory["scanDepthAfterRefresh"].Integer(), 3);
	EXPECT_DOUBLE_EQ(output.memory["safeAttackRatio"].Float(), 1.5);
	EXPECT_DOUBLE_EQ(output.memory["goldPressure"].Float(), 0.25);
	EXPECT_FALSE(output.memory["heroCapReached"].Bool());
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanReadAndAnswerPendingQueries)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local queries = ai:pendingQueries()
				ai:runOption(queries[1].components[1])
				return {
					status = "end_turn",
					memory = { version = 1, queryType = queries[1].type, queryTypeId = queries[1].typeId },
					actions = {}
				}
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["queries"].Vector();
	JsonNode query;
	query["query_id"] = JsonNode(77);
	query["typeId"] = JsonNode(3);
	query["type"] = JsonNode("blocking_dialog");
	query["components"].Vector();
	JsonNode component;
	component["answer"] = JsonNode(2);
	component["planAction"]["type"] = JsonNode("answer_query");
	component["planAction"]["query_id"] = JsonNode(77);
	component["planAction"]["answer"] = JsonNode(2);
	query["components"].Vector().push_back(component);
	input.state["turn"]["queries"].Vector().push_back(query);

	scripting::LuaAdventureScriptRunner runner("test:imperative-pending-queries", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 1);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "answer_query");
	EXPECT_EQ(commands[0]["payload"]["query_id"].Integer(), 77);
	EXPECT_EQ(commands[0]["payload"]["answer"].Integer(), 2);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["queryType"].String(), "blocking_dialog");
	EXPECT_EQ(output.memory["queryTypeId"].Integer(), 3);
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanLetNullkillerAnswerPendingQueries)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local result = ai:nullkillerAnswerPendingQueries(1, 4)
				return {
					status = "end_turn",
					memory = {
						version = 1,
						count = result.count,
						truncated = result.truncated
					},
					actions = {}
				}
			end
		}
	)lua";

	auto makeInputWithRemainingQueries = [](int firstRemainingIndex)
	{
		AI::AdventureScriptInput input = makeInput();
		input.state["turn"]["queries"].Vector();
		for(int index = firstRemainingIndex; index < 2; ++index)
		{
			JsonNode query;
			query["query_id"] = JsonNode(77 + index);
			query["type"] = JsonNode(index == 0 ? "hero_level_up" : "blocking_dialog");
			input.state["turn"]["queries"].Vector().push_back(query);
		}
		return input;
	};

	scripting::LuaAdventureScriptRunner runner("test:imperative-nullkiller-pending-queries", source);
	std::vector<JsonNode> commands;
	int answeredQueries = 0;
	int refreshes = 0;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInputWithRemainingQueries(0), [&](const JsonNode & command)
	{
		JsonNode response;
		response["ok"] = JsonNode(true);

		if(command["kind"].String() == "refresh")
		{
			++refreshes;
			response["input"] = makeInputWithRemainingQueries(answeredQueries).toJson();
			return response;
		}

		commands.push_back(command);
		++answeredQueries;
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_answer_query");
	EXPECT_EQ(commands[0]["payload"]["query_id"].Integer(), 77);
	EXPECT_EQ(commands[0]["payload"]["default_answer"].Integer(), 1);
	EXPECT_TRUE(commands[0]["payload"]["allow_expired"].Bool());
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_answer_query");
	EXPECT_EQ(commands[1]["payload"]["query_id"].Integer(), 78);
	EXPECT_TRUE(commands[1]["payload"]["allow_expired"].Bool());
	EXPECT_EQ(refreshes, 2);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["count"].Integer(), 2);
	EXPECT_FALSE(output.memory["truncated"].Bool());
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanCancelOptionalQueryReply)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local queries = ai:pendingQueries()
				ai:runOption(queries[1], "cancelAction")
				ai:cancelQuery(queries[2].query_id)
				return ai:output("end_turn", "cancelled optional query replies")
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["queries"].Vector();
	for(int queryID : { 77, 78 })
	{
		JsonNode query;
		query["query_id"] = JsonNode(queryID);
		query["type"] = JsonNode("map_object_select");
		query["cancelAction"]["type"] = JsonNode("cancel_query");
		query["cancelAction"]["query_id"] = JsonNode(queryID);
		input.state["turn"]["queries"].Vector().push_back(query);
	}

	scripting::LuaAdventureScriptRunner runner("test:imperative-cancel-query", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "cancel_query");
	EXPECT_EQ(commands[0]["payload"]["query_id"].Integer(), 77);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "cancel_query");
	EXPECT_EQ(commands[1]["payload"]["query_id"].Integer(), 78);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "cancelled optional query replies");
}

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanChooseChestRewardByComponentIds)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local queries = ai:pendingQueries()
				ai:chooseChestReward(queries[1], "experience")
				return ai:output("end_turn", "picked chest experience")
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["queries"].Vector();
	JsonNode query;
	query["query_id"] = JsonNode(88);
	query["type"] = JsonNode("blocking_dialog");
	query["selection"] = JsonNode(true);
	query["components"].Vector();
	JsonNode gold;
	gold["typeId"] = JsonNode(2);
	gold["type"] = JsonNode("resource");
	gold["subtypeId"] = JsonNode(6);
	gold["value"] = JsonNode(1000);
	gold["answer"] = JsonNode(1);
	query["components"].Vector().push_back(gold);
	JsonNode experience;
	experience["typeId"] = JsonNode(8);
	experience["type"] = JsonNode("experience");
	experience["value"] = JsonNode(500);
	experience["answer"] = JsonNode(2);
	query["components"].Vector().push_back(experience);
	input.state["turn"]["queries"].Vector().push_back(query);

	scripting::LuaAdventureScriptRunner runner("test:imperative-chest-query", source);
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		return response;
	});

	ASSERT_EQ(commands.size(), 1);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "answer_query");
	EXPECT_EQ(commands[0]["payload"]["query_id"].Integer(), 88);
	EXPECT_EQ(commands[0]["payload"]["answer"].Integer(), 2);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "picked chest experience");
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

TEST(LuaAdventureScriptRunnerTest, MissingBattleRetreatCallbackDelegatesToHost)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				return ai:output("end_turn")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:missing-retreat-callback", source);
	JsonNode input;
	input["battleRetreat"]["nullkiller_decision_id"] = JsonNode(2);

	const std::optional<JsonNode> output = runner.decideBattleRetreat(input);
	EXPECT_FALSE(output);
}

TEST(LuaAdventureScriptRunnerTest, BattleRetreatCallbackReturnsJsonDecision)
{
	const std::string source = R"lua(
		return {
			decideBattleRetreat = function(input)
				return {
					decision_id = input.battleRetreat.nullkiller_decision_id,
					seen_flee_flag = input.battleRetreat.can_flee,
					memory_version = input.memory.version
				}
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:retreat-callback", source);
	JsonNode input;
	input["memory"]["version"] = JsonNode(7);
	input["battleRetreat"]["can_flee"] = JsonNode(true);
	input["battleRetreat"]["nullkiller_decision_id"] = JsonNode(2);

	const std::optional<JsonNode> output = runner.decideBattleRetreat(input);
	ASSERT_TRUE(output);
	EXPECT_EQ((*output)["decision_id"].Integer(), 2);
	EXPECT_TRUE((*output)["seen_flee_flag"].Bool());
	EXPECT_EQ((*output)["memory_version"].Integer(), 7);
}

TEST(LuaAdventureScriptRunnerTest, BattleRetreatCallbackErrorsSurfaceToHost)
{
	const std::string source = R"lua(
		return {
			decideBattleRetreat = function(input)
				error("retreat policy failed")
			end
		}
	)lua";

	scripting::LuaAdventureScriptRunner runner("test:retreat-callback-error", source);
	JsonNode input;
	EXPECT_THROW(runner.decideBattleRetreat(input), std::runtime_error);
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
		const AI::AdventureScriptInput input = readFixtureInput(fixture);
		if(hasField(fixture, "mode") && fixture["mode"].String() == "imperative")
		{
			std::vector<JsonNode> commands;
			const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
			{
				commands.push_back(command);
				return fixtureHostResponse(fixture, input, command);
			});

			verifyFixtureExpectation(path, fixture, output);
			verifyFixtureCommandExpectation(path, fixture, commands);
			continue;
		}

		const AI::AdventureScriptOutput output = runner.planDay(input);
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
		"scripts/ai/candidates/defensiveBoundedNullkillerControl.lua",
		"scripts/ai/candidates/fallbackAdventure.lua",
		"scripts/ai/candidates/statisticsProbeAdventure.lua"
	};

	for(const std::string & script : scripts)
	{
		scripting::LuaAdventureScriptRunner runner(script, readAdventureScript(script));
		const AI::AdventureScriptOutput output = runner.planDay(makeInput());

		if(script == "scripts/ai/defaultAdventure.lua"
			|| script == "scripts/ai/candidates/boundedNullkillerAdventure.lua"
			|| script == "scripts/ai/candidates/defensiveBoundedNullkillerControl.lua"
			|| script == "scripts/ai/candidates/fallbackAdventure.lua"
			|| script == "scripts/ai/candidates/statisticsProbeAdventure.lua")
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

TEST(LuaAdventureScriptRunnerTest, BoundedNullkillerAdventureEndsTurnWithoutFullFallback)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/boundedNullkillerAdventure.lua");
	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-adventure-idle", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.limits["maxActions"] = JsonNode(4);

	std::vector<JsonNode> commands;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_step")
		{
			response["result"]["didExecute"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(false);
			response["result"]["outcomeId"] = JsonNode(0);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "bounded Nullkiller policy found no executable native task");
}

TEST(LuaAdventureScriptRunnerTest, BoundedNullkillerAdventureErrorsWhenBudgetExhausted)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/boundedNullkillerAdventure.lua");
	scripting::LuaAdventureScriptRunner runner("test:bounded-nullkiller-adventure-budget", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);
	input.limits["maxActions"] = JsonNode(1);

	std::vector<JsonNode> commands;
	EXPECT_THROW(runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		if(command["kind"].String() == "refresh")
		{
			response["input"] = input.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_step")
		{
			response["result"]["didExecute"] = JsonNode(true);
			response["result"]["shouldStopTurn"] = JsonNode(false);
		}
		return response;
	}), std::runtime_error);

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_step");
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
}

TEST(LuaAdventureScriptRunnerTest, StatisticsProbeAdventureUsesBoundedSliceWithoutFullFallback)
{
	const std::string source = readAdventureScript("scripts/ai/candidates/statisticsProbeAdventure.lua");
	scripting::LuaAdventureScriptRunner runner("test:statistics-probe-bounded", source);

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["active"] = JsonNode(true);

	std::vector<JsonNode> commands;
	const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		if(command["kind"].String() == "refresh")
		{
			AI::AdventureScriptInput refreshed = input;
			JsonNode update;
			update["type"] = JsonNode("statistics_response");
			refreshed.updates.Vector().push_back(update);
			response["input"] = refreshed.toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			response["result"]["didWork"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(true);
			response["result"]["adventureStopTurnSteps"] = JsonNode(0);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "request_statistic");
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["statisticsRequests"].Integer(), 1);
	EXPECT_EQ(output.memory["statisticsResponses"].Integer(), 1);
	EXPECT_TRUE(output.memory["lastStatisticResponseSeen"].Bool());
	EXPECT_TRUE(output.memory["lastSliceRequestedStop"].Bool());
	ASSERT_TRUE(output.intent);
	EXPECT_EQ(*output.intent, "statistics probe completed through a bounded native slice");
}

TEST(LuaAdventureScriptRunnerTest, BundledPersonalityScriptsRunImperatively)
{
	const std::vector<std::string> scripts = {
		"scripts/ai/aggressiveAdventure.lua",
		"scripts/ai/economyAdventure.lua",
		"scripts/ai/explorerAdventure.lua"
	};

	for(const std::string & script : scripts)
	{
		SCOPED_TRACE(script);
		scripting::LuaAdventureScriptRunner runner(script, readAdventureScript(script));
		std::vector<JsonNode> commands;

		const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
		{
			commands.push_back(command);

			JsonNode response;
			response["ok"] = JsonNode(true);
			response["result"]["ok"] = JsonNode(true);
			response["result"]["type"] = command["payload"]["type"];
			return response;
		});

		ASSERT_EQ(commands.size(), 1);
		EXPECT_EQ(commands[0]["payload"]["type"].String(), "end_turn");
		EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	}
}

TEST(LuaAdventureScriptRunnerTest, BundledPersonalityScriptsUseBoundedNullkillerAfterBudget)
{
	const std::vector<std::string> scripts = {
		"scripts/ai/aggressiveAdventure.lua",
		"scripts/ai/economyAdventure.lua",
		"scripts/ai/explorerAdventure.lua"
	};

	for(const std::string & script : scripts)
	{
		SCOPED_TRACE(script);
		scripting::LuaAdventureScriptRunner runner(script, readAdventureScript(script));
		AI::AdventureScriptInput input = makeInput();
		input.limits["maxScriptCallsPerTurn"] = JsonNode(1);
		input.limits["maxActions"] = JsonNode(1);
		input.actionSpace["buildOptions"].Vector();

		JsonNode buildOption;
		buildOption["buildingKindId"] = JsonNode(2); // tavern; accepted by all three profile scorers
		buildOption["buildingLevel"] = JsonNode(1);
		buildOption["planAction"]["type"] = JsonNode("build");
		buildOption["planAction"]["town_id"] = JsonNode(17);
		buildOption["planAction"]["building_id"] = JsonNode(2);
		input.actionSpace["buildOptions"].Vector().push_back(buildOption);

		std::vector<JsonNode> commands;
		const AI::AdventureScriptOutput output = runner.runDayImperative(input, [&](const JsonNode & command)
		{
			commands.push_back(command);

			JsonNode response;
			response["ok"] = JsonNode(true);
			if(command["kind"].String() == "refresh")
			{
				response["input"] = input.toJson();
				return response;
			}

			const std::string type = command["payload"]["type"].String();
			response["result"]["ok"] = JsonNode(true);
			response["result"]["type"] = JsonNode(type);
			if(type == "nullkiller_turn_slice")
			{
				response["result"]["didWork"] = JsonNode(false);
				response["result"]["shouldStopTurn"] = JsonNode(true);
				response["result"]["adventureStopTurnSteps"] = JsonNode(1);
			}
			return response;
		});

		ASSERT_EQ(commands.size(), 4);
		EXPECT_EQ(commands[0]["payload"]["type"].String(), "build");
		EXPECT_EQ(commands[1]["kind"].String(), "refresh");
		EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
		EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
		EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
		ASSERT_TRUE(output.intent);
		EXPECT_NE(output.intent->find("bounded Nullkiller slice accepted native stop-turn signal"), std::string::npos);
	}
}

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureEndsTurnAfterIdleBoundedNullkillerSlice)
{
	scripting::LuaAdventureScriptRunner runner(
		"scripts/ai/defaultAdventure.lua",
		readAdventureScript("scripts/ai/defaultAdventure.lua"));
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		response["result"]["didWork"] = JsonNode(false);
		response["result"]["priorityTasksExecuted"] = JsonNode(0);
		response["result"]["adventureStepsExecuted"] = JsonNode(0);
		response["result"]["adventureReplanSteps"] = JsonNode(0);
		response["result"]["adventureStopTurnSteps"] = JsonNode(0);
		response["result"]["tradePasses"] = JsonNode(0);
		response["result"]["paused"] = JsonNode(false);
		response["result"]["stop"] = JsonNode(false);
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[0]["payload"]["max_passes"].Integer(), 4);
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 16);
	EXPECT_EQ(commands[0]["payload"]["max_attempts"].Integer(), 4);
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_NE(output.intent->find("bounded Nullkiller turn slice found no remaining native work"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureEndsTurnOnBoundedNullkillerStopSignal)
{
	scripting::LuaAdventureScriptRunner runner(
		"scripts/ai/defaultAdventure.lua",
		readAdventureScript("scripts/ai/defaultAdventure.lua"));
	std::vector<JsonNode> commands;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			response["result"]["didWork"] = JsonNode(false);
			response["result"]["shouldStopTurn"] = JsonNode(true);
			response["result"]["adventureStopTurnSteps"] = JsonNode(0);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_NE(output.intent->find("bounded Nullkiller turn slice accepted native stop-turn signal"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureContinuesAfterTradeOnlyNullkillerSlice)
{
	scripting::LuaAdventureScriptRunner runner(
		"scripts/ai/defaultAdventure.lua",
		readAdventureScript("scripts/ai/defaultAdventure.lua"));
	std::vector<JsonNode> commands;
	int slices = 0;

	const AI::AdventureScriptOutput output = runner.runDayImperative(makeInput(), [&](const JsonNode & command)
	{
		commands.push_back(command);

		JsonNode response;
		response["ok"] = JsonNode(true);
		if(command["kind"].String() == "refresh")
		{
			response["input"] = makeInput().toJson();
			return response;
		}

		response["result"]["ok"] = JsonNode(true);
		response["result"]["type"] = command["payload"]["type"];
		if(command["payload"]["type"].String() == "nullkiller_turn_slice")
		{
			++slices;
			response["result"]["didWork"] = JsonNode(slices == 1);
			response["result"]["priorityTasksExecuted"] = JsonNode(0);
			response["result"]["adventureStepsExecuted"] = JsonNode(0);
			response["result"]["adventureReplanSteps"] = JsonNode(0);
			response["result"]["adventureStopTurnSteps"] = JsonNode(0);
			response["result"]["tradePasses"] = JsonNode(slices == 1 ? 2 : 0);
			response["result"]["paused"] = JsonNode(false);
			response["result"]["stop"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(slices, 2);
	ASSERT_EQ(commands.size(), 4);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[1]["kind"].String(), "refresh");
	EXPECT_EQ(commands[2]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[3]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_NE(output.intent->find("bounded Nullkiller turn slice found no remaining native work"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, PackagedConfigUsesBoundedControlScript)
{
	const JsonNode config = readJsonFile(std::filesystem::path(VCMI_SOURCE_DIR) / "config/ai/scriptedAdventure.json");

	ASSERT_TRUE(config["script"].isString());
	EXPECT_EQ(config["script"].String(), "ai/candidates/boundedNullkillerControl.lua");
	ASSERT_TRUE(config["reloadScriptEachTurn"].isBool());
	EXPECT_FALSE(config["reloadScriptEachTurn"].Bool())
		<< "Normal AI runtime should keep one Lua runner instance per game; reload mode is an explicit development override.";
	ASSERT_TRUE(config["actionWaitTimeoutMs"].isNumber());
	EXPECT_GT(config["actionWaitTimeoutMs"].Integer(), 0);
	ASSERT_TRUE(config["battleActionWaitTimeoutMs"].isNumber());
	EXPECT_GT(config["battleActionWaitTimeoutMs"].Integer(), 0);
	EXPECT_LT(config["battleActionWaitTimeoutMs"].Integer(), 180000)
		<< "Scripted battle/blocker waits should return as checked action failures before the batch idle watchdog kills the process.";
	EXPECT_FALSE(hasField(config, "players"))
		<< "Experimental personality scripts should stay opt-in until they beat the bounded control.";
}
