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
				return {
					status = "end_turn",
					memory = {
						version = 1,
						firstTask = candidates.tasks[1].task_id,
						stepTask = step.selectedTask.task_id,
						executedOutcome = ai.nullkillerStepOutcomes.executed,
						defenseMode = ai.nullkillerTaskModes.defense,
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

	ASSERT_EQ(commands.size(), 46);
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
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	EXPECT_EQ(output.memory["firstTask"].Integer(), 41);
	EXPECT_EQ(output.memory["stepTask"].Integer(), 42);
	EXPECT_EQ(output.memory["executedOutcome"].Integer(), 1);
	EXPECT_EQ(output.memory["defenseMode"].Integer(), 8);
	EXPECT_EQ(output.memory["dimensionDoorSpellKind"].Integer(), 2);
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

TEST(LuaAdventureScriptRunnerTest, ImperativeDayCanReadAndAnswerPendingQueries)
{
	const std::string source = R"lua(
		return {
			runDay = function(ai, input)
				local queries = ai:pendingQueries()
				ai:runOption(queries[1].components[1])
				return {
					status = "end_turn",
					memory = { version = 1, queryType = queries[1].type },
					actions = {}
				}
			end
		}
	)lua";

	AI::AdventureScriptInput input = makeInput();
	input.state["turn"]["queries"].Vector();
	JsonNode query;
	query["query_id"] = JsonNode(77);
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
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "nullkiller_answer_query");
	EXPECT_EQ(commands[1]["payload"]["query_id"].Integer(), 78);
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
		"scripts/ai/candidates/fallbackAdventure.lua",
		"scripts/ai/candidates/statisticsProbeAdventure.lua"
	};

	for(const std::string & script : scripts)
	{
		scripting::LuaAdventureScriptRunner runner(script, readAdventureScript(script));
		const AI::AdventureScriptOutput output = runner.planDay(makeInput());

		if(script == "scripts/ai/defaultAdventure.lua"
			|| script == "scripts/ai/candidates/boundedNullkillerAdventure.lua"
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

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureTriesBoundedNullkillerTurnSliceBeforeFallback)
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

	ASSERT_EQ(commands.size(), 1);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[0]["payload"]["max_passes"].Integer(), 4);
	EXPECT_EQ(commands[0]["payload"]["max_candidates"].Integer(), 16);
	EXPECT_EQ(commands[0]["payload"]["max_attempts"].Integer(), 4);
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::FALLBACK);
	ASSERT_TRUE(output.intent);
	EXPECT_NE(output.intent->find("No high-confidence scripted candidate remains"), std::string::npos);
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

TEST(LuaAdventureScriptRunnerTest, DefaultAdventureEndsTurnOnTradeOnlyNullkillerSlice)
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
			response["result"]["didWork"] = JsonNode(true);
			response["result"]["priorityTasksExecuted"] = JsonNode(0);
			response["result"]["adventureStepsExecuted"] = JsonNode(0);
			response["result"]["adventureReplanSteps"] = JsonNode(0);
			response["result"]["adventureStopTurnSteps"] = JsonNode(0);
			response["result"]["tradePasses"] = JsonNode(2);
			response["result"]["paused"] = JsonNode(false);
			response["result"]["stop"] = JsonNode(false);
		}
		return response;
	});

	ASSERT_EQ(commands.size(), 2);
	EXPECT_EQ(commands[0]["payload"]["type"].String(), "nullkiller_turn_slice");
	EXPECT_EQ(commands[1]["payload"]["type"].String(), "end_turn");
	EXPECT_EQ(output.status, AI::AdventureScriptStatus::END_TURN);
	ASSERT_TRUE(output.intent);
	EXPECT_NE(output.intent->find("only traded resources"), std::string::npos);
}

TEST(LuaAdventureScriptRunnerTest, PackagedConfigUsesFallbackControlScript)
{
	const JsonNode config = readJsonFile(std::filesystem::path(VCMI_SOURCE_DIR) / "config/ai/scriptedAdventure.json");

	ASSERT_TRUE(config["script"].isString());
	EXPECT_EQ(config["script"].String(), "ai/candidates/fallbackAdventure.lua");
	EXPECT_FALSE(hasField(config, "players"))
		<< "Experimental personality scripts should stay opt-in until they beat the fallback control.";
}
